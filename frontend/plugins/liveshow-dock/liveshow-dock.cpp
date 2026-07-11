#include <obs-module.h>
#include <obs-frontend-api.h>
#include <browser-panel.hpp>

#include <QDockWidget>
#include <QMainWindow>
#include <QString>

#include <cstdlib>
#include <string>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("liveshow-dock", "en-US")

namespace {

constexpr const char *kDockId = "liveshow-broadcaster-dock";
constexpr const char *kDockTitle = "LiveShow Broadcaster";
constexpr const char *kDefaultBaseUrl = "http://localhost:3000";
constexpr const char *kObsWebsocketConfigFile = "config.json";

QCefWidget *dockWidget = nullptr;

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

// Builds `window.dispatchEvent(new CustomEvent('liveshow-obs-credentials', { detail: {...} }))`
// with the password JSON-escaped via obs_data's own serializer (never hand-concatenate
// the raw password into a JS string literal — a password containing a quote or
// backslash would break the script or, worse, allow injection).
std::string BuildCredentialsScript(const std::string &password)
{
	obs_data_t *payload = obs_data_create();
	obs_data_set_string(payload, "password", password.c_str());
	const char *payloadJson = obs_data_get_json(payload);

	std::string script = "window.dispatchEvent(new CustomEvent('liveshow-obs-credentials', { detail: ";
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

} // namespace

bool obs_module_load(void)
{
	return true;
}

void obs_module_unload(void) {}

void obs_module_post_load(void)
{
	CreateDock();
}
