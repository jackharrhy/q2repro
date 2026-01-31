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

/* NOTE(notscared) Voice chat subsystem using Opus codec */

#ifndef CLIENT_VOICE_H
#define CLIENT_VOICE_H

#include "shared/shared.h"

/* Voice subsystem configuration */
#define VOICE_SAMPLE_RATE       24000   /* 24kHz - good quality for voice */
#define VOICE_CHANNELS          1       /* Mono - stereo handled at playback */
#define VOICE_FRAME_MS          20      /* 20ms frames - Opus preferred */
#define VOICE_FRAME_SAMPLES     (VOICE_SAMPLE_RATE * VOICE_FRAME_MS / 1000)  /* 480 samples */
#define VOICE_MAX_FRAME_BYTES   1000    /* Max encoded opus frame size */
#define VOICE_MAX_FRAMES_BUNDLE 6       /* Bundle up to 6 frames per packet (120ms) */
#define VOICE_BITRATE           24000   /* 24 kbps - good voice quality */

/* Voice activation */
#define VOICE_DEFAULT_THRESHOLD 0.02f   /* RMS threshold for voice detection */
#define VOICE_DEFAULT_HANGTIME  300     /* ms to keep transmitting after voice stops */

/* Proximity */
#define VOICE_DEFAULT_DISTANCE      1024.0f /* Max hearing distance in units */
#define VOICE_DEFAULT_DISTANCE_MIN  64.0f   /* Distance for full volume */

/* Initialize voice subsystem (called from client init) */
bool Voice_Init(void);

/* Shutdown voice subsystem */
void Voice_Shutdown(void);

/* Called each frame to capture and process microphone input */
void Voice_Capture(void);

/* Check if voice is currently active (transmitting) */
bool Voice_IsActive(void);

/* Encode PCM samples to Opus
 * Returns number of bytes written to out_data, or -1 on error */
int Voice_Encode(const int16_t *pcm, int frame_samples, 
                 uint8_t *out_data, int max_out_bytes);

/* Decode Opus data to PCM samples
 * Returns number of samples decoded, or -1 on error */
int Voice_Decode(const uint8_t *opus_data, int opus_len,
                 int16_t *out_pcm, int max_out_samples);

/* Loopback test - play back captured audio locally */
void Voice_Loopback(bool enable);

/* Get current voice cvars for external use */
float Voice_GetThreshold(void);
int Voice_GetHangtime(void);

/* ========================================================================= */
/* Network functions                                                         */
/* ========================================================================= */

/* Write pending voice data to message buffer for transmission.
 * Call this after writing move commands, before Netchan_Transmit.
 * Returns number of bytes written, or 0 if no voice data pending. */
int Voice_WritePacket(void);

/* Process received voice packet from server (svc_voice).
 * Reads from current message position. */
void Voice_ParseServerPacket(void);

/* ========================================================================= */
/* Remote voice playback (Phase 4)                                           */
/* ========================================================================= */

#define VOICE_MAX_CLIENTS       256     /* Max concurrent voice sources */

/* Called when receiving voice from another player.
 * sender: client slot number
 * volume: 0-255 pre-calculated volume from server
 * pan: -128 to 127 stereo pan
 * opus_data: encoded Opus frames
 * opus_len: total length of opus data */
void Voice_ReceiveFrom(int sender, uint8_t volume, int8_t pan,
                       const uint8_t *opus_data, int opus_len,
                       const uint8_t *frame_lens, int frame_count);

#endif /* CLIENT_VOICE_H */
