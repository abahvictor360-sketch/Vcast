/*
Vcast - cast OBS to TVs and players on your local network
Copyright (C) 2026 Vcast contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "cast-manager.h"
#include "net-util.h"
#include "vcast-output.h"
#include "receiver/receiver-source.h"
#include "ui/vcast-dock.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <QMainWindow>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("Vcast.Description");
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return "Vcast";
}

static void frontend_event(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_EXIT) {
		/* Tell the TV to stop and free our output before OBS tears everything down. */
		vcast::CastManager::instance().shutdown();
	}
}

bool obs_module_load(void)
{
	if (!vcast::net_init()) {
		obs_log(LOG_ERROR, "could not initialise networking");
		return false;
	}

	vcast_register_output();
	vcast_register_receiver_source();
	vcast::CastManager::instance().settings().load();

	auto *main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	auto *dock = new VcastDock(main_window);
	obs_frontend_add_dock_by_id("vcast-dock", obs_module_text("Vcast.DockTitle"), dock);

	obs_frontend_add_event_callback(frontend_event, nullptr);

	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(frontend_event, nullptr);
	vcast::CastManager::instance().shutdown();
	vcast_receiver_shutdown();
	vcast::net_shutdown();
	obs_log(LOG_INFO, "plugin unloaded");
}
