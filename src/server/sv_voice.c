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

/* NOTE(notscared) Server-side voice chat - receive from clients, relay to nearby players */

#include "server.h"
#include "server/sv_voice.h"
#include "common/protocol.h"

#include <math.h>

/* ========================================================================= */
/* CVars                                                                     */
/* ========================================================================= */

cvar_t *sv_voip;
cvar_t *sv_voip_distance;

/* Proximity settings */
#define VOICE_MIN_DISTANCE      64.0f   /* Full volume within this range */
#define VOICE_DEFAULT_MAX_DIST  1024.0f /* Default max hearing distance */

/* ========================================================================= */
/* Helper functions                                                          */
/* ========================================================================= */

/*
 * Calculate voice volume based on distance between two players
 * Returns 0-255, where 255 is full volume
 */
static uint8_t SV_Voice_CalculateVolume(const vec3_t listener_origin, 
                                        const vec3_t speaker_origin,
                                        float max_distance)
{
    vec3_t delta;
    VectorSubtract(speaker_origin, listener_origin, delta);
    float distance = VectorLength(delta);

    /* Inside full volume radius */
    if (distance < VOICE_MIN_DISTANCE)
        return 255;

    /* Beyond max distance - silent */
    if (distance > max_distance)
        return 0;

    /* Inverse square falloff */
    float range = max_distance - VOICE_MIN_DISTANCE;
    float normalized = (distance - VOICE_MIN_DISTANCE) / range;
    
    /* Quadratic falloff: (1 - normalized)^2 */
    float volume = (1.0f - normalized) * (1.0f - normalized);
    
    return (uint8_t)(volume * 255.0f);
}

/*
 * Calculate stereo pan based on angle from listener to speaker
 * Returns -128 (full left) to +127 (full right)
 */
static int8_t SV_Voice_CalculatePan(const edict_t *listener, const edict_t *speaker)
{
    if (!listener || !listener->client || !speaker)
        return 0;

    vec3_t dir, forward, right, up;
    
    /* Direction from listener to speaker */
    VectorSubtract(speaker->s.origin, listener->s.origin, dir);
    VectorNormalize(dir);
    
    /* Listener's orientation - use view angles */
    AngleVectors(listener->client->ps.viewangles, forward, right, up);
    
    /* Dot product with right vector = pan amount */
    float pan = DotProduct(dir, right);
    
    /* Clamp and scale to -128..127 range */
    if (pan > 1.0f) pan = 1.0f;
    if (pan < -1.0f) pan = -1.0f;
    
    return (int8_t)(pan * 127.0f);
}

/* ========================================================================= */
/* Voice processing                                                          */
/* ========================================================================= */

bool SV_Voice_CheckRemaining(void)
{
    /* Check if there's remaining data and if it's a voice command */
    if (msg_read.readcount >= msg_read.cursize)
        return false;

    /* Peek at the next byte without consuming it */
    uint8_t cmd = msg_read.data[msg_read.readcount];
    return (cmd == clc_voice);
}

void SV_Voice_ParseClient(client_t *speaker)
{
    if (!sv_voip || !sv_voip->integer) {
        Com_DPrintf("SV_Voice: voice disabled, dropping packet\n");
        /* Skip the voice data */
        while (msg_read.readcount < msg_read.cursize)
            MSG_ReadByte();
        return;
    }

    /* Read command byte (already peeked, but read to advance position) */
    int cmd = MSG_ReadByte();
    if (cmd != clc_voice) {
        Com_WPrintf("SV_Voice: expected clc_voice, got %d\n", cmd);
        return;
    }

    /* Read frame count */
    int frame_count = MSG_ReadByte();
    if (frame_count <= 0 || frame_count > SV_VOICE_MAX_FRAMES) {
        Com_DPrintf("SV_Voice: invalid frame_count %d\n", frame_count);
        return;
    }

    /* Read frame lengths */
    uint8_t frame_lens[SV_VOICE_MAX_FRAMES];
    int total_data_len = 0;
    for (int i = 0; i < frame_count; i++) {
        frame_lens[i] = MSG_ReadByte();
        if (frame_lens[i] > SV_VOICE_MAX_FRAME_SIZE) {
            Com_DPrintf("SV_Voice: frame %d too large (%d)\n", i, frame_lens[i]);
            return;
        }
        total_data_len += frame_lens[i];
    }

    /* Read frame data */
    uint8_t opus_data[SV_VOICE_MAX_PACKET];
    if (total_data_len > sizeof(opus_data)) {
        Com_DPrintf("SV_Voice: total data too large (%d)\n", total_data_len);
        return;
    }

    byte *data = MSG_ReadData(total_data_len);
    if (!data) {
        Com_DPrintf("SV_Voice: failed to read opus data\n");
        return;
    }
    memcpy(opus_data, data, total_data_len);

    Com_DDPrintf("SV_Voice: received %d frames (%d bytes) from client %d\n",
                 frame_count, total_data_len, speaker->number);

    /* Get speaker position */
    if (!speaker->edict) {
        Com_DPrintf("SV_Voice: speaker has no edict\n");
        return;
    }
    const vec3_t *speaker_origin = &speaker->edict->s.origin;

    float max_distance = sv_voip_distance ? sv_voip_distance->value : VOICE_DEFAULT_MAX_DIST;

    /* Relay to all nearby players */
    client_t *listener;
    FOR_EACH_CLIENT(listener) {
        /* Skip inactive clients */
        if (listener->state != cs_spawned)
            continue;

        /* Don't send to self */
        if (listener == speaker)
            continue;

        /* Check distance */
        if (!listener->edict)
            continue;

        uint8_t volume = SV_Voice_CalculateVolume(listener->edict->s.origin,
                                                  *speaker_origin,
                                                  max_distance);

        /* Skip if too far away */
        if (volume == 0)
            continue;

        int8_t pan = SV_Voice_CalculatePan(listener->edict, speaker->edict);

        /* Build voice packet with custom header (bypasses q2proto) */
        /* Format: VOIP_MAGIC (4) + sender (1) + volume (1) + pan (1) + 
         *         frame_count (1) + frame_lens (N) + opus_data (M) */
        uint8_t packet[SV_VOICE_MAX_PACKET + 16];
        int packet_len = 0;

        /* Write VOIP magic header: 0xFFFFFFFF + "voip" to mark as OOB voice */
        packet[packet_len++] = 0xFF;
        packet[packet_len++] = 0xFF;
        packet[packet_len++] = 0xFF;
        packet[packet_len++] = 0xFF;
        packet[packet_len++] = 'v';
        packet[packet_len++] = 'o';
        packet[packet_len++] = 'i';
        packet[packet_len++] = 'p';

        /* Voice data */
        packet[packet_len++] = (uint8_t)speaker->number;
        packet[packet_len++] = volume;
        packet[packet_len++] = (uint8_t)(pan & 0xFF);
        packet[packet_len++] = (uint8_t)frame_count;

        for (int i = 0; i < frame_count; i++) {
            packet[packet_len++] = frame_lens[i];
        }

        memcpy(packet + packet_len, opus_data, total_data_len);
        packet_len += total_data_len;

        /* Send directly to client (or via proxy) */
        if (listener->proxy_client) {
            SV_ProxySendToClient(listener, packet, packet_len);
        } else {
            NET_SendPacket(NS_SERVER, packet, packet_len, &listener->netchan.remote_address);
        }

        Com_DDPrintf("SV_Voice: relayed to client %d (vol=%d, pan=%d, %d bytes)\n",
                     listener->number, volume, pan, packet_len);
    }
}

/* ========================================================================= */
/* Init / Shutdown                                                           */
/* ========================================================================= */

void SV_Voice_Init(void)
{
    sv_voip = Cvar_Get("sv_voip", "1", 0);
    sv_voip_distance = Cvar_Get("sv_voip_distance", "1024", 0);

    Com_Printf("Server voice chat initialized\n");
}

void SV_Voice_Shutdown(void)
{
    Com_Printf("Server voice chat shutdown\n");
}
