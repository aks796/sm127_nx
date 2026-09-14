/* jni_godot_ext.c -- the org.godotengine.godot methods jni_fake.c does not
 * yet answer.
 *
 * Derived, not guessed: intersection of classes.dex with the string table of
 * libgodot_android.so, minus the 68 names jni_fake.c already handles. See
 * docs/JNI_SURFACE.md.
 *
 * These are additions to jni_fake.c's existing strcmp chains rather than a
 * replacement -- the existing handlers are correct and hardware-tested in
 * drmariomania_nx, and there is no reason to disturb them.
 *
 * Editor-only entry points (nativeSignApk, nativeVerifyApk,
 * nativeOnEditorWorkspaceSelected, nativeDumpBenchmark) are present because
 * the engine looks them up unconditionally at startup, not because a release
 * build ever calls them. A missing GetMethodID is a pending JNI exception, and
 * an unchecked pending exception aborts on the next JNI call -- which is why
 * answering them matters even though they do nothing.
 *
 * MIT license; see LICENSE. */

#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "jni_fake.h"
#include "jni_godot_ext.h"
#include "swkbd_shim.h"
#include "web_applet.h"

#if DEBUG_LOG
#define gxLog(...) debugPrintf("[jni+] " __VA_ARGS__)
#else
#define gxLog(...) do {} while (0)
#endif

// --- Godot (instance), int/bool returning ------------------------------------

int godot_ext_int(const char *name, int *out, va_list va) {
  if (!name || !out) return 0;

  // hasFeature(Ljava/lang/String;)Z -- OS.has_feature() from GDScript/C#.
  // The engine asks about "mobile", "pc", "etc" and the game may ask about
  // anything. Answering "mobile" false and "pc" true is the honest mapping:
  // the Switch build is a TV/handheld console, but every code path guarded by
  // "mobile" in a Godot game assumes a touch-first phone UI, which this is not.
  if (!strcmp(name, "hasFeature")) {
    const char *feat = jni_string_utf(va_arg(va, void *));
    int r = 0;
    if (feat) {
      if      (!strcmp(feat, "mobile"))  r = 0;
      else if (!strcmp(feat, "pc"))      r = 1;
      else if (!strcmp(feat, "android")) r = 0;
      else if (!strcmp(feat, "etc2"))    r = 0;   // nouveau gives us S3TC/BPTC
      else if (!strcmp(feat, "astc"))    r = 0;
      else if (!strcmp(feat, "s3tc"))    r = 1;
      else if (!strcmp(feat, "bptc"))    r = 1;
      gxLog("hasFeature(%s) -> %d\n", feat, r);
    }
    *out = r;
    return 1;
  }

  // nativeSignApk / nativeVerifyApk -- editor-only. Non-zero is an Error code
  // in Godot's enum; ERR_UNAVAILABLE (48) is the honest answer.
  if (!strcmp(name, "nativeSignApk")) {
    for (int i = 0; i < 5; i++) (void)va_arg(va, void *);
    *out = 48;
    return 1;
  }
  if (!strcmp(name, "nativeVerifyApk")) {
    (void)va_arg(va, void *);
    *out = 48;
    return 1;
  }

  return 0;
}

// --- Godot (instance), void returning ----------------------------------------

int godot_ext_void(const char *name, va_list va) {
  if (!name) return 0;

  // Lifecycle notifications. onGodotSetupCompleted fires after the engine has
  // finished setup() but before the main loop; onGodotMainLoopStarted after
  // the first iteration. main.c can hang its own "engine is alive" watchdog
  // reset off these -- see the hang watchdog in watchdog.c.
  if (!strcmp(name, "onGodotSetupCompleted")) {
    gxLog("engine: setup completed\n");
    jni_note_lifecycle(JNI_LIFECYCLE_SETUP_DONE);
    return 1;
  }
  if (!strcmp(name, "onGodotMainLoopStarted")) {
    gxLog("engine: main loop started\n");
    jni_note_lifecycle(JNI_LIFECYCLE_MAINLOOP_STARTED);
    return 1;
  }
  if (!strcmp(name, "onGodotTerminating")) {
    gxLog("engine: terminating\n");
    jni_note_lifecycle(JNI_LIFECYCLE_TERMINATING);
    return 1;
  }

  // initInputDevices() -- the Java side would enumerate InputDevices and call
  // GodotLib.joyconnectionchanged for each. The wrapper does that itself from
  // input.c against libnx hid, so there is nothing to do here; the call still
  // has to succeed.
  if (!strcmp(name, "initInputDevices")) return 1;

  // restart() -- relaunching the NRO from inside itself is not something
  // homebrew can do cleanly. Treat as a quit request; the Homebrew Menu is
  // where the user lands, and relaunching from there is one button.
  if (!strcmp(name, "restart")) {
    gxLog("restart() requested -> treating as quit\n");
    jni_quit_requested = 1;
    return 1;
  }

  // setKeepScreenOn(Z)V -- the applet is foreground; the system does not
  // dim under it. No-op rather than fiddling with appletSetAutoSleepDisabled,
  // which would fight the user's own power settings.
  if (!strcmp(name, "setKeepScreenOn")) { (void)va_arg(va, int); return 1; }

  // vibrate(II)V -- (durationMs, amplitude). HID rumble is a genuine mapping
  // and worth doing properly; see input.c for the handle set.
  if (!strcmp(name, "vibrate")) {
    int ms  = va_arg(va, int);
    int amp = va_arg(va, int);
    gxLog("vibrate(%d ms, amp %d)\n", ms, amp);
    jni_request_rumble(ms, amp);
    return 1;
  }

  // setClipboard(Ljava/lang/String;)V -- no system clipboard for homebrew.
  // hasClipboard already returns 0 in jni_fake.c, so nothing should reach
  // here; answer anyway so an unguarded caller does not leave a pending
  // exception.
  if (!strcmp(name, "setClipboard")) { (void)va_arg(va, void *); return 1; }

  // Immersive mode / benchmark / editor hooks: all no-ops, all must succeed.
  if (!strcmp(name, "nativeEnableImmersiveMode")) { (void)va_arg(va, int); return 1; }
  if (!strcmp(name, "nativeBeginBenchmarkMeasure") ||
      !strcmp(name, "nativeEndBenchmarkMeasure")) {
    (void)va_arg(va, void *); (void)va_arg(va, void *);
    return 1;
  }
  if (!strcmp(name, "nativeDumpBenchmark") ||
      !strcmp(name, "nativeOnEditorWorkspaceSelected")) {
    (void)va_arg(va, void *);
    return 1;
  }

  // Dialogs. showDialog/showInputDialog/showFilePicker would present Android
  // UI. There is none, and the engine expects an asynchronous callback rather
  // than a return value -- so the correct behaviour is to invoke the callback
  // immediately with a cancel result, NOT to do nothing. Doing nothing leaves
  // whatever asked for the dialog waiting forever.
  //
  // The callbacks are GodotLib.dialogCallback / inputDialogCallback /
  // filePickerCallback, all exported by libgodot_android.so (confirmed in the
  // dynsym list). main.c already resolves GodotLib entry points; route through
  // the same table.
  if (!strcmp(name, "showDialog")) {
    (void)va_arg(va, void *); (void)va_arg(va, void *); (void)va_arg(va, void *);
    gxLog("showDialog -> cancelling immediately\n");
    jni_post_dialog_cancel(JNI_DIALOG_PLAIN);
    return 1;
  }
  if (!strcmp(name, "showInputDialog")) {
    (void)va_arg(va, void *); (void)va_arg(va, void *); (void)va_arg(va, void *);
    gxLog("showInputDialog -> cancelling immediately\n");
    jni_post_dialog_cancel(JNI_DIALOG_INPUT);
    return 1;
  }
  if (!strcmp(name, "showFilePicker")) {
    (void)va_arg(va, void *); (void)va_arg(va, void *);
    (void)va_arg(va, int);    (void)va_arg(va, void *);
    gxLog("showFilePicker -> cancelling immediately\n");
    jni_post_dialog_cancel(JNI_DIALOG_FILE);
    return 1;
  }

  return 0;
}

// --- GodotIO -----------------------------------------------------------------

int godot_io_ext_int(const char *name, int *out, va_list va) {
  if (!name || !out) return 0;

  // setScreenOrientation(I)V is void but lands in the int dispatcher on some
  // paths; harmless either way. The Switch panel does not rotate.
  if (!strcmp(name, "setScreenOrientation")) { (void)va_arg(va, int); *out = 0; return 1; }
  if (!strcmp(name, "hideKeyboard")) { swkbd_hide(); *out = 0; return 1; }

  // openURI(Ljava/lang/String;)I -- OS.shell_open. SM127's login screen links
  // to Level Share Square's sign-up and password-reset pages. Answers with a
  // Godot Error: OK once the page is queued for the browser (web_applet.c).
  if (!strcmp(name, "openURI")) {
    const char *uri = jni_string_utf(va_arg(va, void *));
    *out = web_request(uri) == 0 ? 0 : 1;   // OK / FAILED
    return 1;
  }

  // showKeyboard -- Godot 3.6: (Ljava/lang/String;ZIII)V
  //   (existingText, multiline, maxInputLength, cursorStart, cursorEnd)
  //
  // Godot 4's (Ljava/lang/String;IIII)V has a keyboard type in the second
  // slot. 3.6's multiline flag is 0 or 1 -- the same numbers as DEFAULT and
  // MULTILINE -- so one read serves both. The kind of field (password, email)
  // comes from the helper instead; see sm127_setenv in swkbd_shim.c.
  //
  // Queued rather than run here: swkbdShow blocks and the OS suspends the
  // process while the applet is up, which must not happen on the engine
  // thread mid-frame. swkbd_pump() runs it from the appletMainLoop thread and
  // swkbd_deliver() feeds the result back as key events.
  //
  // cursorStart/cursorEnd are read and discarded: swkbd has no selection
  // model, so a partial selection cannot be honoured. The delivery path diffs
  // the whole string instead, which gives the right end state.
  if (!strcmp(name, "showKeyboard")) {
    const char *existing = jni_string_utf(va_arg(va, void *));
    int type    = va_arg(va, int);
    int max_len = va_arg(va, int);
    (void)va_arg(va, int);   // cursorStart
    (void)va_arg(va, int);   // cursorEnd
    swkbd_request(existing, type, max_len);
    *out = 0;
    return 1;
  }

  return 0;
}

// getScaledDensity()F and getScreenRefreshRate(D)D are NOT here: upstream
// jni_fake.c already answers both in call_float/call_double. My first pass
// listed them as missing because the diff regex only matched
// strcmp(name, ...) and those sites use strcmp(id->name, ...). 28 gaps, not 30.

// --- GodotRenderView ---------------------------------------------------------
//
// Pointer capture and cursor icons. There is no OS cursor; input.c synthesises
// pointer events from touch and (eventually) the right stick. canCapturePointer
// returning false keeps the engine from entering relative-mouse mode, which
// would break the synthesised absolute coordinates.

int godot_renderview_ext(const char *name, int *out, va_list va) {
  if (!name || !out) return 0;

  if (!strcmp(name, "canCapturePointer")) { *out = 0; return 1; }

  if (!strcmp(name, "setPointerIcon")) { (void)va_arg(va, int); *out = 0; return 1; }

  if (!strcmp(name, "configurePointerIcon")) {
    (void)va_arg(va, int);      // shape
    (void)va_arg(va, void *);   // image path
    (void)va_arg(va, double);   // hotspot x  (float promoted through varargs)
    (void)va_arg(va, double);   // hotspot y
    *out = 0;
    return 1;
  }

  return 0;
}

// --- Dictionary --------------------------------------------------------------
//
// org/godotengine/godot/Dictionary marshals a Godot Dictionary across JNI as
// parallel String[]/Object[] arrays. It is reached by FindClass (confirmed in
// the .so string table) and used by the plugin/Callable paths.
//
// StS2 ships no Godot Android plugins -- getGDExtensionConfigFiles is answered
// by jni_fake.c and the GDExtensions load natively through fmod_shim.c -- so
// nothing should construct one. These are here so that if something does, it
// fails visibly rather than by null deref.

// ClassLoader.loadClass / findClass are handled in jni_fake.c's call_object,
// not here: they must return the same named class object j_FindClass vends,
// and find_class_obj is static to that file. Returning NULL from here (the
// first attempt) made Godot's jni_find_class fail for java/lang/Class, the
// reflect/* types and every boxed primitive.

int godot_dictionary_ext(const char *name, void **out) {
  if (!name || !out) return 0;

  if (!strcmp(name, "get_keys") || !strcmp(name, "get_values")) {
    gxLog("Dictionary.%s -- returning empty array (unexpected; "
          "something is using the plugin marshalling path)\n", name);
    *out = jni_new_string_array(0, NULL);
    return 1;
  }
  if (!strcmp(name, "set_keys") || !strcmp(name, "set_values")) {
    gxLog("Dictionary.%s -- ignored\n", name);
    *out = NULL;
    return 1;
  }

  return 0;
}
