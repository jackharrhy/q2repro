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

/* NOTE(notscared) Voice chat implementation using Opus codec and OpenAL capture */

#include "shared/shared.h"
#include "system/system.h"
#include "common/cvar.h"
#include "common/common.h"
#include "common/zone.h"
#include "common/cmd.h"
#include "common/msg.h"
#include "common/protocol.h"
#include "client/voice.h"

#include <opus.h>
#include <AL/al.h>
#include <AL/alc.h>

#include <math.h>

/* ========================================================================= */
/* CVars                                                                     */
/* ========================================================================= */

static cvar_t *cl_voip;             /* 0=disabled, 1=receive only, 2=send+receive */
static cvar_t *cl_voip_threshold;   /* Voice activation threshold (0.0-1.0) */
static cvar_t *cl_voip_hangtime;    /* ms to keep transmitting after voice stops */
static cvar_t *cl_voip_gain;        /* Microphone input gain multiplier */
static cvar_t *cl_voip_loopback;    /* Loopback test mode */

/* ========================================================================= */
/* Opus state                                                                */
/* ========================================================================= */

static OpusEncoder *voice_encoder;
static OpusDecoder *voice_decoder;

/* ========================================================================= */
/* OpenAL capture state                                                      */
/* ========================================================================= */

static ALCdevice *capture_device;
static bool capture_started;

/* Capture buffer - holds raw PCM samples from microphone */
#define CAPTURE_BUFFER_SAMPLES  (VOICE_SAMPLE_RATE / 2)  /* 500ms buffer */
static int16_t capture_buffer[CAPTURE_BUFFER_SAMPLES];

/* ========================================================================= */
/* Voice activation state                                                    */
/* ========================================================================= */

static bool voice_active;           /* Currently transmitting */
static int voice_hangtime_remaining; /* ms until we stop transmitting */
static int64_t voice_last_frame_time;

/* ========================================================================= */
/* Loopback state                                                            */
/* ========================================================================= */

static bool loopback_enabled;
static ALuint loopback_source;
static ALuint loopback_buffers[4];
static int loopback_buf_index;

/* ========================================================================= */
/* Network transmission queue                                                */
/* ========================================================================= */

/* Queue of encoded frames waiting to be sent */
typedef struct {
    uint8_t data[VOICE_MAX_FRAME_BYTES];
    int len;
} voice_frame_t;

static voice_frame_t voice_queue[VOICE_MAX_FRAMES_BUNDLE];
static int voice_queue_count;

/* ========================================================================= */
/* Remote voice playback state                                               */
/* ========================================================================= */

/* Per-client voice playback state */
typedef struct {
    bool active;
    ALuint source;
    ALuint buffers[8];          /* Ring buffer of OpenAL buffers */
    int buf_write_index;
    int buf_read_index;
    OpusDecoder *decoder;       /* Separate decoder per client */
    int64_t last_recv_time;
} voice_client_t;

static voice_client_t voice_clients[VOICE_MAX_CLIENTS];

/* ========================================================================= */
/* Helper functions                                                          */
/* ========================================================================= */

/*
 * Calculate RMS (root mean square) amplitude of PCM samples
 * Returns a value between 0.0 and 1.0
 */
static float Voice_CalculateRMS(const int16_t *samples, int count)
{
    if (count <= 0)
        return 0.0f;

    float sum = 0.0f;
    for (int i = 0; i < count; i++) {
        float sample = samples[i] / 32768.0f;  /* Normalize to -1.0 to 1.0 */
        sum += sample * sample;
    }

    return sqrtf(sum / count);
}

/*
 * Apply gain to PCM samples in-place
 */
static void Voice_ApplyGain(int16_t *samples, int count, float gain)
{
    if (gain == 1.0f)
        return;

    for (int i = 0; i < count; i++) {
        int32_t sample = (int32_t)(samples[i] * gain);
        /* Clamp to int16 range */
        if (sample > 32767) sample = 32767;
        if (sample < -32768) sample = -32768;
        samples[i] = (int16_t)sample;
    }
}

/* ========================================================================= */
/* Opus encode/decode                                                        */
/* ========================================================================= */

int Voice_Encode(const int16_t *pcm, int frame_samples, 
                 uint8_t *out_data, int max_out_bytes)
{
    if (!voice_encoder)
        return -1;

    int encoded = opus_encode(voice_encoder, pcm, frame_samples, 
                              out_data, max_out_bytes);
    if (encoded < 0) {
        Com_DPrintf("Voice_Encode: opus_encode failed: %s\n", 
                    opus_strerror(encoded));
        return -1;
    }

    return encoded;
}

int Voice_Decode(const uint8_t *opus_data, int opus_len,
                 int16_t *out_pcm, int max_out_samples)
{
    if (!voice_decoder)
        return -1;

    int decoded = opus_decode(voice_decoder, opus_data, opus_len,
                              out_pcm, max_out_samples, 0);
    if (decoded < 0) {
        Com_DPrintf("Voice_Decode: opus_decode failed: %s\n",
                    opus_strerror(decoded));
        return -1;
    }

    return decoded;
}

/* ========================================================================= */
/* Loopback playback                                                         */
/* ========================================================================= */

static void Voice_PlayLoopback(const int16_t *pcm, int samples)
{
    if (!loopback_enabled || !loopback_source)
        return;

    /* Unqueue any finished buffers */
    ALint processed = 0;
    alGetSourcei(loopback_source, AL_BUFFERS_PROCESSED, &processed);
    while (processed > 0) {
        ALuint buf;
        alSourceUnqueueBuffers(loopback_source, 1, &buf);
        processed--;
    }

    /* Queue new buffer with captured audio */
    ALuint buf = loopback_buffers[loopback_buf_index];
    loopback_buf_index = (loopback_buf_index + 1) % 4;

    alBufferData(buf, AL_FORMAT_MONO16, pcm, samples * sizeof(int16_t), 
                 VOICE_SAMPLE_RATE);
    alSourceQueueBuffers(loopback_source, 1, &buf);

    /* Start playing if not already */
    ALint state;
    alGetSourcei(loopback_source, AL_SOURCE_STATE, &state);
    if (state != AL_PLAYING)
        alSourcePlay(loopback_source);
}

void Voice_Loopback(bool enable)
{
    if (enable == loopback_enabled)
        return;

    loopback_enabled = enable;

    if (enable) {
        if (!loopback_source) {
            alGenSources(1, &loopback_source);
            alGenBuffers(4, loopback_buffers);
            alSourcef(loopback_source, AL_GAIN, 1.0f);
            alSourcei(loopback_source, AL_SOURCE_RELATIVE, AL_TRUE);
            alSource3f(loopback_source, AL_POSITION, 0, 0, 0);
        }
        Com_Printf("Voice loopback enabled - you will hear yourself\n");
    } else {
        if (loopback_source) {
            alSourceStop(loopback_source);
            /* Unqueue all buffers */
            ALint queued;
            alGetSourcei(loopback_source, AL_BUFFERS_QUEUED, &queued);
            while (queued > 0) {
                ALuint buf;
                alSourceUnqueueBuffers(loopback_source, 1, &buf);
                queued--;
            }
        }
        Com_Printf("Voice loopback disabled\n");
    }
}

/* ========================================================================= */
/* Capture                                                                   */
/* ========================================================================= */

static bool Voice_StartCapture(void)
{
    if (capture_started)
        return true;

    if (!capture_device) {
        Com_WPrintf("Voice_StartCapture: no capture device\n");
        return false;
    }

    alcCaptureStart(capture_device);
    if (alcGetError(capture_device) != ALC_NO_ERROR) {
        Com_WPrintf("Voice_StartCapture: alcCaptureStart failed\n");
        return false;
    }

    capture_started = true;
    Com_DPrintf("Voice capture started\n");
    return true;
}

static void Voice_StopCapture(void)
{
    if (!capture_started)
        return;

    if (capture_device) {
        alcCaptureStop(capture_device);
    }

    capture_started = false;
    voice_active = false;
    voice_hangtime_remaining = 0;
    Com_DPrintf("Voice capture stopped\n");
}

void Voice_Capture(void)
{
    if (!capture_device || !voice_encoder)
        return;

    /* Check if voice is enabled for sending */
    if (cl_voip->integer < 2) {
        if (capture_started)
            Voice_StopCapture();
        return;
    }

    /* Start capture if not already */
    if (!capture_started) {
        if (!Voice_StartCapture())
            return;
    }

    /* Check how many samples are available */
    ALint samples_available = 0;
    alcGetIntegerv(capture_device, ALC_CAPTURE_SAMPLES, 1, &samples_available);

    if (samples_available < VOICE_FRAME_SAMPLES)
        return;  /* Not enough for a full frame */

    /* Only process one frame per call to avoid buffering too much */
    int samples_to_read = VOICE_FRAME_SAMPLES;
    if (samples_to_read > CAPTURE_BUFFER_SAMPLES)
        samples_to_read = CAPTURE_BUFFER_SAMPLES;

    /* Read samples from capture device */
    alcCaptureSamples(capture_device, capture_buffer, samples_to_read);
    if (alcGetError(capture_device) != ALC_NO_ERROR) {
        Com_WPrintf("Voice_Capture: alcCaptureSamples failed\n");
        return;
    }

    /* Apply input gain */
    Voice_ApplyGain(capture_buffer, samples_to_read, cl_voip_gain->value);

    /* Voice activation detection */
    float rms = Voice_CalculateRMS(capture_buffer, samples_to_read);
    bool voice_detected = rms > cl_voip_threshold->value;

    if (voice_detected) {
        voice_active = true;
        voice_hangtime_remaining = cl_voip_hangtime->integer;
    } else if (voice_active) {
        /* Hysteresis - keep transmitting for hangtime after voice stops */
        int64_t now = Sys_Milliseconds();
        int delta = (int)(now - voice_last_frame_time);
        voice_hangtime_remaining -= delta;

        if (voice_hangtime_remaining <= 0) {
            voice_active = false;
            voice_hangtime_remaining = 0;
        }
    }

    voice_last_frame_time = Sys_Milliseconds();

    /* If voice is active, encode and handle the data */
    if (voice_active) {
        uint8_t opus_data[VOICE_MAX_FRAME_BYTES];
        int opus_len = Voice_Encode(capture_buffer, samples_to_read, 
                                    opus_data, sizeof(opus_data));

        if (opus_len > 0) {
            /* For loopback testing: decode and play back immediately */
            if (loopback_enabled) {
                int16_t decoded[VOICE_FRAME_SAMPLES];
                int decoded_samples = Voice_Decode(opus_data, opus_len,
                                                   decoded, VOICE_FRAME_SAMPLES);
                if (decoded_samples > 0) {
                    Voice_PlayLoopback(decoded, decoded_samples);
                }
            }

            /* Queue the encoded frame for network transmission */
            if (voice_queue_count < VOICE_MAX_FRAMES_BUNDLE) {
                voice_frame_t *frame = &voice_queue[voice_queue_count];
                memcpy(frame->data, opus_data, opus_len);
                frame->len = opus_len;
                voice_queue_count++;
            }
        }
    }
}

/* ========================================================================= */
/* Network transmission                                                      */
/* ========================================================================= */

int Voice_WritePacket(void)
{
    if (voice_queue_count == 0)
        return 0;

    /* Check if voice is enabled for sending */
    if (!cl_voip || cl_voip->integer < 2)
        return 0;

    /* Calculate total size needed */
    int total_data_size = 0;
    for (int i = 0; i < voice_queue_count; i++) {
        total_data_size += voice_queue[i].len;
    }

    /* Format: clc_voice (1) + frame_count (1) + lengths (N) + data (variable) */
    int packet_size = 1 + 1 + voice_queue_count + total_data_size;

    /* Write the voice packet */
    MSG_WriteByte(clc_voice);
    MSG_WriteByte(voice_queue_count);

    /* Write frame lengths */
    for (int i = 0; i < voice_queue_count; i++) {
        MSG_WriteByte(voice_queue[i].len);
    }

    /* Write frame data */
    for (int i = 0; i < voice_queue_count; i++) {
        MSG_WriteData(voice_queue[i].data, voice_queue[i].len);
    }

    /* Clear the queue */
    voice_queue_count = 0;

    Com_DDPrintf("Voice_WritePacket: sent %d frames, %d bytes\n",
                 voice_queue_count, packet_size);

    return packet_size;
}

/* ========================================================================= */
/* Network reception                                                         */
/* ========================================================================= */

/*
 * Get or create a voice client state for the given sender
 */
static voice_client_t *Voice_GetClient(int sender)
{
    if (sender < 0 || sender >= VOICE_MAX_CLIENTS)
        return NULL;

    voice_client_t *vc = &voice_clients[sender];

    if (!vc->active) {
        /* Initialize new voice client */
        vc->active = true;
        vc->buf_write_index = 0;
        vc->buf_read_index = 0;

        /* Create OpenAL source */
        alGenSources(1, &vc->source);
        alGenBuffers(8, vc->buffers);

        /* Configure source for stereo panning (relative to listener) */
        alSourcef(vc->source, AL_GAIN, 1.0f);
        alSourcei(vc->source, AL_SOURCE_RELATIVE, AL_TRUE);  /* Position is relative to listener */
        alSource3f(vc->source, AL_POSITION, 0, 0, -1);  /* Default: in front */

        /* Create decoder for this client */
        int err;
        vc->decoder = opus_decoder_create(VOICE_SAMPLE_RATE, VOICE_CHANNELS, &err);
        if (err != OPUS_OK) {
            Com_WPrintf("Voice: failed to create decoder for client %d\n", sender);
            vc->decoder = NULL;
        }

        Com_DPrintf("Voice: initialized playback for client %d\n", sender);
    }

    vc->last_recv_time = Sys_Milliseconds();
    return vc;
}

/*
 * Clean up inactive voice clients
 * TODO: Call this periodically to free resources from disconnected players
 */
static q_unused void Voice_CleanupClients(void)
{
    int64_t now = Sys_Milliseconds();

    for (int i = 0; i < VOICE_MAX_CLIENTS; i++) {
        voice_client_t *vc = &voice_clients[i];
        if (!vc->active)
            continue;

        /* Clean up if no voice received for 5 seconds */
        if (now - vc->last_recv_time > 5000) {
            if (vc->source) {
                alSourceStop(vc->source);
                alDeleteSources(1, &vc->source);
                alDeleteBuffers(8, vc->buffers);
                vc->source = 0;
            }
            if (vc->decoder) {
                opus_decoder_destroy(vc->decoder);
                vc->decoder = NULL;
            }
            vc->active = false;
            Com_DPrintf("Voice: cleaned up client %d\n", i);
        }
    }
}

void Voice_ReceiveFrom(int sender, uint8_t volume, int8_t pan,
                       const uint8_t *opus_data, int opus_len,
                       const uint8_t *frame_lens, int frame_count)
{
    /* Check if voice reception is enabled */
    if (!cl_voip || cl_voip->integer < 1)
        return;

    voice_client_t *vc = Voice_GetClient(sender);
    if (!vc || !vc->decoder)
        return;

    /* Decode each frame and queue for playback */
    const uint8_t *data_ptr = opus_data;
    for (int i = 0; i < frame_count; i++) {
        int frame_len = frame_lens[i];

        /* Decode to PCM */
        int16_t pcm[VOICE_FRAME_SAMPLES];
        int samples = opus_decode(vc->decoder, data_ptr, frame_len,
                                  pcm, VOICE_FRAME_SAMPLES, 0);
        if (samples <= 0) {
            Com_DPrintf("Voice: decode failed for client %d\n", sender);
            data_ptr += frame_len;
            continue;
        }

        /* Apply volume from server */
        float vol = volume / 255.0f;
        for (int j = 0; j < samples; j++) {
            pcm[j] = (int16_t)(pcm[j] * vol);
        }

        /* Unqueue finished buffers */
        ALint processed = 0;
        alGetSourcei(vc->source, AL_BUFFERS_PROCESSED, &processed);
        while (processed > 0) {
            ALuint buf;
            alSourceUnqueueBuffers(vc->source, 1, &buf);
            processed--;
        }

        /* Queue new buffer */
        ALuint buf = vc->buffers[vc->buf_write_index];
        vc->buf_write_index = (vc->buf_write_index + 1) % 8;

        alBufferData(buf, AL_FORMAT_MONO16, pcm, samples * sizeof(int16_t),
                     VOICE_SAMPLE_RATE);
        alSourceQueueBuffers(vc->source, 1, &buf);

        /* NOTE(notscared) Apply pan from server for stereo positioning.
         * Pan is -128 (left) to +127 (right).
         * Position source in listener-relative coordinates:
         * X = left/right, Y = up/down, Z = front/back (negative = in front)
         */
        float pan_normalized = (float)pan / 127.0f;
        alSource3f(vc->source, AL_POSITION, pan_normalized, 0.0f, -1.0f);

        /* Start playing if not already */
        ALint state;
        alGetSourcei(vc->source, AL_SOURCE_STATE, &state);
        if (state != AL_PLAYING)
            alSourcePlay(vc->source);

        data_ptr += frame_len;
    }
}

void Voice_ParseServerPacket(void)
{
    /* Read svc_voice packet format:
     * sender (1) + volume (1) + pan (1) + frame_count (1) + lengths (N) + data
     */
    int sender = MSG_ReadByte();
    int volume = MSG_ReadByte();
    int pan = (int8_t)MSG_ReadByte();  /* Signed */
    int frame_count = MSG_ReadByte();

    if (frame_count <= 0 || frame_count > VOICE_MAX_FRAMES_BUNDLE) {
        Com_DPrintf("Voice_ParseServerPacket: invalid frame_count %d\n", frame_count);
        return;
    }

    /* Read frame lengths */
    uint8_t frame_lens[VOICE_MAX_FRAMES_BUNDLE];
    int total_data_len = 0;
    for (int i = 0; i < frame_count; i++) {
        frame_lens[i] = MSG_ReadByte();
        total_data_len += frame_lens[i];
    }

    /* Read frame data */
    uint8_t *opus_data = MSG_ReadData(total_data_len);
    if (!opus_data) {
        Com_DPrintf("Voice_ParseServerPacket: failed to read opus data\n");
        return;
    }

    Voice_ReceiveFrom(sender, (uint8_t)volume, (int8_t)pan,
                      opus_data, total_data_len, frame_lens, frame_count);
}

bool Voice_IsActive(void)
{
    return voice_active;
}

float Voice_GetThreshold(void)
{
    return cl_voip_threshold ? cl_voip_threshold->value : VOICE_DEFAULT_THRESHOLD;
}

int Voice_GetHangtime(void)
{
    return cl_voip_hangtime ? cl_voip_hangtime->integer : VOICE_DEFAULT_HANGTIME;
}

/* ========================================================================= */
/* Console commands                                                          */
/* ========================================================================= */

static void Voice_Loopback_f(void)
{
    if (Cmd_Argc() < 2) {
        Com_Printf("Usage: voice_loopback <0|1>\n");
        Com_Printf("Current: %d\n", loopback_enabled ? 1 : 0);
        return;
    }

    int enable = atoi(Cmd_Argv(1));
    Voice_Loopback(enable != 0);
}

static void Voice_Test_f(void)
{
    Com_Printf("Voice subsystem status:\n");
    Com_Printf("  Encoder: %s\n", voice_encoder ? "OK" : "NOT INITIALIZED");
    Com_Printf("  Decoder: %s\n", voice_decoder ? "OK" : "NOT INITIALIZED");
    Com_Printf("  Capture device: %s\n", capture_device ? "OK" : "NOT INITIALIZED");
    Com_Printf("  Capture active: %s\n", capture_started ? "YES" : "NO");
    Com_Printf("  Voice active: %s\n", voice_active ? "YES" : "NO");
    Com_Printf("  Loopback: %s\n", loopback_enabled ? "ON" : "OFF");
    Com_Printf("  cl_voip: %d\n", cl_voip ? cl_voip->integer : -1);
    Com_Printf("  Threshold: %.3f\n", cl_voip_threshold ? cl_voip_threshold->value : -1);
    Com_Printf("  Hangtime: %d ms\n", cl_voip_hangtime ? cl_voip_hangtime->integer : -1);
    Com_Printf("  Gain: %.2f\n", cl_voip_gain ? cl_voip_gain->value : -1);
}

/* ========================================================================= */
/* Init / Shutdown                                                           */
/* ========================================================================= */

bool Voice_Init(void)
{
    int err;

    Com_Printf("Initializing voice chat subsystem...\n");

    /* Register cvars */
    cl_voip = Cvar_Get("cl_voip", "2", 0);
    cl_voip_threshold = Cvar_Get("cl_voip_threshold", "0.02", 0);
    cl_voip_hangtime = Cvar_Get("cl_voip_hangtime", "300", 0);
    cl_voip_gain = Cvar_Get("cl_voip_gain", "1.0", 0);
    cl_voip_loopback = Cvar_Get("cl_voip_loopback", "0", 0);

    /* Register commands */
    Cmd_AddCommand("voice_loopback", Voice_Loopback_f);
    Cmd_AddCommand("voice_test", Voice_Test_f);

    /* Create Opus encoder */
    voice_encoder = opus_encoder_create(VOICE_SAMPLE_RATE, VOICE_CHANNELS,
                                        OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || !voice_encoder) {
        Com_EPrintf("Voice_Init: opus_encoder_create failed: %s\n",
                    opus_strerror(err));
        return false;
    }

    /* Configure encoder */
    opus_encoder_ctl(voice_encoder, OPUS_SET_BITRATE(VOICE_BITRATE));
    opus_encoder_ctl(voice_encoder, OPUS_SET_COMPLEXITY(5));
    opus_encoder_ctl(voice_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(voice_encoder, OPUS_SET_DTX(1));  /* Discontinuous transmission */

    /* Create Opus decoder */
    voice_decoder = opus_decoder_create(VOICE_SAMPLE_RATE, VOICE_CHANNELS, &err);
    if (err != OPUS_OK || !voice_decoder) {
        Com_EPrintf("Voice_Init: opus_decoder_create failed: %s\n",
                    opus_strerror(err));
        opus_encoder_destroy(voice_encoder);
        voice_encoder = NULL;
        return false;
    }

    /* Open capture device */
    const char *capture_device_name = alcGetString(NULL, ALC_CAPTURE_DEFAULT_DEVICE_SPECIFIER);
    if (!capture_device_name) {
        Com_WPrintf("Voice_Init: no capture device available\n");
        /* Not fatal - we can still receive voice from others */
    } else {
        Com_Printf("Voice capture device: %s\n", capture_device_name);

        capture_device = alcCaptureOpenDevice(
            NULL,  /* Use default device */
            VOICE_SAMPLE_RATE,
            AL_FORMAT_MONO16,
            CAPTURE_BUFFER_SAMPLES
        );

        if (!capture_device) {
            Com_WPrintf("Voice_Init: alcCaptureOpenDevice failed\n");
            /* Not fatal */
        }
    }

    Com_Printf("Voice chat initialized (Opus %s)\n", opus_get_version_string());
    return true;
}

void Voice_Shutdown(void)
{
    Com_Printf("Shutting down voice chat subsystem...\n");

    /* Stop capture */
    Voice_StopCapture();

    /* Clean up loopback */
    if (loopback_source) {
        alDeleteSources(1, &loopback_source);
        alDeleteBuffers(4, loopback_buffers);
        loopback_source = 0;
        memset(loopback_buffers, 0, sizeof(loopback_buffers));
    }

    /* Clean up remote voice clients */
    for (int i = 0; i < VOICE_MAX_CLIENTS; i++) {
        voice_client_t *vc = &voice_clients[i];
        if (!vc->active)
            continue;

        if (vc->source) {
            alSourceStop(vc->source);
            alDeleteSources(1, &vc->source);
            alDeleteBuffers(8, vc->buffers);
        }
        if (vc->decoder) {
            opus_decoder_destroy(vc->decoder);
        }
        memset(vc, 0, sizeof(*vc));
    }

    /* Close capture device */
    if (capture_device) {
        alcCaptureCloseDevice(capture_device);
        capture_device = NULL;
    }

    /* Destroy Opus encoder/decoder */
    if (voice_encoder) {
        opus_encoder_destroy(voice_encoder);
        voice_encoder = NULL;
    }

    if (voice_decoder) {
        opus_decoder_destroy(voice_decoder);
        voice_decoder = NULL;
    }

    /* Remove commands */
    Cmd_RemoveCommand("voice_loopback");
    Cmd_RemoveCommand("voice_test");

    /* Clear state */
    loopback_enabled = false;
    voice_active = false;
    voice_queue_count = 0;
}
