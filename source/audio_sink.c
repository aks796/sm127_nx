/* audio_sink.c -- see audio_sink.h. MIT license; see LICENSE. */

#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "audio_sink.h"

#if VERBOSE_AUDIO && DEBUG_LOG
#define sinkLog(...) debugPrintf("[sink] " __VA_ARGS__)
#else
#define sinkLog(...) do {} while (0)
#endif

// audout wants 0x1000-aligned buffers whose size is a multiple of 0x1000.
#define SINK_BUF_RAW   (SINK_FRAMES_PER_BUF * SINK_FRAME_BYTES)
#define SINK_BUF_ALIGN 0x1000
#define SINK_BUF_SIZE  (((SINK_BUF_RAW) + (SINK_BUF_ALIGN - 1)) & ~(SINK_BUF_ALIGN - 1))

// Per-client ring, 4x the in-flight window so a producer burst does not stall
// during a GC pause on the managed side.
#define RING_FRAMES (SINK_FRAMES_PER_BUF * SINK_NUM_BUFFERS * 4)

typedef struct {
  int      active;
  char     name[24];
  int16_t *data;           // RING_FRAMES * SINK_CHANNELS
  int      head, tail;     // frame indices
  Mutex    lock;
  CondVar  space;
  int64_t  underruns;
} Client;

static Client         s_clients[SINK_MAX_CLIENTS];
static Mutex          s_clients_lock;
static int            s_client_count = 0;

static AudioOutBuffer s_bufs[SINK_NUM_BUFFERS];
static void          *s_buf_mem[SINK_NUM_BUFFERS];
static Thread         s_feeder;
static int            s_open = 0;
static int            s_running = 0;
static int            s_paused = 0;
static int64_t        s_frames_written = 0;

// s32 accumulator so summing several clients cannot wrap before the
// saturating store.
static int32_t        s_mix[SINK_FRAMES_PER_BUF * SINK_CHANNELS];

static int ring_used(Client *c) {
  int used = c->head - c->tail;
  if (used < 0) used += RING_FRAMES;
  return used;
}

static int ring_free(Client *c) {
  return RING_FRAMES - 1 - ring_used(c);   // one frame kept empty
}

static inline int16_t sat16(int32_t v) {
  if (v >  32767) return  32767;
  if (v < -32768) return -32768;
  return (int16_t)v;
}

static void mix_client(Client *c, int want) {
  mutexLock(&c->lock);
  int used = ring_used(c);
  int take = used < want ? used : want;

  for (int i = 0; i < take; i++) {
    int idx = (c->tail + i) % RING_FRAMES;
    s_mix[i * SINK_CHANNELS + 0] += c->data[idx * SINK_CHANNELS + 0];
    s_mix[i * SINK_CHANNELS + 1] += c->data[idx * SINK_CHANNELS + 1];
  }
  c->tail = (c->tail + take) % RING_FRAMES;

  if (take < want) c->underruns++;

  condvarWakeAll(&c->space);
  mutexUnlock(&c->lock);
}

static void feeder_thread(void *arg) {
  (void)arg;

  // Prime every buffer with silence so audout has a full pipeline before the
  // first client catches up; starting partially filled is audible as a click.
  for (int i = 0; i < SINK_NUM_BUFFERS; i++) {
    memset(s_buf_mem[i], 0, SINK_BUF_SIZE);
    s_bufs[i].next        = NULL;
    s_bufs[i].buffer      = s_buf_mem[i];
    s_bufs[i].buffer_size = SINK_BUF_SIZE;
    s_bufs[i].data_size   = SINK_BUF_RAW;
    s_bufs[i].data_offset = 0;
    audoutAppendAudioOutBuffer(&s_bufs[i]);
  }

  while (s_running) {
    AudioOutBuffer *released = NULL;
    uint32_t count = 0;

    // 100 ms timeout so a stalled producer still lets us observe s_running
    // going false rather than blocking until the next buffer completes.
    Result rc = audoutWaitPlayFinish(&released, &count, 100000000ULL);
    if (R_FAILED(rc) || !released) continue;

    if (s_paused) {
      memset(released->buffer, 0, SINK_BUF_SIZE);
    } else {
      memset(s_mix, 0, sizeof(s_mix));

      mutexLock(&s_clients_lock);
      for (int i = 0; i < SINK_MAX_CLIENTS; i++)
        if (s_clients[i].active) mix_client(&s_clients[i], SINK_FRAMES_PER_BUF);
      mutexUnlock(&s_clients_lock);

      int16_t *dst = (int16_t *)released->buffer;
      for (int i = 0; i < SINK_FRAMES_PER_BUF * SINK_CHANNELS; i++)
        dst[i] = sat16(s_mix[i]);

      s_frames_written += SINK_FRAMES_PER_BUF;
    }

    released->data_size   = SINK_BUF_RAW;
    released->data_offset = 0;
    audoutAppendAudioOutBuffer(released);
  }
}

static int device_open(void) {
  Result rc = audoutInitialize();
  if (R_FAILED(rc)) { sinkLog("audoutInitialize failed: 0x%x\n", rc); return -1; }

  rc = audoutStartAudioOut();
  if (R_FAILED(rc)) { sinkLog("audoutStartAudioOut failed: 0x%x\n", rc); audoutExit(); return -1; }

  sinkLog("audout: %lu Hz, %lu ch\n",
          (unsigned long)audoutGetSampleRate(),
          (unsigned long)audoutGetChannelCount());

  for (int i = 0; i < SINK_NUM_BUFFERS; i++) {
    s_buf_mem[i] = memalign(SINK_BUF_ALIGN, SINK_BUF_SIZE);
    if (!s_buf_mem[i]) {
      for (int j = 0; j < i; j++) { free(s_buf_mem[j]); s_buf_mem[j] = NULL; }
      audoutStopAudioOut(); audoutExit();
      return -1;
    }
  }

  s_running = 1;
  s_paused  = 0;
  s_frames_written = 0;

  // priority 0x2C: just above the engine's audio threads as created through
  // the pthread shim, so the feeder is not starved during asset decode.
  // Core 0 (light work): away from the frame thread on core 1 and the mixer
  // on core 2.
  rc = threadCreate(&s_feeder, feeder_thread, NULL, NULL, 0x8000, 0x2C, 0);
  if (R_FAILED(rc)) rc = threadCreate(&s_feeder, feeder_thread, NULL, NULL, 0x8000, 0x2C, -2);
  if (R_SUCCEEDED(rc)) rc = threadStart(&s_feeder);
  if (R_FAILED(rc)) {
    s_running = 0;
    for (int i = 0; i < SINK_NUM_BUFFERS; i++) { free(s_buf_mem[i]); s_buf_mem[i] = NULL; }
    audoutStopAudioOut(); audoutExit();
    return -1;
  }

  s_open = 1;
  sinkLog("device open\n");
  return 0;
}

static void device_close(void) {
  s_running = 0;
  threadWaitForExit(&s_feeder);
  threadClose(&s_feeder);

  audoutStopAudioOut();
  audoutExit();

  for (int i = 0; i < SINK_NUM_BUFFERS; i++) { free(s_buf_mem[i]); s_buf_mem[i] = NULL; }

  s_open = 0;
  sinkLog("device closed\n");
}

int audio_sink_client_open(const char *name) {
  mutexLock(&s_clients_lock);

  int id = -1;
  for (int i = 0; i < SINK_MAX_CLIENTS; i++)
    if (!s_clients[i].active) { id = i; break; }

  if (id < 0) {
    sinkLog("no free client slot for '%s'\n", name ? name : "?");
    mutexUnlock(&s_clients_lock);
    return -1;
  }

  Client *c = &s_clients[id];
  c->data = (int16_t *)malloc((size_t)RING_FRAMES * SINK_FRAME_BYTES);
  if (!c->data) { mutexUnlock(&s_clients_lock); return -1; }
  memset(c->data, 0, (size_t)RING_FRAMES * SINK_FRAME_BYTES);

  c->head = c->tail = 0;
  c->underruns = 0;
  mutexInit(&c->lock);
  condvarInit(&c->space);
  strncpy(c->name, name ? name : "?", sizeof(c->name) - 1);
  c->name[sizeof(c->name) - 1] = '\0';
  c->active = 1;
  s_client_count++;

  int need_device = !s_open;
  mutexUnlock(&s_clients_lock);

  if (need_device && device_open() != 0) {
    mutexLock(&s_clients_lock);
    c->active = 0;
    free(c->data); c->data = NULL;
    s_client_count--;
    mutexUnlock(&s_clients_lock);
    return -1;
  }

  sinkLog("client %d '%s' opened (%d active)\n", id, c->name, s_client_count);
  return id;
}

void audio_sink_client_close(int client) {
  if (client < 0 || client >= SINK_MAX_CLIENTS) return;

  mutexLock(&s_clients_lock);
  Client *c = &s_clients[client];
  if (!c->active) { mutexUnlock(&s_clients_lock); return; }

  c->active = 0;
  s_client_count--;
  condvarWakeAll(&c->space);   // release anyone blocked in a write
  int last = (s_client_count == 0);
  int64_t under = c->underruns;
  mutexUnlock(&s_clients_lock);

  if (last && s_open) device_close();

  mutexLock(&s_clients_lock);
  free(c->data); c->data = NULL;
  mutexUnlock(&s_clients_lock);

  sinkLog("client %d closed (%d active, %lld underruns)\n",
          client, s_client_count, (long long)under);
}

// Writes `count` frames from `src` (src_channels interleaved) into c's ring,
// converting to stereo. Caller holds no lock.
static int write_frames(Client *c, const int16_t *src, int count,
                        int src_channels, int blocking) {
  int done = 0;

  while (done < count) {
    mutexLock(&c->lock);

    if (blocking) {
      while (ring_free(c) == 0 && c->active && s_running)
        condvarWait(&c->space, &c->lock);
    }
    if (!c->active || !s_running) { mutexUnlock(&c->lock); break; }

    int space = ring_free(c);
    if (space == 0) { mutexUnlock(&c->lock); break; }   // non-blocking, ring full

    int chunk = count - done;
    if (chunk > space) chunk = space;

    for (int i = 0; i < chunk; i++) {
      int idx = (c->head + i) % RING_FRAMES;
      const int16_t *f = src + (size_t)(done + i) * src_channels;
      if (src_channels == 1) {
        c->data[idx * SINK_CHANNELS + 0] = f[0];
        c->data[idx * SINK_CHANNELS + 1] = f[0];
      } else {
        c->data[idx * SINK_CHANNELS + 0] = f[0];
        c->data[idx * SINK_CHANNELS + 1] = f[1];
      }
    }
    c->head = (c->head + chunk) % RING_FRAMES;
    done += chunk;

    mutexUnlock(&c->lock);
  }
  return done;
}

int audio_sink_client_write(int client, const int16_t *frames,
                            int frame_count, int src_channels) {
  if (client < 0 || client >= SINK_MAX_CLIENTS || !frames || frame_count <= 0) return 0;
  Client *c = &s_clients[client];
  if (!c->active) return 0;
  return write_frames(c, frames, frame_count, src_channels < 1 ? 1 : src_channels, 1);
}

int audio_sink_client_write_nonblocking(int client, const int16_t *frames,
                                        int frame_count, int src_channels) {
  if (client < 0 || client >= SINK_MAX_CLIENTS || !frames || frame_count <= 0) return 0;
  Client *c = &s_clients[client];
  if (!c->active) return 0;
  return write_frames(c, frames, frame_count, src_channels < 1 ? 1 : src_channels, 0);
}

int audio_sink_client_queued(int client) {
  if (client < 0 || client >= SINK_MAX_CLIENTS) return 0;
  Client *c = &s_clients[client];
  if (!c->active) return 0;
  mutexLock(&c->lock);
  int used = ring_used(c);
  mutexUnlock(&c->lock);
  return used;
}

int     audio_sink_is_open(void)         { return s_open; }
int     audio_sink_active_clients(void)  { return s_client_count; }
int64_t audio_sink_frames_written(void)  { return s_frames_written; }
void    audio_sink_set_paused(int p)     { s_paused = p ? 1 : 0; }
