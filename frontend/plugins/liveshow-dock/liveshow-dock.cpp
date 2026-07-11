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

QCefWidget *dockWidget = nullptr;
obs_websocket_vendor vendor = nullptr;

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
