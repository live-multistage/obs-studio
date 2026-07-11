#include <obs-module.h>
#include <obs-frontend-api.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("liveshow-dock", "en-US")

bool obs_module_load(void)
{
	return true;
}

void obs_module_unload(void) {}

void obs_module_post_load(void)
{
	blog(LOG_INFO, "[liveshow-dock] loaded");
}
