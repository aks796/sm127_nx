/* nx_input.c -- Switch touchscreen -> Godot 3.6 touch events.
 *
 * Godot 3.6 replaced 3.5's overloaded GodotLib.touch() with the entry point
 * Godot 4 uses:
 *
 *   GodotLib.dispatchTouchEvent(int event, int pointer, int pointerCount,
 *                               float[] positions, boolean doubleTap)
 *                                                  (JNI: (III[FZ)V)
 *
 * The android.view.InputDevice argument is gone -- 3.6 routes mouse input
 * through its own dispatchMouseEvent instead of muxing both onto one call --
 * and a doubleTap flag is appended.
 *
 * The semantics are still Android MotionEvent, and AndroidInputHandler::
 * process_touch_event keeps its own list of active touches that has to stay
 * in lock-step with what it is sent:
 *
 *   ACTION_DOWN          the first finger; positions = the active set
 *   ACTION_POINTER_DOWN  another finger joins; pointer = its id, and the id
 *                        must also appear in positions (3.6 looks it up there)
 *   ACTION_MOVE          positions must be EXACTLY the active set -- a size
 *                        mismatch is an early return and the move is dropped
 *   ACTION_POINTER_UP    a finger lifts while others stay; pointer = its id,
 *                        matched against the engine's own list
 *   ACTION_UP            the last finger lifts; the engine clears its list
 *
 * positions carries 3 floats per finger: id, x, y (window pixels).
 *
 * The engine turns these into InputEventScreenTouch / ScreenDrag, and since
 * SM127 leaves input_devices/pointing/emulate_mouse_from_touch at its default
 * (on), also into mouse clicks. That matters more here than it would in most
 * games: SM127's menus are mouse-driven, and its level editor binds LMB/RMB
 * directly, so the touchscreen alone can drive the whole interface.
 *
 * The controller equivalent -- for when the Switch is docked, or the player
 * would rather not reach for the screen -- is the cursor in
 * port/switch_port.gd, which synthesises the same mouse events inside Godot.
 *
 * MIT license; see LICENSE. */

#include <string.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "imports.h"
#include "jni_fake.h"
#include "nx_input.h"

// android.view.MotionEvent actions
#define ACTION_DOWN          0
#define ACTION_UP            1
#define ACTION_MOVE          2
#define ACTION_POINTER_DOWN  5
#define ACTION_POINTER_UP    6

// The Switch touch panel reports handheld-screen coordinates, always 1280x720.
#define PANEL_W 1280.0f
#define PANEL_H 720.0f

#define MAX_FINGERS 10

typedef void (*godot_touch_fn)(void *env, void *cls, int ev, int pointer,
                               int pointer_count, void *positions,
                               uint8_t double_tap);

typedef struct { int id; float x, y; } Finger;

static godot_touch_fn s_touch;
static int    s_enabled, s_resolved;
static int    s_screen_w = 1280, s_screen_h = 720;
static Finger s_active[MAX_FINGERS];   // mirrors the engine's touch list
static int    s_nactive;

static void send(int action, int pointer) {
  float pos[MAX_FINGERS * 3];
  for (int i = 0; i < s_nactive; i++) {
    pos[i * 3 + 0] = (float)s_active[i].id;
    pos[i * 3 + 1] = s_active[i].x;
    pos[i * 3 + 2] = s_active[i].y;
  }
  void *arr = jni_new_float_array(s_nactive * 3, pos);
  // doubleTap is always false: the engine only forwards it on
  // InputEventScreenTouch, and nothing in SM127 reads it.
  s_touch(fake_env, jni_activity_class(), action, pointer, s_nactive, arr, 0);
  jni_release_local(arr);
}

static int find_active(int id) {
  for (int i = 0; i < s_nactive; i++)
    if (s_active[i].id == id) return i;
  return -1;
}

void nx_input_init(void) {
  s_enabled = config.touch;
  debugPrintf("[input] touchscreen %s (config touch %d)\n",
              s_enabled ? "ENABLED" : "disabled", config.touch);
}

void nx_input_set_screen(int w, int h) {
  if (w > 0 && h > 0) { s_screen_w = w; s_screen_h = h; }
}

// The system keyboard takes touch too: a key tapped on it must not also tap
// the game behind it, and the finger that tapped OK must not land as a new
// touch when the game comes back.
static int s_suspended, s_wait_clear;

void nx_input_suspend(int on) {
  if (on) {
    while (s_touch && s_nactive > 0) {
      send(s_nactive == 1 ? ACTION_UP : ACTION_POINTER_UP, s_active[s_nactive - 1].id);
      s_nactive--;
    }
    s_nactive = 0;
    s_suspended = 1;
  } else {
    s_suspended = 0;
    s_wait_clear = 1;
  }
}

void nx_input_update(void) {
  if (!s_enabled) return;
  if (!s_resolved) {
    s_resolved = 1;
    s_touch = (godot_touch_fn)sm127_engine_symbol(
        "Java_org_godotengine_godot_GodotLib_dispatchTouchEvent");
    debugPrintf("[input] GodotLib.dispatchTouchEvent %s\n",
                s_touch ? "resolved" : "NOT FOUND -- touch disabled");
  }
  if (!s_touch) return;

  if (s_suspended) return;

  HidTouchScreenState st;
  memset(&st, 0, sizeof(st));
  if (!hidGetTouchScreenStates(&st, 1)) return;
  if (s_wait_clear) {
    if (st.count > 0) return;
    s_wait_clear = 0;
  }

  Finger cur[MAX_FINGERS];
  int ncur = st.count < MAX_FINGERS ? st.count : MAX_FINGERS;
  const float sx = s_screen_w / PANEL_W, sy = s_screen_h / PANEL_H;
  for (int i = 0; i < ncur; i++) {
    cur[i].id = (int)st.touches[i].finger_id;
    cur[i].x  = st.touches[i].x * sx;
    cur[i].y  = st.touches[i].y * sy;
  }

  // 1. Lifts, while the lifting finger is still in the set the engine holds.
  for (int i = s_nactive - 1; i >= 0; i--) {
    int still = 0;
    for (int j = 0; j < ncur; j++) if (cur[j].id == s_active[i].id) { still = 1; break; }
    if (still) continue;
    send(s_nactive == 1 ? ACTION_UP : ACTION_POINTER_UP, s_active[i].id);
    s_active[i] = s_active[--s_nactive];
  }

  // 2. Moves: one MOVE carrying the whole (now unchanged-size) active set.
  int moved = 0;
  for (int j = 0; j < ncur; j++) {
    int i = find_active(cur[j].id);
    if (i < 0) continue;
    if (s_active[i].x != cur[j].x || s_active[i].y != cur[j].y) {
      s_active[i].x = cur[j].x;
      s_active[i].y = cur[j].y;
      moved = 1;
    }
  }
  if (moved && s_nactive) send(ACTION_MOVE, s_active[0].id);

  // 3. New fingers. The finger joins the active set before the send, so
  //    positions carries it -- 3.6's POINTER_DOWN looks the id up in there.
  for (int j = 0; j < ncur && s_nactive < MAX_FINGERS; j++) {
    if (find_active(cur[j].id) >= 0) continue;
    s_active[s_nactive++] = cur[j];
    send(s_nactive == 1 ? ACTION_DOWN : ACTION_POINTER_DOWN, cur[j].id);
  }
}

// No cursor overlay and no button arbitration here; the controller cursor
// lives in port/switch_port.gd. Kept so main.c's call sites stay unconditional.
void nx_input_draw(void) {}
void nx_input_shutdown(void) {}
int nx_input_cursor_visible(void) { return 0; }
uint64_t nx_input_masked_buttons(void) { return 0; }
int nx_input_left_stick_masked(void) { return 0; }
