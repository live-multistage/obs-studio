#include <obs-module.h>
#include <obs-frontend-api.h>
#include <browser-panel.hpp>
#include <obs-websocket-api.h>
#include <util/platform.h>

#include <QDockWidget>
#include <QMainWindow>
#include <QString>

#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("liveshow-dock", "en-US")

namespace {

constexpr const char *kDockId = "liveshow-broadcaster-dock";
constexpr const char *kDockTitle = "LiveShow Broadcaster";
constexpr const char *kDefaultBaseUrl = "http://localhost:3000";
constexpr const char *kObsWebsocketConfigFile = "config.json";
constexpr const char *kVendorName = "liveshow";
constexpr const char *kActiveContextConfigFile = "active-context.json";
constexpr const char *kCameraCanvasNamePrefix = "liveshow-camera-";
constexpr const char *kCameraSourcePrefix = "liveshow-capture-";
constexpr const char *kCameraSceneName = "scene";

#if defined(__APPLE__)
constexpr const char *kCameraCaptureSourceId = "macos-avcapture";
constexpr const char *kScreenCaptureSourceId = "display_capture";
#elif defined(_WIN32)
constexpr const char *kCameraCaptureSourceId = "dshow_input";
constexpr const char *kScreenCaptureSourceId = "monitor_capture";
#elif defined(__linux__)
constexpr const char *kCameraCaptureSourceId = "v4l2_input";
constexpr const char *kScreenCaptureSourceId = "pipewire-screen-capture-source";
#else
#error "Unsupported platform for liveshow-dock capture sources"
#endif

QCefWidget *dockWidget = nullptr;
obs_websocket_vendor vendor = nullptr;
std::map<std::string, obs_canvas_t *> cameraCanvases;
std::mutex cameraCanvasesMutex;

struct CameraOutput {
	obs_output_t *output;
	obs_encoder_t *videoEncoder;
	obs_encoder_t *audioEncoder;
	obs_service_t *service;
};

// Guarded by cameraCanvasesMutex (not a second mutex) — StartCameraOutput needs
// to read cameraCanvases and write cameraOutputs in the same critical section,
// and reusing the one existing lock avoids any two-mutex lock-ordering risk.
std::map<std::string, CameraOutput> cameraOutputs;

std::string EnvOrDefault(const char *name, const char *fallback)
{
	const char *value = std::getenv(name);
	if (value && *value)
		return std::string(value);
	return std::string(fallback);
}

// obs-websocket's GetConfig()/Config::ServerPassword are private symbols — confirmed
// via `nm -gU` on the built plugin binary (only qrcodegen and obs_module_* entry
// points are exported, nothing from obs-websocket's own classes). Read its persisted
// config.json directly instead, via public libobs file/module APIs.
std::string ReadObsWebsocketPassword()
{
	obs_module_t *wsModule = obs_get_module("obs-websocket");
	if (!wsModule) {
		blog(LOG_WARNING, "[liveshow-dock] obs-websocket module not found");
		return std::string();
	}

	char *path = obs_module_get_config_path(wsModule, kObsWebsocketConfigFile);
	if (!path)
		return std::string();

	obs_data_t *data = obs_data_create_from_json_file(path);
	bfree(path);

	if (!data) {
		blog(LOG_WARNING, "[liveshow-dock] obs-websocket config.json not found or invalid");
		return std::string();
	}

	std::string password = obs_data_get_string(data, "server_password");
	obs_data_release(data);
	return password;
}

// setStartupScript runs in CEF's OnLoadEnd (main frame load complete), which fires
// before the page's own JS has had a chance to attach a 'liveshow-obs-credentials'
// listener (e.g. a React useEffect that runs post-hydration). A bare dispatchEvent()
// is lost in that case — nothing was listening yet. Stash the payload on window as a
// durable value the page can read on its own schedule, and also dispatch the event as
// a fallback for the (unlikely) case a listener is already attached when this runs.
// The password is JSON-escaped via obs_data's own serializer (never hand-concatenate
// the raw password into a JS string literal — a password containing a quote or
// backslash would break the script or, worse, allow injection).
std::string BuildCredentialsScript(const std::string &password)
{
	obs_data_t *payload = obs_data_create();
	obs_data_set_string(payload, "password", password.c_str());
	const char *payloadJson = obs_data_get_json(payload);

	std::string script = "window.liveshowObsCredentials = ";
	script += payloadJson;
	script += "; window.dispatchEvent(new CustomEvent('liveshow-obs-credentials', { detail: ";
	script += payloadJson;
	script += " }));";

	obs_data_release(payload);
	return script;
}

void CreateDock()
{
	std::string password = ReadObsWebsocketPassword();
	if (password.empty()) {
		blog(LOG_WARNING, "[liveshow-dock] could not read obs-websocket password, dock not created");
		return;
	}

	QCef *cef = obs_browser_init_panel();
	if (!cef) {
		blog(LOG_WARNING, "[liveshow-dock] obs-browser not available, dock not created");
		return;
	}

	std::string baseUrl = EnvOrDefault("LIVESHOW_DOCK_BASE_URL", kDefaultBaseUrl);
	std::string token = EnvOrDefault("LIVESHOW_DOCK_TOKEN", "");
	if (token.empty()) {
		blog(LOG_WARNING, "[liveshow-dock] LIVESHOW_DOCK_TOKEN not set, dock not created");
		return;
	}

	std::string url = baseUrl + "/broadcaster-dock/" + token;

	dockWidget = cef->create_widget(nullptr, url);
	if (!dockWidget) {
		blog(LOG_WARNING, "[liveshow-dock] failed to create browser widget");
		return;
	}

	dockWidget->setStartupScript(BuildCredentialsScript(password));

	if (!obs_frontend_add_dock_by_id(kDockId, kDockTitle, dockWidget)) {
		blog(LOG_WARNING, "[liveshow-dock] failed to register dock (duplicate id?)");
		return;
	}

	// obs_frontend_add_dock_by_id() always registers the dock hidden and floating
	// (frontend/OBSStudioAPI.cpp: `dock->setVisible(false); dock->setFloating(true);`)
	// — that's the public API's own behavior, not something the caller can pass a flag
	// for. Reach back into the Qt tree via the main window and force it visible/docked
	// so it actually shows up on first launch, matching this screen's "visible by
	// default" requirement instead of requiring the user to dig it out of the Docks menu.
	if (QMainWindow *mainWindow = static_cast<QMainWindow *>(obs_frontend_get_main_window())) {
		if (QDockWidget *dock = mainWindow->findChild<QDockWidget *>(QString::fromUtf8(kDockId))) {
			dock->setFloating(false);
			dock->setVisible(true);
		}
	}

	blog(LOG_INFO, "[liveshow-dock] dock created at %s", url.c_str());
}

// obs-websocket vendor request callback signature: (request_data, response_data, priv_data).
// The caller (obs-websocket's RequestHandler) allocates response_data; we only fill it in.
// Reads are missing-safe: obs_data_get_string returns "" for an absent key, so a plugin
// with no prior SetActiveStream call yields an empty {} response rather than an error —
// the dock's own logic distinguishes "no selection yet" that way.
//
// The stored file only ever tracks the single most recent selection (still just one
// active-context.json), but it's now tagged with the userId that made it. A different
// user asking for the active stream must see "no selection yet", not someone else's
// event/stream (and, transitively, not risk a 403 fetching an event they don't own).
void HandleGetActiveStream(obs_data_t *request, obs_data_t *response, void *)
{
	const char *requestUserId = obs_data_get_string(request, "userId");
	if (!requestUserId || !*requestUserId)
		return;

	char *path = obs_module_config_path(kActiveContextConfigFile);
	if (!path)
		return;

	obs_data_t *stored = obs_data_create_from_json_file(path);
	bfree(path);
	if (!stored)
		return;

	const char *storedUserId = obs_data_get_string(stored, "userId");
	const char *eventId = obs_data_get_string(stored, "eventId");
	const char *streamId = obs_data_get_string(stored, "streamId");
	if (storedUserId && *storedUserId && strcmp(storedUserId, requestUserId) == 0 && eventId && *eventId &&
	    streamId && *streamId) {
		obs_data_set_string(response, "eventId", eventId);
		obs_data_set_string(response, "streamId", streamId);
	}
	obs_data_release(stored);
}

void HandleSetActiveStream(obs_data_t *request, obs_data_t *response, void *)
{
	const char *userId = obs_data_get_string(request, "userId");
	const char *eventId = obs_data_get_string(request, "eventId");
	const char *streamId = obs_data_get_string(request, "streamId");

	obs_data_t *toSave = obs_data_create();
	obs_data_set_string(toSave, "userId", userId);
	obs_data_set_string(toSave, "eventId", eventId);
	obs_data_set_string(toSave, "streamId", streamId);

	char *path = obs_module_config_path(kActiveContextConfigFile);
	if (path) {
		obs_data_save_json_safe(toSave, path, "tmp", "bak");
		bfree(path);
	}
	obs_data_release(toSave);

	obs_data_set_string(response, "userId", userId);
	obs_data_set_string(response, "eventId", eventId);
	obs_data_set_string(response, "streamId", streamId);
}

std::string CameraCanvasName(const std::string &cameraId)
{
	return std::string(kCameraCanvasNamePrefix) + cameraId;
}

std::string CameraSourceName(const std::string &cameraId)
{
	return std::string(kCameraSourcePrefix) + cameraId;
}

std::string CameraOutputName(const std::string &cameraId)
{
	return std::string("liveshow-output-") + cameraId;
}

bool CaptureFirstSceneItem(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	*static_cast<obs_sceneitem_t **>(param) = item;
	return false; // stop after the first (and only) item
}

// The scene is only ever expected to hold exactly one item (the attached
// capture source). Looking it up this way — rather than by the name passed
// to obs_source_create() — is deliberate: on a rapid re-attach, OBS's own
// name-dedup logic can silently rename the new source if the previous one's
// deferred destroy (obs_source_destroy_defer) hasn't completed yet, which
// would break a lookup keyed on the predicted name.
obs_source_t *FindAttachedCaptureSource(obs_scene_t *scene)
{
	obs_sceneitem_t *item = nullptr;
	obs_scene_enum_items(scene, CaptureFirstSceneItem, &item);
	return item ? obs_sceneitem_get_source(item) : nullptr;
}

// See this plan's Global Constraints: obs_canvas_create() returns a canvas
// with its weak-ref zero-initialized, and OBS's internal name/uuid lookup
// tables store raw pointers with no addref — being findable by name does not
// keep the object alive. This plugin holds the strong ref itself in
// cameraCanvases for as long as the canvas should exist, releasing only in
// HandleRemoveCameraCanvas.
void HandleCreateCameraCanvas(obs_data_t *request, obs_data_t *response, void *)
{
	const char *cameraId = obs_data_get_string(request, "cameraId");
	if (!cameraId || !*cameraId)
		return;

	std::lock_guard<std::mutex> lock(cameraCanvasesMutex);

	std::string id(cameraId);
	auto it = cameraCanvases.find(id);
	if (it != cameraCanvases.end()) {
		obs_data_set_string(response, "canvasName", obs_canvas_get_name(it->second));
		return;
	}

	struct obs_video_info ovi;
	if (!obs_get_video_info(&ovi)) {
		blog(LOG_WARNING, "[liveshow-dock] obs_get_video_info failed, cannot create canvas for camera %s", cameraId);
		return;
	}

	std::string name = CameraCanvasName(id);
	obs_canvas_t *canvas = obs_canvas_create(name.c_str(), &ovi, 0);
	if (!canvas) {
		blog(LOG_WARNING, "[liveshow-dock] obs_canvas_create failed for camera %s", cameraId);
		return;
	}

	cameraCanvases[id] = canvas;
	obs_data_set_string(response, "canvasName", name.c_str());
}

void HandleRemoveCameraCanvas(obs_data_t *request, obs_data_t *, void *)
{
	const char *cameraId = obs_data_get_string(request, "cameraId");
	if (!cameraId || !*cameraId)
		return;

	std::lock_guard<std::mutex> lock(cameraCanvasesMutex);

	auto it = cameraCanvases.find(cameraId);
	if (it == cameraCanvases.end())
		return;

	// obs_canvas_remove() only marks the canvas removed and fires a signal — it's
	// internally ref-neutral (get_ref then release, self-balanced). The plugin's
	// own release() is what actually drops the held ref and triggers destruction.
	obs_canvas_remove(it->second);
	obs_canvas_release(it->second);
	cameraCanvases.erase(it);
}

void HandleGetCameraCanvasStatus(obs_data_t *request, obs_data_t *response, void *)
{
	const char *cameraId = obs_data_get_string(request, "cameraId");

	std::lock_guard<std::mutex> lock(cameraCanvasesMutex);
	bool exists = cameraId && *cameraId && cameraCanvases.count(cameraId) > 0;
	obs_data_set_bool(response, "exists", exists);
}

// Ref-counting note (opposite of the bare-canvas case above, verified against
// obs-scene.c/obs-canvas.c): obs_scene_add() and obs_canvas_set_channel() both
// call obs_source_get_ref() on what's handed to them, so the container takes
// its own ownership. Standard pattern applies here: create your own ref, add
// it to a container that addrefs, release your own ref right after.
void HandleAttachCameraSource(obs_data_t *request, obs_data_t *response, void *)
{
	const char *cameraId = obs_data_get_string(request, "cameraId");
	const char *sourceType = obs_data_get_string(request, "sourceType");
	if (!cameraId || !*cameraId || !sourceType || !*sourceType)
		return;

	const char *sourceId = nullptr;
	if (strcmp(sourceType, "camera") == 0)
		sourceId = kCameraCaptureSourceId;
	else if (strcmp(sourceType, "screen") == 0)
		sourceId = kScreenCaptureSourceId;
	else
		return;

	// Held for the entire function body, not just the map lookup: obs-websocket
	// dispatches vendor requests on a QThreadPool, so releasing the lock after
	// copying out `canvas` would let a concurrent RemoveCameraCanvas destroy it
	// out from under the obs_canvas_*/obs_source_* calls below.
	std::lock_guard<std::mutex> lock(cameraCanvasesMutex);
	auto it = cameraCanvases.find(cameraId);
	if (it == cameraCanvases.end()) {
		blog(LOG_WARNING, "[liveshow-dock] AttachCameraSource: no canvas for camera %s", cameraId);
		return;
	}
	obs_canvas_t *canvas = it->second;

	// Replace whatever scene/source was previously attached, if any. Clearing
	// the channel first releases the channel's own held ref (confirmed in
	// obs_canvas_set_channel's implementation: it releases prev_source when
	// replacing); obs_source_remove()+release() below then drops our lookup
	// ref, and standard scene teardown releases its contained scene item's
	// source ref as part of normal destruction.
	obs_scene_t *existingScene = obs_canvas_get_scene_by_name(canvas, kCameraSceneName);
	if (existingScene) {
		obs_canvas_set_channel(canvas, 0, nullptr);
		obs_source_t *existingSceneSource = obs_scene_get_source(existingScene);
		obs_source_remove(existingSceneSource);
		obs_source_release(existingSceneSource);
	}

	std::string sourceName = CameraSourceName(cameraId);
	obs_source_t *source = obs_source_create(sourceId, sourceName.c_str(), nullptr, nullptr);
	if (!source) {
		blog(LOG_WARNING, "[liveshow-dock] obs_source_create(%s) failed for camera %s", sourceId, cameraId);
		return;
	}

	obs_scene_t *scene = obs_canvas_scene_create(canvas, kCameraSceneName);
	if (!scene) {
		blog(LOG_WARNING, "[liveshow-dock] obs_canvas_scene_create failed for camera %s", cameraId);
		obs_source_release(source);
		return;
	}

	obs_scene_add(scene, source);
	obs_source_release(source);

	obs_source_t *sceneSource = obs_scene_get_source(scene);
	obs_canvas_set_channel(canvas, 0, sceneSource);
	obs_source_release(sceneSource);

	obs_data_set_string(response, "sourceType", sourceType);
}

void HandleOpenCameraSourceProperties(obs_data_t *request, obs_data_t *, void *)
{
	const char *cameraId = obs_data_get_string(request, "cameraId");
	if (!cameraId || !*cameraId)
		return;

	// Held for the entire function body — see HandleAttachCameraSource for why.
	std::lock_guard<std::mutex> lock(cameraCanvasesMutex);
	auto it = cameraCanvases.find(cameraId);
	if (it == cameraCanvases.end())
		return;
	obs_canvas_t *canvas = it->second;

	obs_scene_t *scene = obs_canvas_get_scene_by_name(canvas, kCameraSceneName);
	if (!scene)
		return;

	obs_source_t *source = FindAttachedCaptureSource(scene);
	if (source)
		obs_frontend_open_source_properties(source);

	obs_source_release(obs_scene_get_source(scene));
}

void HandleGetCameraSourceStatus(obs_data_t *request, obs_data_t *response, void *)
{
	const char *cameraId = obs_data_get_string(request, "cameraId");
	if (!cameraId || !*cameraId)
		return;

	// Held for the entire function body — see HandleAttachCameraSource for why.
	std::lock_guard<std::mutex> lock(cameraCanvasesMutex);
	auto it = cameraCanvases.find(cameraId);
	if (it == cameraCanvases.end())
		return;
	obs_canvas_t *canvas = it->second;

	obs_scene_t *scene = obs_canvas_get_scene_by_name(canvas, kCameraSceneName);
	if (!scene)
		return;

	obs_source_t *source = FindAttachedCaptureSource(scene);
	if (source) {
		const char *sourceId = obs_source_get_id(source);
		obs_data_set_bool(response, "attached", true);
		if (strcmp(sourceId, kCameraCaptureSourceId) == 0)
			obs_data_set_string(response, "sourceType", "camera");
		else if (strcmp(sourceId, kScreenCaptureSourceId) == 0)
			obs_data_set_string(response, "sourceType", "screen");
	}

	obs_source_release(obs_scene_get_source(scene));
}

// Ref-counting note: obs_output_create()/obs_video_encoder_create()/
// obs_audio_encoder_create()/obs_service_create() all zero-init their ref
// exactly like Phase 1's bare canvas — none of these four objects are ever
// added to an addrefing container (unlike Phase 2's sources, which scenes
// addref), so the plugin is the sole owner of all four and must hold and
// release all four itself.
void HandleStartCameraOutput(obs_data_t *request, obs_data_t *response, void *)
{
	const char *cameraId = obs_data_get_string(request, "cameraId");
	const char *url = obs_data_get_string(request, "url");
	const char *streamId = obs_data_get_string(request, "streamId");
	const char *streamKey = obs_data_get_string(request, "streamKey");
	if (!cameraId || !*cameraId || !url || !*url || !streamId || !*streamId)
		return;

	std::lock_guard<std::mutex> lock(cameraCanvasesMutex);

	if (cameraOutputs.count(cameraId) > 0) {
		obs_data_set_bool(response, "active", true);
		return;
	}

	auto canvasIt = cameraCanvases.find(cameraId);
	if (canvasIt == cameraCanvases.end()) {
		blog(LOG_WARNING, "[liveshow-dock] StartCameraOutput: no canvas for camera %s", cameraId);
		return;
	}
	obs_canvas_t *canvas = canvasIt->second;

	std::string baseName = CameraOutputName(cameraId);

	obs_data_t *videoSettings = obs_data_create();
	obs_data_set_int(videoSettings, "bitrate", 4000);
	std::string videoEncoderName = baseName + "-video";
	obs_encoder_t *videoEncoder = obs_video_encoder_create("obs_x264", videoEncoderName.c_str(), videoSettings, nullptr);
	obs_data_release(videoSettings);
	if (!videoEncoder) {
		blog(LOG_WARNING, "[liveshow-dock] StartCameraOutput: video encoder create failed for camera %s", cameraId);
		return;
	}
	obs_encoder_set_video(videoEncoder, obs_canvas_get_video(canvas));

	obs_data_t *audioSettings = obs_data_create();
	obs_data_set_int(audioSettings, "bitrate", 128);
	std::string audioEncoderName = baseName + "-audio";
	obs_encoder_t *audioEncoder = obs_audio_encoder_create("ffmpeg_aac", audioEncoderName.c_str(), audioSettings, 0, nullptr);
	obs_data_release(audioSettings);
	if (!audioEncoder) {
		blog(LOG_WARNING, "[liveshow-dock] StartCameraOutput: audio encoder create failed for camera %s", cameraId);
		obs_encoder_release(videoEncoder);
		return;
	}
	obs_encoder_set_audio(audioEncoder, obs_get_audio());

	obs_data_t *serviceSettings = obs_data_create();
	obs_data_set_string(serviceSettings, "server", url);
	obs_data_set_string(serviceSettings, "key", streamId);
	obs_data_set_string(serviceSettings, "password", streamKey);
	std::string serviceName = baseName + "-service";
	obs_service_t *service = obs_service_create("rtmp_custom", serviceName.c_str(), serviceSettings, nullptr);
	obs_data_release(serviceSettings);
	if (!service) {
		blog(LOG_WARNING, "[liveshow-dock] StartCameraOutput: service create failed for camera %s", cameraId);
		obs_encoder_release(videoEncoder);
		obs_encoder_release(audioEncoder);
		return;
	}

	std::string outputName = baseName + "-output";
	obs_output_t *output = obs_output_create("ffmpeg_mpegts_muxer", outputName.c_str(), nullptr, nullptr);
	if (!output) {
		blog(LOG_WARNING, "[liveshow-dock] StartCameraOutput: output create failed for camera %s", cameraId);
		obs_encoder_release(videoEncoder);
		obs_encoder_release(audioEncoder);
		obs_service_release(service);
		return;
	}

	obs_output_set_video_encoder(output, videoEncoder);
	obs_output_set_audio_encoder(output, audioEncoder, 0);
	obs_output_set_service(output, service);

	if (!obs_output_start(output)) {
		const char *err = obs_output_get_last_error(output);
		blog(LOG_WARNING, "[liveshow-dock] StartCameraOutput: obs_output_start failed for camera %s: %s", cameraId,
		     (err && *err) ? err : "(no error message)");
		if (err && *err)
			obs_data_set_string(response, "error", err);
		obs_output_release(output);
		obs_encoder_release(videoEncoder);
		obs_encoder_release(audioEncoder);
		obs_service_release(service);
		return;
	}

	cameraOutputs[cameraId] = CameraOutput{output, videoEncoder, audioEncoder, service};
	obs_data_set_bool(response, "active", true);
}

void HandleStopCameraOutput(obs_data_t *request, obs_data_t *, void *)
{
	const char *cameraId = obs_data_get_string(request, "cameraId");
	if (!cameraId || !*cameraId)
		return;

	std::lock_guard<std::mutex> lock(cameraCanvasesMutex);

	auto it = cameraOutputs.find(cameraId);
	if (it == cameraOutputs.end())
		return;

	// obs_output_stop()'s type-specific teardown (output->info.stop, invoked from
	// obs_output_actual_stop) runs synchronously within this call — confirmed by
	// reading obs-output.c directly. Releasing our held refs right after matches
	// the same lifecycle sequence OBS's own frontend uses for any output.
	obs_output_stop(it->second.output);
	obs_output_release(it->second.output);
	obs_encoder_release(it->second.videoEncoder);
	obs_encoder_release(it->second.audioEncoder);
	obs_service_release(it->second.service);
	cameraOutputs.erase(it);
}

void HandleGetCameraOutputStatus(obs_data_t *request, obs_data_t *response, void *)
{
	const char *cameraId = obs_data_get_string(request, "cameraId");
	if (!cameraId || !*cameraId)
		return;

	std::lock_guard<std::mutex> lock(cameraCanvasesMutex);
	auto it = cameraOutputs.find(cameraId);
	obs_data_set_bool(response, "active", it != cameraOutputs.end() && obs_output_active(it->second.output));
}

// Per obs-websocket-api.h: "ALWAYS CALL ONLY VIA obs_module_post_load() CALLBACK!" — same
// lifecycle CreateDock() already relies on, since obs-websocket's own obs_module_load()
// (which sets up the proc handler this all rides on) is guaranteed to have run by then.
void RegisterVendorRequests()
{
	// obs_module_config_path() only builds the path string, it doesn't create the
	// directory — unlike obs-websocket's own config dir (already created by its own
	// first settings save), this plugin's config dir doesn't exist yet on a fresh
	// install, so the first obs_data_save_json_safe() would silently fail without this.
	char *configDir = obs_module_config_path("");
	if (configDir) {
		os_mkdirs(configDir);
		bfree(configDir);
	}

	vendor = obs_websocket_register_vendor(kVendorName);
	if (!vendor) {
		blog(LOG_WARNING, "[liveshow-dock] obs-websocket vendor registration failed (obs-websocket not loaded?)");
		return;
	}

	obs_websocket_vendor_register_request(vendor, "GetActiveStream", HandleGetActiveStream, nullptr);
	obs_websocket_vendor_register_request(vendor, "SetActiveStream", HandleSetActiveStream, nullptr);
	obs_websocket_vendor_register_request(vendor, "CreateCameraCanvas", HandleCreateCameraCanvas, nullptr);
	obs_websocket_vendor_register_request(vendor, "RemoveCameraCanvas", HandleRemoveCameraCanvas, nullptr);
	obs_websocket_vendor_register_request(vendor, "GetCameraCanvasStatus", HandleGetCameraCanvasStatus, nullptr);
	obs_websocket_vendor_register_request(vendor, "AttachCameraSource", HandleAttachCameraSource, nullptr);
	obs_websocket_vendor_register_request(vendor, "OpenCameraSourceProperties", HandleOpenCameraSourceProperties, nullptr);
	obs_websocket_vendor_register_request(vendor, "GetCameraSourceStatus", HandleGetCameraSourceStatus, nullptr);
	obs_websocket_vendor_register_request(vendor, "StartCameraOutput", HandleStartCameraOutput, nullptr);
	obs_websocket_vendor_register_request(vendor, "StopCameraOutput", HandleStopCameraOutput, nullptr);
	obs_websocket_vendor_register_request(vendor, "GetCameraOutputStatus", HandleGetCameraOutputStatus, nullptr);
}

} // namespace

bool obs_module_load(void)
{
	return true;
}

void obs_module_unload(void) {}

void obs_module_post_load(void)
{
	CreateDock();
	RegisterVendorRequests();
}
