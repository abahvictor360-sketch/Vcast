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

#define VCAST_MIRACAST_SOURCE_ID "vcast_miracast"

/*
 * "Vcast Win+K Receiver" source (Windows only): laptops cast to OBS with
 * Win + K, and Android phones with Cast / Smart View (Miracast).
 *
 * Windows only lets packaged Store apps use its Miracast receiver API, so this
 * source drives the receiver Windows already ships ("Wireless Display" /
 * "Connect"): it starts it, keeps it behind OBS, captures its window and shows
 * the cast scaled to fit a fixed picture size (1920 x 1080 by default), with
 * the receiver's sound.
 */
void vcast_register_miracast_source();
