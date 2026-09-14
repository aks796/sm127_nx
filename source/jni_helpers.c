/* jni_helpers.c -- the small services jni_godot_ext.c and jni_fmod.c need from
 * the fake JNI environment.
 *
 * Kept out of jni_fake.c so the inherited file stays close to upstream and
 * fixes can still be moved between this port and drmariomania_nx.
 *
 * MIT license; see LICENSE. */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "jni_fake.h"
#include "jni_godot_ext.h"
#include "imports.h"

#if DEBUG_LOG
#define hLog(...) debugPrintf("[jni-help] " __VA_ARGS__)
#else
#define hLog(...) do {} while (0)
#endif

// Tags duplicated from jni_fake.c. Kept in sync by the compile-time assert
// below rather than by hoping -- if the object model changes, this fails to
// build instead of reading garbage out of a FakeString.
#define TAG_STRING 0x53545231
#define TAG_PRIARR 0x50415231

typedef struct { uint32_t tag; char *utf; } HFakeString;
typedef struct { uint32_t tag; int len; int elem_size; void *data; } HFakePriArray;

// --- string / array views ----------------------------------------------------

const char *jni_string_utf(void *jstring_ref) {
  HFakeString *s = (HFakeString *)jstring_ref;
  if (s && s->tag == TAG_STRING && s->utf) return s->utf;
  return NULL;
}

const int16_t *jni_short_array_data(void *array_ref) {
  HFakePriArray *a = (HFakePriArray *)array_ref;
  if (!a || a->tag != TAG_PRIARR) return NULL;
  if (a->elem_size != (int)sizeof(int16_t)) {
    hLog("short array with elem_size %d -- refusing\n", a->elem_size);
    return NULL;
  }
  return (const int16_t *)a->data;
}

int jni_array_length(void *array_ref) {
  HFakePriArray *a = (HFakePriArray *)array_ref;
  return (a && a->tag == TAG_PRIARR) ? a->len : 0;
}

// --- lifecycle ---------------------------------------------------------------
//
// The engine tells us when setup finished and when the main loop started.
// watchdog.c needs the difference: a process sitting in asset load for 40
// seconds is fine, the same wall-clock silence after the main loop started is
// a hang.

static volatile int s_lifecycle = -1;
static volatile int s_mainloop_started = 0;

void jni_note_lifecycle(jni_lifecycle_event ev) {
  s_lifecycle = (int)ev;
  if (ev == JNI_LIFECYCLE_MAINLOOP_STARTED) s_mainloop_started = 1;
  if (ev == JNI_LIFECYCLE_TERMINATING)      s_mainloop_started = 0;
}

int jni_lifecycle_state(void)    { return s_lifecycle; }
int jni_mainloop_started(void)   { return s_mainloop_started; }

// --- rumble ------------------------------------------------------------------
//
// Godot.vibrate(durationMs, amplitude). Android's amplitude is 1..255 with 0
// meaning "default"; libnx wants normalised floats plus frequency bands.
//
// Deliberately fire-and-forget: the duration is tracked and the main loop
// stops the motors when it expires (jni_rumble_tick), because holding a thread
// asleep for the duration inside a JNI call would stall the engine.

static HidVibrationDeviceHandle s_vib[2];
static int      s_vib_ready = 0;
static uint64_t s_vib_stop_tick = 0;

static void rumble_init(void) {
  if (s_vib_ready) return;
  Result rc = hidInitializeVibrationDevices(s_vib, 2, HidNpadIdType_No1,
                                            HidNpadStyleSet_NpadFullCtrl);
  if (R_FAILED(rc)) {
    hLog("hidInitializeVibrationDevices failed: 0x%x\n", rc);
    return;
  }
  s_vib_ready = 1;
}

static void rumble_set(float amp) {
  if (!s_vib_ready) return;
  HidVibrationValue v[2];
  for (int i = 0; i < 2; i++) {
    // 160/320 Hz are the band centres libnx's own examples use; amplitude is
    // split evenly so a single-motor pattern still feels symmetric.
    v[i].freq_low   = 160.0f;
    v[i].freq_high  = 320.0f;
    v[i].amp_low    = amp;
    v[i].amp_high   = amp;
  }
  hidSendVibrationValues(s_vib, v, 2);
}

void jni_request_rumble(int duration_ms, int amplitude) {
  rumble_init();
  if (!s_vib_ready) return;

  if (duration_ms <= 0) { rumble_set(0.0f); s_vib_stop_tick = 0; return; }

  // Android: 0 means "use the default strength", 1..255 otherwise.
  float amp = (amplitude <= 0) ? 0.5f : (float)amplitude / 255.0f;
  if (amp > 1.0f) amp = 1.0f;

  rumble_set(amp);
  s_vib_stop_tick = armGetSystemTick() +
                    armNsToTicks((uint64_t)duration_ms * 1000000ULL);
}

// Called once per frame from main.c's loop.
void jni_rumble_tick(void) {
  if (!s_vib_stop_tick) return;
  if (armGetSystemTick() >= s_vib_stop_tick) {
    rumble_set(0.0f);
    s_vib_stop_tick = 0;
  }
}

void jni_rumble_stop(void) {
  if (s_vib_ready) rumble_set(0.0f);
  s_vib_stop_tick = 0;
}

// --- deferred dialog cancellation --------------------------------------------
//
// showDialog/showInputDialog/showFilePicker are asynchronous on Android: the
// Java side returns immediately and later calls back into GodotLib. Doing
// nothing leaves whatever asked for the dialog waiting forever, so the
// callback has to fire -- but NOT from inside the JNI call, because the engine
// is mid-call on its own thread and re-entering it is how you get a deadlock
// or a corrupted Variant stack.
//
// So the request is queued and drained from the frame loop, on the same thread
// that owns the engine.

#define DIALOG_QUEUE_MAX 8

static volatile int s_dialog_q[DIALOG_QUEUE_MAX];
static volatile int s_dialog_head = 0, s_dialog_tail = 0;
static Mutex s_dialog_lock;

void jni_post_dialog_cancel(jni_dialog_kind kind) {
  mutexLock(&s_dialog_lock);
  int next = (s_dialog_head + 1) % DIALOG_QUEUE_MAX;
  if (next != s_dialog_tail) {
    s_dialog_q[s_dialog_head] = (int)kind;
    s_dialog_head = next;
  } else {
    hLog("dialog queue full, dropping cancel for kind %d\n", (int)kind);
  }
  mutexUnlock(&s_dialog_lock);
}

// Resolved lazily from the engine module. All three are in the exported dynsym
// list (confirmed in docs/jni_surface_godot.txt), so a null here means the
// engine build differs from the one that was analysed.
static void (*e_dialogCallback)(void *env, void *cls, int button) = NULL;
static void (*e_inputDialogCallback)(void *env, void *cls, void *text) = NULL;
static void (*e_filePickerCallback)(void *env, void *cls, int ok, void *paths) = NULL;
static int s_cb_resolved = 0;

static void resolve_callbacks(void) {
  if (s_cb_resolved) return;
  s_cb_resolved = 1;
#define G "Java_org_godotengine_godot_GodotLib_"
  e_dialogCallback      = (void *)sm127_engine_symbol(G "dialogCallback");
  e_inputDialogCallback = (void *)sm127_engine_symbol(G "inputDialogCallback");
  e_filePickerCallback  = (void *)sm127_engine_symbol(G "filePickerCallback");
#undef G
  hLog("dialog callbacks: %p %p %p\n",
       (void *)e_dialogCallback, (void *)e_inputDialogCallback,
       (void *)e_filePickerCallback);
}

// Drains the queue. MUST be called from the engine thread, between frames.
void jni_drain_dialog_queue(void) {
  resolve_callbacks();

  for (;;) {
    int kind = -1;
    mutexLock(&s_dialog_lock);
    if (s_dialog_tail != s_dialog_head) {
      kind = s_dialog_q[s_dialog_tail];
      s_dialog_tail = (s_dialog_tail + 1) % DIALOG_QUEUE_MAX;
    }
    mutexUnlock(&s_dialog_lock);
    if (kind < 0) return;

    void *cls = jni_activity_class();

    switch ((jni_dialog_kind)kind) {
      case JNI_DIALOG_PLAIN:
        // Button index. Godot's DisplayServer treats the callback's argument
        // as the index pressed; 0 is the first (and for an alert, only) button.
        if (e_dialogCallback) e_dialogCallback(fake_env, cls, 0);
        break;

      case JNI_DIALOG_INPUT:
        // Empty string rather than null: Godot's handler does not null-check.
        if (e_inputDialogCallback)
          e_inputDialogCallback(fake_env, cls, jni_new_string(""));
        break;

      case JNI_DIALOG_FILE:
        // ok=0 (cancelled) with an empty path array.
        if (e_filePickerCallback)
          e_filePickerCallback(fake_env, cls, 0, jni_new_string_array(0, NULL));
        break;
    }
    hLog("dialog cancel delivered, kind %d\n", kind);
  }
}
