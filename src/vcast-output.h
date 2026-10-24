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

#pragma once

#include <obs-module.h>

#include <cstddef>
#include <cstdint>

#define VCAST_OUTPUT_ID "vcast_output"

/*
 * "vcast_output": an encoded OBS output (H.264 + AAC) that muxes into MPEG-TS
 * and serves it over HTTP on the local network.
 *
 * Settings: "port" (int, first port to try), "port_attempts" (int).
 */
void vcast_register_output();

/* Port the running output is listening on (0 if not running). */
uint16_t vcast_output_port(obs_output_t *output);

/* Number of devices currently pulling the stream. */
size_t vcast_output_client_count(obs_output_t *output);
