/* swkbd_shim.c -- GodotIO.showKeyboard backed by libnx swkbd.
 *
 * Why this is not a one-liner:
 *
 *   swkbdShow() runs the system keyboard as a LIBRARY APPLET. It blocks, and
 *   while it runs the calling process is suspended by the OS. Calling it from
 *   the engine thread would suspend the process mid-frame, with the EGL
 *   context current and a GL command stream possibly in flight.
 *
 *   The engine also does not want a return value. On Android, GodotEditText
 *   feeds the edited text back one character at a time through GodotLib.key(),
 *   and DisplayServer's virtual-keyboard callback fires off that. So the
 *   result has to be turned into key events, not handed back from the call.
 *
 * The flow, therefore:
 *
 *   engine thread   GodotIO.showKeyboard  -> queue a request, return at once
 *   main thread     swkbd_pump()          -> swkbdShow() blocks here; queues
 *                                            the result
 *   engine thread   swkbd_deliver()       -> diffs old vs new, emits
 *                                            backspaces then characters
 *
 * swkbd_pump() runs on the thread that owns appletMainLoop(), which is where
 * a library applet transition belongs. swkbd_deliver() runs from the frame
 * loop, on the engine thread, next to jni_drain_dialog_queue().
 *
 * MIT license; see LICENSE. */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "imports.h"
#include "jni_fake.h"
#include "swkbd_shim.h"

#if DEBUG_LOG
#define kbLog(...) debugPrintf("[swkbd] " __VA_ARGS__)
#else
#define kbLog(...) do {} while (0)
#endif

#define KBD_TEXT_MAX 1024

// Godot's VirtualKeyboardType, platform/android/java/.../GodotIO.java.
// Values are stable across 4.x; the mapping to swkbd presets is ours.
enum {
  GD_KEYBOARD_TYPE_DEFAULT = 0,
  GD_KEYBOARD_TYPE_MULTILINE,
  GD_KEYBOARD_TYPE_NUMBER,
  GD_KEYBOARD_TYPE_NUMBER_DECIMAL,
  GD_KEYBOARD_TYPE_PHONE,
  GD_KEYBOARD_TYPE_EMAIL_ADDRESS,
  GD_KEYBOARD_TYPE_PASSWORD,
  GD_KEYBOARD_TYPE_URL,
};

typedef struct {
  int  pending;
  int  type;
  int  max_len;
  char existing[KBD_TEXT_MAX];
} KbdRequest;

typedef struct {
  int  ready;
  int  cancelled;
  char before[KBD_TEXT_MAX];
  char after[KBD_TEXT_MAX];
} KbdResult;

static KbdRequest s_req;
static KbdResult  s_res;
static Mutex      s_lock;

// What the text is never goes into the log -- this keyboard types passwords
// and email addresses -- only how long it is.
static int utf8_len(const char *s);

// The kind of field being edited. Godot 3's showKeyboard passes only the text,
// a multiline flag and a length, so the helper (switch_port.gd) names the field
// as it takes focus: SM127NX_KBD=<kind>|<placeholder>, kind being text, email,
// password or multiline. OS.set_environment arrives here as setenv().
#define KBD_HINT_ENV "SM127NX_KBD"
static char s_hint_kind[16];
static char s_hint_guide[128];

// Once the helper has named a field, it also takes the keyboard's result and
// sets the field's text itself (see publish_to_helper). Typing the result as
// key events could not replace text: on hardware no backspace ever removed a
// character -- a field went 6 -> 26 -> 46 characters across three edits,
// every new string landing in front of the old one.
#define KBD_TEXT_ENV "SM127NX_KBD_TEXT"
#define KBD_SEQ_ENV  "SM127NX_KBD_SEQ"
// "ok" or "cancel". The helper hands focus back from the field either way:
// left focused, SM127's Level Share Square cursor stays frozen until B.
#define KBD_STATUS_ENV "SM127NX_KBD_STATUS"
static int s_helper_seen;

// While the applet is up. hideKeyboard cannot close it from here, and dropping
// the request under it let a second request queue behind it.
static int s_showing;

// Godot 3's LineEdit asks for the keyboard on focus AND on the release of the
// click that focused it. The release can land after the applet has closed,
// which would open a second keyboard straight after the first.
#define KBD_REOPEN_GUARD_MS 400
static u64 s_closed_tick;

int sm127_setenv(const char *name, const char *value, int overwrite) {
  if (name && !strcmp(name, KBD_HINT_ENV)) {
    const char *v = value ? value : "";
    const char *bar = strchr(v, '|');
    const int kind_len = bar ? (int)(bar - v) : (int)strlen(v);
    mutexLock(&s_lock);
    snprintf(s_hint_kind, sizeof(s_hint_kind), "%.*s", kind_len, v);
    snprintf(s_hint_guide, sizeof(s_hint_guide), "%s", bar ? bar + 1 : "");
    s_helper_seen = 1;
    mutexUnlock(&s_lock);
  }
  return setenv(name, value, overwrite);
}

// ---------------------------------------------------------------------------
// engine thread: request
// ---------------------------------------------------------------------------

void swkbd_request(const char *existing, int type, int max_len) {
  mutexLock(&s_lock);

  if (s_req.pending) {
    kbLog("request already pending, ignoring\n");
    mutexUnlock(&s_lock);
    return;
  }
  if (s_closed_tick &&
      armTicksToNs(armGetSystemTick() - s_closed_tick) < KBD_REOPEN_GUARD_MS * 1000000ull) {
    mutexUnlock(&s_lock);
    kbLog("request right after the keyboard closed, ignoring\n");
    return;
  }

  s_req.type    = type;
  s_req.max_len = (max_len > 0 && max_len < KBD_TEXT_MAX) ? max_len : KBD_TEXT_MAX - 1;
  strncpy(s_req.existing, existing ? existing : "", sizeof(s_req.existing) - 1);
  s_req.existing[sizeof(s_req.existing) - 1] = '\0';
  s_req.pending = 1;

  mutexUnlock(&s_lock);
  kbLog("queued: type %d, max %d, %d character(s) already in the field\n",
        type, max_len, utf8_len(s_req.existing));
}

int swkbd_is_pending(void) {
  mutexLock(&s_lock);
  int p = s_req.pending;
  mutexUnlock(&s_lock);
  return p;
}

// ---------------------------------------------------------------------------
// main thread: run the applet
// ---------------------------------------------------------------------------

static void configure_for_type(SwkbdConfig *cfg, int type) {
  switch (type) {
    case GD_KEYBOARD_TYPE_NUMBER:
      swkbdConfigMakePresetDefault(cfg);
      swkbdConfigSetType(cfg, SwkbdType_NumPad);
      break;
    case GD_KEYBOARD_TYPE_NUMBER_DECIMAL:
      swkbdConfigMakePresetDefault(cfg);
      swkbdConfigSetType(cfg, SwkbdType_NumPad);
      swkbdConfigSetLeftOptionalSymbolKey(cfg, ".");
      break;
    case GD_KEYBOARD_TYPE_PHONE:
      swkbdConfigMakePresetDefault(cfg);
      swkbdConfigSetType(cfg, SwkbdType_NumPad);
      swkbdConfigSetLeftOptionalSymbolKey(cfg, "+");
      break;
    case GD_KEYBOARD_TYPE_EMAIL_ADDRESS:
      swkbdConfigMakePresetDefault(cfg);
      swkbdConfigSetLeftOptionalSymbolKey(cfg, "@");
      swkbdConfigSetRightOptionalSymbolKey(cfg, ".");
      break;
    case GD_KEYBOARD_TYPE_PASSWORD:
      swkbdConfigMakePresetPassword(cfg);
      break;
    case GD_KEYBOARD_TYPE_URL:
      swkbdConfigMakePresetDefault(cfg);
      swkbdConfigSetLeftOptionalSymbolKey(cfg, "/");
      swkbdConfigSetRightOptionalSymbolKey(cfg, ".");
      break;
    case GD_KEYBOARD_TYPE_MULTILINE:
      // No true multiline preset; the default with a generous length is the
      // closest thing. Newlines typed into swkbd come back as '\n' and are
      // emitted as ENTER by the delivery path.
      swkbdConfigMakePresetDefault(cfg);
      break;
    default:
      swkbdConfigMakePresetDefault(cfg);
      break;
  }
}

void swkbd_pump(void) {
  KbdRequest req;

  mutexLock(&s_lock);
  if (!s_req.pending || s_res.ready) { mutexUnlock(&s_lock); return; }
  req = s_req;
  mutexUnlock(&s_lock);

  SwkbdConfig cfg;
  Result rc = swkbdCreate(&cfg, 0);
  if (R_FAILED(rc)) {
    kbLog("swkbdCreate failed: 0x%x\n", rc);
    mutexLock(&s_lock);
    s_req.pending = 0;
    s_res.ready = 1; s_res.cancelled = 1;
    mutexUnlock(&s_lock);
    return;
  }

  // Read at show time, not at request time: the helper's hint and the
  // engine's request come from the same focus change on the engine thread.
  char kind[sizeof(s_hint_kind)], guide[sizeof(s_hint_guide)];
  mutexLock(&s_lock);
  memcpy(kind, s_hint_kind, sizeof(kind));
  memcpy(guide, s_hint_guide, sizeof(guide));
  s_showing = 1;
  mutexUnlock(&s_lock);

  int type = req.type;
  if (!strcmp(kind, "password"))   type = GD_KEYBOARD_TYPE_PASSWORD;
  else if (!strcmp(kind, "email")) type = GD_KEYBOARD_TYPE_EMAIL_ADDRESS;

  configure_for_type(&cfg, type);
  if (guide[0]) swkbdConfigSetGuideText(&cfg, guide);
  swkbdConfigSetInitialText(&cfg, req.existing);
  swkbdConfigSetStringLenMax(&cfg, (u32)req.max_len);
  swkbdConfigSetInitialCursorPos(&cfg, 1);   // cursor at end
  swkbdConfigSetBlurBackground(&cfg, 1);

  char out[KBD_TEXT_MAX];
  out[0] = '\0';

  kbLog("showing applet (%s field)...\n", kind[0] ? kind : "unnamed");
  rc = swkbdShow(&cfg, out, sizeof(out));
  swkbdClose(&cfg);

  mutexLock(&s_lock);
  s_closed_tick = armGetSystemTick();
  s_showing = 0;
  s_req.pending = 0;
  strncpy(s_res.before, req.existing, sizeof(s_res.before) - 1);
  s_res.before[sizeof(s_res.before) - 1] = '\0';

  if (R_SUCCEEDED(rc)) {
    strncpy(s_res.after, out, sizeof(s_res.after) - 1);
    s_res.after[sizeof(s_res.after) - 1] = '\0';
    s_res.cancelled = 0;
    kbLog("result: %d character(s)\n", utf8_len(s_res.after));
  } else {
    // User cancelled, or the applet failed. Either way the text is unchanged;
    // emitting nothing is correct.
    s_res.after[0] = '\0';
    s_res.cancelled = 1;
    kbLog("cancelled or failed: 0x%x\n", rc);
  }
  s_res.ready = 1;
  mutexUnlock(&s_lock);
}

// ---------------------------------------------------------------------------
// engine thread: turn the result into key events
// ---------------------------------------------------------------------------

// Android KeyEvent codes -- what GodotLib.key takes in every generation.
// (These were Godot-4-style 0x04000004/5 before, which is neither a Godot 4
// Key value -- SPECIAL is 1<<22 -- nor an Android code.)
#define GD_KEY_BACKSPACE 67   /* KEYCODE_DEL      */
#define GD_KEY_ENTER     66   /* KEYCODE_ENTER    */
#define GD_KEY_MOVE_END  123  /* KEYCODE_MOVE_END; in this engine's key table */

// GodotLib.key. Signature per platform/android/java_godot_lib_jni.cpp:
//
//   void key(JNIEnv*, jclass, jint physical_keycode, jint unicode,
//            jint key_label, jboolean pressed, jboolean echo)
//
// ASSUMPTION, and the most likely thing in this file to be wrong: the
// key_label parameter was added in Godot 4.2 (godotengine/godot#70517). If the
// engine here predates that, this passes one argument too many -- which on
// AAPCS64 is harmless (the extra lands in an unread register) but means
// `pressed` and `echo` arrive in the wrong slots, so keys would repeat or
// never release. If typed characters come out doubled, this is why.
//
// Godot 3.x has a different one (3.6):
//
//   void key(JNIEnv*, jclass, jint keycode, jint scancode, jint unicode,
//            jboolean pressed)
//
// Both take ANDROID key codes (android_get_keysym / godot_code_from_android_
// code convert them), not Godot Key values. Note 3.x treats scancode 4
// (KEYCODE_BACK) as the back button, so it must never be passed by accident.
typedef void (*godot_key_fn)(void *env, void *cls, int physical, int unicode,
                             int label, uint8_t pressed, uint8_t echo);
typedef void (*godot_key3_fn)(void *env, void *cls, int keycode, int scancode,
                              int unicode, uint8_t pressed);

static godot_key_fn s_key = NULL;
static void (*s_set_kbd_height)(void *env, void *cls, int height) = NULL;
static int s_resolved = 0;

static void resolve_key_fns(void) {
  if (s_resolved) return;
  s_resolved = 1;
#define G "Java_org_godotengine_godot_GodotLib_"
  s_key            = (godot_key_fn)sm127_engine_symbol(G "key");
  s_set_kbd_height = (void *)sm127_engine_symbol(G "setVirtualKeyboardHeight");
#undef G
  kbLog("key=%p setVirtualKeyboardHeight=%p\n", (void *)s_key, (void *)s_set_kbd_height);
}

// Minimal UTF-8 decode. swkbd returns UTF-8; GodotLib.key wants a codepoint.
static const char *utf8_next(const char *s, uint32_t *cp) {
  const unsigned char *u = (const unsigned char *)s;
  if (u[0] < 0x80)                { *cp = u[0]; return s + 1; }
  if ((u[0] & 0xE0) == 0xC0 && u[1]) {
    *cp = ((uint32_t)(u[0] & 0x1F) << 6) | (u[1] & 0x3F);
    return s + 2;
  }
  if ((u[0] & 0xF0) == 0xE0 && u[1] && u[2]) {
    *cp = ((uint32_t)(u[0] & 0x0F) << 12) | ((uint32_t)(u[1] & 0x3F) << 6) | (u[2] & 0x3F);
    return s + 3;
  }
  if ((u[0] & 0xF8) == 0xF0 && u[1] && u[2] && u[3]) {
    *cp = ((uint32_t)(u[0] & 0x07) << 18) | ((uint32_t)(u[1] & 0x3F) << 12) |
          ((uint32_t)(u[2] & 0x3F) << 6)  | (u[3] & 0x3F);
    return s + 4;
  }
  *cp = 0xFFFD;
  return s + 1;   // resync
}

static int utf8_len(const char *s) {
  int n = 0;
  uint32_t cp;
  while (*s) { s = utf8_next(s, &cp); n++; }
  return n;
}

static void emit_key(int physical, uint32_t unicode) {
  if (!s_key) return;
  // The engine is Godot 3.x: key(keycode, scancode, unicode, pressed).
  godot_key3_fn k3 = (godot_key3_fn)(void *)s_key;
  void *cls = jni_activity_class();
  k3(fake_env, cls, physical, physical, (int)unicode, 1);  // press
  k3(fake_env, cls, physical, physical, (int)unicode, 0);  // release
}

// The whole result, for the helper (switch_port.gd _kbd_poll): hex in
// SM127NX_KBD_TEXT, because OS.get_environment reads bytes as Latin-1 and this
// is UTF-8; SM127NX_KBD_SEQ changes with every result so an unchanged text
// still counts. Runs on the engine thread, where the helper reads them.
static void publish_to_helper(const char *text, int ok) {
  static const char digits[] = "0123456789abcdef";
  static char hex[2 * KBD_TEXT_MAX + 1];
  static unsigned seq;
  size_t n = 0;
  for (const unsigned char *u = (const unsigned char *)text; *u && n + 2 < sizeof(hex); u++) {
    hex[n++] = digits[*u >> 4];
    hex[n++] = digits[*u & 0xF];
  }
  hex[n] = '\0';
  char num[16];
  snprintf(num, sizeof(num), "%u", ++seq);
  setenv(KBD_TEXT_ENV, hex, 1);
  setenv(KBD_STATUS_ENV, ok ? "ok" : "cancel", 1);
  setenv(KBD_SEQ_ENV, num, 1);
  kbLog("result handed to the helper: %s, %d character(s)\n", ok ? "OK" : "cancelled",
        utf8_len(text));
}

void swkbd_deliver(void) {
  mutexLock(&s_lock);
  if (!s_res.ready) { mutexUnlock(&s_lock); return; }

  KbdResult res = s_res;
  s_res.ready = 0;
  mutexUnlock(&s_lock);

  resolve_key_fns();

  mutexLock(&s_lock);
  const int helper = s_helper_seen;
  mutexUnlock(&s_lock);

  if (res.cancelled) {
    if (helper) publish_to_helper("", 0);
    if (s_set_kbd_height) s_set_kbd_height(fake_env, jni_activity_class(), 0);
    return;
  }

  if (helper) {
    publish_to_helper(res.after, 1);
  } else {
    // No helper: type it. Nothing says the caret is at the end, so put it
    // there, remove the whole old text and type the whole new one.
    const int to_delete = utf8_len(res.before);
    kbLog("delivering as keys: end, %d backspace(s), %d character(s)\n", to_delete,
          utf8_len(res.after));
    emit_key(GD_KEY_MOVE_END, 0);
    for (int i = 0; i < to_delete; i++)
      emit_key(GD_KEY_BACKSPACE, 0);

    const char *p = res.after;
    uint32_t cp;
    while (*p) {
      p = utf8_next(p, &cp);
      if (cp == '\n' || cp == '\r') emit_key(GD_KEY_ENTER, 0);
      else                          emit_key(0, cp);
    }
  }

  // swkbd is a fullscreen modal applet, not an inline keyboard, so it never
  // occludes part of the game view. Reporting 0 stops the engine shifting the
  // UI up to make room for a keyboard that is not there.
  if (s_set_kbd_height) s_set_kbd_height(fake_env, jni_activity_class(), 0);
}

void swkbd_hide(void) {
  mutexLock(&s_lock);
  if (!s_showing) s_req.pending = 0;
  mutexUnlock(&s_lock);
  resolve_key_fns();
  if (s_set_kbd_height) s_set_kbd_height(fake_env, jni_activity_class(), 0);
}
