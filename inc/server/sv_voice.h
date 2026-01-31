/*
Copyright (C) 2026 notscared contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

/* NOTE(notscared) Server-side voice chat handling */

#ifndef SERVER_VOICE_H
#define SERVER_VOICE_H

#include "shared/shared.h"

/* Forward declarations */
struct client_s;

/* Voice configuration */
#define SV_VOICE_MAX_FRAMES     6       /* Max frames per packet */
#define SV_VOICE_MAX_FRAME_SIZE 255     /* Max bytes per frame (fits in uint8_t) */
#define SV_VOICE_MAX_PACKET     (1 + 1 + SV_VOICE_MAX_FRAMES + SV_VOICE_MAX_FRAMES * SV_VOICE_MAX_FRAME_SIZE)

/* Server voice CVars */
extern cvar_t *sv_voip;             /* 0=disabled, 1=enabled */
extern cvar_t *sv_voip_distance;    /* Max hearing distance in units */

/* Initialize server voice subsystem */
void SV_Voice_Init(void);

/* Shutdown server voice subsystem */
void SV_Voice_Shutdown(void);

/* Parse clc_voice from client and relay to nearby players.
 * Called from SV_ExecuteClientMessage when voice data is detected. */
void SV_Voice_ParseClient(struct client_s *client);

/* Check if there's voice data remaining in the message buffer */
bool SV_Voice_CheckRemaining(void);

#endif /* SERVER_VOICE_H */
