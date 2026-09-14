/* audio_sink.h -- shared libnx audout output layer, multi-client.
 *
 * Why multi-client: the APK analysis (docs/JNI_SURFACE.md) shows THREE
 * independent things can want the output device at once.
 *
 *   1. Godot's own AudioServer. libgodot_android.so has libOpenSLES.so in its
 *      NEEDED list, so the engine's audio driver is OpenSL ES and it
 *      initialises whether or not the game routes everything through FMOD.
 *   2. FMOD via AAudio. libfmod.so has no AAudio UND symbols but its string
 *      table is full of AAudioStreamBuilder_* names -- it dlopen()s libaaudio
 *      and dlsym()s each entry point.
 *   3. FMOD via org/fmod/AudioDevice, the AudioTrack fallback, reached if both
 *      of the above fail.
 *
 * audout is single-instance. The original single-owner design would have had
 * whichever of these initialised second get a failed audoutInitialize, which
 * would have looked like an FMOD bug and would not have been one. So the sink
 * owns the device and mixes its clients.
 *
 * Format is fixed at what audout is: 48 kHz, stereo, signed 16-bit. Clients
 * resample; the sink only converts channel count.
 *
 * MIT license; see LICENSE. */

#ifndef __AUDIO_SINK_H__
#define __AUDIO_SINK_H__

#include <stdint.h>
#include <stddef.h>

#define SINK_SAMPLE_RATE 48000
#define SINK_CHANNELS    2
#define SINK_FRAME_BYTES (SINK_CHANNELS * (int)sizeof(int16_t))

// 960 frames = 20 ms at 48 kHz. Reported to FMOD as getOutputBlockSize, so it
// must stay consistent with jni_fmod.c.
#define SINK_FRAMES_PER_BUF 960

// 4 x 20 ms = 80 ms of slack, which is what drmariomania_nx settled on for
// audout under asset-load stalls.
#define SINK_NUM_BUFFERS 4

// Godot's OpenSL driver, FMOD/AAudio, FMOD/AudioDevice. Three is the real
// ceiling; the fourth slot is headroom for a bring-up tone generator.
#define SINK_MAX_CLIENTS 4

// --- client API --------------------------------------------------------------

// Opens the device if this is the first client. `name` is for logging only.
// Returns a client id, or negative on failure.
int  audio_sink_client_open(const char *name);

// Closes the client; closes the device when the last one leaves.
void audio_sink_client_close(int client);

// Blocking write of interleaved s16. src_channels of 1 is duplicated to
// stereo, 2 is passed through, >2 takes the first two. Blocks until the ring
// has room, which paces the caller at real time -- that is what makes
// AAudioStream_write's contract work without a sleep(). Returns frames taken.
int  audio_sink_client_write(int client, const int16_t *frames,
                             int frame_count, int src_channels);

// Non-blocking variant for callback-driven clients that must not stall.
int  audio_sink_client_write_nonblocking(int client, const int16_t *frames,
                                         int frame_count, int src_channels);

int  audio_sink_client_queued(int client);

// --- device-wide -------------------------------------------------------------

int     audio_sink_is_open(void);
void    audio_sink_set_paused(int paused);   // applet focus loss
int64_t audio_sink_frames_written(void);
int     audio_sink_active_clients(void);

#endif
