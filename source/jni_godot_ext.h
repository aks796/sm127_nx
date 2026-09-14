/* jni_godot_ext.h -- additions to jni_fake.c's org.godotengine.godot surface.
 * See jni_godot_ext.c and docs/JNI_SURFACE.md. MIT license; see LICENSE. */

#ifndef __JNI_GODOT_EXT_H__
#define __JNI_GODOT_EXT_H__

#include <stdarg.h>

// Each returns 1 if the name was handled. Call from the matching dispatcher in
// jni_fake.c, AFTER its own strcmp chain so the hardware-tested handlers keep
// priority and these only fill gaps.
int godot_ext_int(const char *name, int *out, va_list va);
int godot_ext_void(const char *name, va_list va);

int godot_io_ext_int(const char *name, int *out, va_list va);

int godot_renderview_ext(const char *name, int *out, va_list va);
int godot_dictionary_ext(const char *name, void **out);


// --- required from jni_fake.c ------------------------------------------------
// These do not exist yet. Each is a small addition; see docs/JNI_SURFACE.md
// "wiring" for what they need to do.

typedef enum {
  JNI_LIFECYCLE_SETUP_DONE = 0,
  JNI_LIFECYCLE_MAINLOOP_STARTED,
  JNI_LIFECYCLE_TERMINATING,
} jni_lifecycle_event;

typedef enum {
  JNI_DIALOG_PLAIN = 0,
  JNI_DIALOG_INPUT,
  JNI_DIALOG_FILE,
} jni_dialog_kind;

// UTF-8 view of a jstring. jni_fake.c already builds jstrings in
// jni_new_string(); this is the read direction.
const char *jni_string_utf(void *jstring_ref);

// Records an engine lifecycle transition. watchdog.c wants these to know the
// difference between "still loading" and "hung".
void jni_note_lifecycle(jni_lifecycle_event ev);

// Invokes GodotLib.dialogCallback / inputDialogCallback / filePickerCallback
// with a cancel result. Must be posted to the engine thread, not called
// inline -- see docs/JNI_SURFACE.md.
void jni_post_dialog_cancel(jni_dialog_kind kind);

// HID rumble via libnx hidSendVibrationValues. Fire-and-forget: the duration
// is tracked and jni_rumble_tick() stops the motors, so the JNI call does not
// block the engine thread.
void jni_request_rumble(int duration_ms, int amplitude);

// --- called from main.c's frame loop -----------------------------------------
void jni_rumble_tick(void);          // expire a running rumble
void jni_rumble_stop(void);          // on focus loss / shutdown
void jni_drain_dialog_queue(void);   // deliver queued dialog cancellations

// --- state, for watchdog.c ---------------------------------------------------
int jni_lifecycle_state(void);
int jni_mainloop_started(void);

int jni_array_length(void *array_ref);

#endif
