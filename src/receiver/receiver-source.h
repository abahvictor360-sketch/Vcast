/*
Vcast - cast to and from OBS on your local network
Copyright (C) 2026 Vcast contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#pragma once

#include <cstdint>
#include <string>

#include <obs.h>

#define VCAST_RECEIVER_SOURCE_ID "vcast_receiver"

/*
 * "Vcast Screen Receiver" source: phones, tablets and laptops cast their
 * screen into it, the way they would cast to a smart TV.
 *   - iPhone / iPad / Mac: AirPlay Screen Mirroring
 *   - Android: the Vcast app
 *   - Any laptop: the cast web page (Chrome, Edge, Firefox, Safari)
 */
void vcast_register_receiver_source();

/* Stops shared receiver services (web server, discovery). Call on module unload. */
void vcast_receiver_shutdown();

/* https://<lan-ip>:<port>/ of the cast page, or empty if no receiver exists. */
std::string vcast_receiver_web_url();

/*
 * Makes receiver scene items fill the canvas, keeping the picture's aspect
 * ratio (a rotating phone keeps fitting). only: limit to one source (NULL =
 * every receiver). only_new: skip items already fitted once, so manual
 * adjustments are kept. Returns the number of items changed. UI thread only.
 */
int vcast_receiver_fit_to_screen(obs_source_t *only, bool only_new);

/* Name of the default receiver ("Vcast (<computer name>)"). */
std::string vcast_receiver_default_name();
