/* swkbd_shim.h -- GodotIO.showKeyboard via libnx swkbd. See swkbd_shim.c.
 * MIT license; see LICENSE. */

#ifndef __SWKBD_SHIM_H__
#define __SWKBD_SHIM_H__

// --- engine thread -----------------------------------------------------------
// Queues a keyboard request and returns immediately. Called from
// GodotIO.showKeyboard in jni_godot_ext.c.
void swkbd_request(const char *existing, int type, int max_len);

// GodotIO.hideKeyboard.
void swkbd_hide(void);

int  swkbd_is_pending(void);

// setenv(), as the engine's import: passes everything through. SM127NX_KBD
// also records what kind of field is about to be edited (switch_port.gd sets
// it as a text field takes focus), which Godot 3's showKeyboard does not say.
int  sm127_setenv(const char *name, const char *value, int overwrite);

// Turns a completed result into GodotLib.key events. Call from the frame loop,
// on the engine thread, next to jni_drain_dialog_queue().
void swkbd_deliver(void);

// --- main thread -------------------------------------------------------------
// Runs the applet. BLOCKS while the keyboard is up (the OS suspends the
// process), so this must be on the thread that owns appletMainLoop(), never
// the engine thread.
void swkbd_pump(void);

#endif
