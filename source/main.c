/* main.c -- Super Mario 127 (Godot 3.6, Android) Switch wrapper entry
 * point.
 *
 * Loads the arm64-v8a libc++_shared.so + libgodot_android.so pair from the
 * game's Android APK (Godot 3.6), provides a minimal Android-like
 * environment (fake JNI, libc/GLES3/EGL import table), owns the EGL/GLES3
 * context, and drives the GodotLib native lifecycle (initialize/setup/
 * newcontext/resize/step) with joypad input from the Switch controllers.
 *
 * The game data is the APK's own assets/ tree, read through the emulated
 * AAssetManager exactly as on a phone. SM127 is pure GDScript, so no GDNative
 * module has to be loaded alongside it (native_modules.c stays idle unless a
 * modded APK brings one). Nothing of the game ships here; it all comes from
 * the user's own APK (scripts/extract_apk.sh).
 *
 * Forked from sts2_nx (MIT; drmariomania_nx / smwr_nx lineage). Its
 * NativeAOT, FMOD/Spine, Vulkan/NVK and ASTC paths are gone.
 *
 * MIT license; see LICENSE. */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <malloc.h>   // mallinfo, for the optional memory trend
#include <switch.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include "config.h"
#include "lock_trace.h"
#include "nx_c11.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "imports.h"
#include "jni_fake.h"
#include "jni_godot_ext.h"
#include "swkbd_shim.h"
#include "web_applet.h"
#include "nx_input.h"
#include "app_paths.h"
#include "c11_probe.h"
#include "crash_handler.h"  // __libnx_exception_handler; no calls, see header
#include "audio_sink.h"
#include "patch.h"
#include "native_modules.h"
#include "clocks.h"
#include "apk_install.h"

so_module cxx_mod, game_mod;

// Read by imports.c's shader tracker and the crash handler.
int s_frames_done_public;

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

// Recorded during heap setup, reported once the log is open.
static size_t s_heap_total = 0;
static size_t s_so_reserve = 0;
static int    s_heap_squeezed = 0;

// Reserve a slice of the process heap for the .so loader; the rest is the
// newlib heap where the engine's malloc lands. Sized from the real binaries
// (readelf -lW, LOAD vaddr span):
//
//   libgodot_android.so  (3.6)                    ~30 MB
//   libc++_shared.so                              ~1 MB
//
// 64 MB leaves generous room for the engine. The native slice at the top holds
// GDNative libraries the engine dlopen()s (native_modules.c); SM127 has none,
// so it normally goes unused.
#define SO_HEAP_RESERVE   (64 * 1024 * 1024)
#define CXX_SO_SLICE      (4 * 1024 * 1024)
#define NATIVE_SO_SLICE   (16 * 1024 * 1024)

size_t g_newlib_heap_size;   // set by __libnx_initheap

void __libnx_initheap(void) {
  void *addr;
  size_t size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  size_t so_reserve = SO_HEAP_RESERVE;
  if (so_reserve > size / 2) {
    // Applet mode (hbmenu from the album) gets a few hundred MB. Printed
    // later, because the symptom is otherwise a so_load failure that points
    // at the wrong thing.
    so_reserve = size / 2;
    s_heap_squeezed = 1;
  }
  s_heap_total = size;
  s_so_reserve = so_reserve;

  extern char *fake_heap_start;
  extern char *fake_heap_end;
  size_t fake_heap_size = size - so_reserve;
  g_newlib_heap_size = fake_heap_size;
  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base = (char *)addr + fake_heap_size;
  heap_so_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base, 0x1000);
  heap_so_limit = (char *)addr + size - (char *)heap_so_base;
}

static int is_dir(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77)) fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78)) fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73)) fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE) fatal_error("Own process handle is unavailable.");
}

// Every probe is built from config.data_root rather than a bare filename:
// under app_paths.c's scan fallback the CWD can be somewhere else entirely.
static void check_probe(const char *rel, const char *hint) {
  char path[352];
  snprintf(path, sizeof(path), "%s/%s", config.data_root, rel);
  struct stat st;
  if (stat(path, &st) < 0)
    fatal_error("Could not find\n%s\nin %s.\n\n%s", rel, config.data_root, hint);
}

static int s_have_pck = 0;

static void check_data(void) {
  static const char *hint =
      "Put the Super Mario 127 Android APK next to sm127.nro\n"
      "and start it again to reinstall the game.";
  check_probe(SO_NAME, hint);
  check_probe(CXX_SO_NAME, hint);

  // Preferred: the assets/ tree packed into one file (extract_apk.sh). Every
  // res:// lookup is then an in-memory PackedData hit and every read comes
  // from pooled handles of one file, instead of a FAT path walk per resource
  // -- which is what scene loads were spending their time on.
  char p[352];
  struct stat st;
  snprintf(p, sizeof(p), "%s/assets/%s", config.data_root, PCK_NAME);
  s_have_pck = (stat(p, &st) == 0);
  if (s_have_pck) {
    debugPrintf("[data] assets/%s: %lld bytes\n", PCK_NAME, (long long)st.st_size);
    return;
  }
  // Otherwise the loose tree, read through FileAccessAndroid -> AAssetManager.
  check_probe("assets/project.binary", hint);
  debugPrintf("[data] no %s; using the loose assets/ tree\n", PCK_NAME);
}

static void set_screen_size(int w, int h) {
  if (w <= 0 || h <= 0 || w > 1920 || h > 1080) {
    if (appletGetOperationMode() == AppletOperationMode_Console) {
      screen_width = 1920; screen_height = 1080;
    } else {
      screen_width = 1280; screen_height = 720;
    }
  } else {
    screen_width = w; screen_height = h;
  }

  // config.resolution_height overrides the display-derived default. The
  // width is computed (16:9, even), never configured.
  if (config.resolution_height > 0) {
    int h2 = config.resolution_height;
    if (h2 < 360) h2 = 360;
    if (h2 > 1080) h2 = 1080;
    int w2 = (h2 * 16 / 9) & ~1;
    debugPrintf(">> render resolution %dx%d (config resolution_height %d)\n",
                w2, h2, config.resolution_height);
    screen_width = w2; screen_height = h2;
  }
}

// ---------------------------------------------------------------------------
// EGL / GLES3 context (mesa). Godot's android GL path expects an external
// context that is current on the thread that calls step(), so the wrapper
// owns it, exactly like the Java GLSurfaceView does on Android.
// ---------------------------------------------------------------------------

static EGLDisplay s_dpy = EGL_NO_DISPLAY;
static EGLSurface s_surf = EGL_NO_SURFACE;
static EGLContext s_ctx = EGL_NO_CONTEXT;

#ifndef EGL_OPENGL_ES3_BIT
#define EGL_OPENGL_ES3_BIT 0x0040
#endif

static int egl_setup(void) {
  s_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (s_dpy == EGL_NO_DISPLAY) return -1;
  if (eglInitialize(s_dpy, NULL, NULL) == EGL_FALSE) return -2;
  if (eglBindAPI(EGL_OPENGL_ES_API) == EGL_FALSE) return -3;

  const EGLint cfg_attr[] = {
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
    EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
    EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
    EGL_NONE
  };
  EGLConfig cfg;
  EGLint num = 0;
  if (eglChooseConfig(s_dpy, cfg_attr, &cfg, 1, &num) == EGL_FALSE || num < 1)
    return -4;

  NWindow *win = nwindowGetDefault();
  nwindowSetDimensions(win, screen_width, screen_height);
  s_surf = eglCreateWindowSurface(s_dpy, cfg, (EGLNativeWindowType)win, NULL);
  if (s_surf == EGL_NO_SURFACE) return -5;

  const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
  s_ctx = eglCreateContext(s_dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
  if (s_ctx == EGL_NO_CONTEXT) return -6;
  return 0;
}

// ---------------------------------------------------------------------------
// GodotLib native entry points, Godot 3.6 (java_godot_lib_jni.h).
//
// Two of these differ from 3.5.x, which is why this wrapper refuses an older
// engine in resolve_entry_points():
//
//   setup()  gained a GodotTTS argument (TTS_Android::setup caches its method
//            IDs from it) and now RETURNS whether Main::setup succeeded, so a
//            bad project no longer fails silently into a black screen.
//   touch()  was replaced by dispatchTouchEvent(); see nx_input.c.
//
// initialize() is unchanged from 3.5: the Activity comes first, and the two
// file handlers are the ones jni_fake.c answers for.
// ---------------------------------------------------------------------------

typedef uint8_t jboolean;

static int      (*e_JNI_OnLoad)(void *vm, void *reserved);
static void     (*e_initialize)(void *env, void *cls, void *activity, void *godot,
                                void *asset_mgr, void *io, void *net_utils,
                                void *dir_handler, void *file_handler,
                                jboolean use_apk_expansion);
static jboolean (*e_setup)(void *env, void *cls, void *cmdline_array, void *tts);
static void     (*e_resize)(void *env, void *cls, int w, int h);
static void     (*e_newcontext)(void *env, void *cls);
static jboolean (*e_step)(void *env, void *cls);
static void     (*e_ondestroy)(void *env, void *cls);
static void     (*e_joybutton)(void *env, void *cls, int device, int button, jboolean pressed);
static void     (*e_joyaxis)(void *env, void *cls, int device, int axis, float value);
static void     (*e_joyconnectionchanged)(void *env, void *cls, int device, jboolean connected, void *name);
static void     (*e_focusin)(void *env, void *cls);
static void     (*e_focusout)(void *env, void *cls);
static void     (*e_onRendererResumed)(void *env, void *cls);
static void     (*e_onRendererPaused)(void *env, void *cls);

#define G "Java_org_godotengine_godot_GodotLib_"

static void resolve_entry_points(void) {
  // Two guards, because calling either wrong engine would crash somewhere
  // far less explainable than here.
  //
  // callobject is 3.x's JNISingleton bridge, gone in 4.0 (whose initialize()
  // takes different arguments entirely).
  if (!so_try_find_addr_rx(&game_mod, G "callobject"))
    fatal_error("%s is not a Godot 3.x engine.\n\n"
                "This wrapper is built for Super Mario 127's Android APK\n"
                "(Godot 3.6). Use that APK's lib/arm64-v8a files.",
                SO_NAME);

  // dispatchTouchEvent exists only from 3.6: 3.5 named it touch() and its
  // setup() neither took a GodotTTS nor returned a result. Calling a 3.5
  // setup() through the 3.6 prototype would read a garbage return value and
  // report a bogus failure, so refuse it by name instead.
  if (!so_try_find_addr_rx(&game_mod, G "dispatchTouchEvent"))
    fatal_error("%s is a Godot 3.5 or older engine.\n\n"
                "Super Mario 127 0.9.1 is built with Godot 3.6, and this\n"
                "wrapper follows 3.6's native interface. Use the\n"
                "lib/arm64-v8a files from the SM127 APK.",
                SO_NAME);

  e_JNI_OnLoad           = (void *)so_try_find_addr_rx(&game_mod, "JNI_OnLoad");
  e_initialize           = (void *)so_find_addr_rx(&game_mod, G "initialize");
  e_setup                = (void *)so_find_addr_rx(&game_mod, G "setup");
  e_resize               = (void *)so_find_addr_rx(&game_mod, G "resize");
  e_newcontext           = (void *)so_find_addr_rx(&game_mod, G "newcontext");
  e_step                 = (void *)so_find_addr_rx(&game_mod, G "step");
  e_ondestroy            = (void *)so_try_find_addr_rx(&game_mod, G "ondestroy");
  e_joybutton            = (void *)so_try_find_addr_rx(&game_mod, G "joybutton");
  e_joyaxis              = (void *)so_try_find_addr_rx(&game_mod, G "joyaxis");
  e_joyconnectionchanged = (void *)so_try_find_addr_rx(&game_mod, G "joyconnectionchanged");
  e_focusin              = (void *)so_try_find_addr_rx(&game_mod, G "focusin");
  e_focusout             = (void *)so_try_find_addr_rx(&game_mod, G "focusout");
  e_onRendererResumed    = (void *)so_try_find_addr_rx(&game_mod, G "onRendererResumed");
  e_onRendererPaused     = (void *)so_try_find_addr_rx(&game_mod, G "onRendererPaused");
}

// ---------------------------------------------------------------------------
// input: Switch pad -> Godot 3 joypad buttons/axes.
//
// The fake Java side reports no input fallback mapping and the pad's GUID is
// in no controller DB, so InputDefault runs the pad UNMAPPED (mapping == -1)
// and takes our indices as its own JoystickList enum verbatim (3.6
// InputDefault::joy_button). So these are Godot 3's canonical values
// (core/os/input_event.h), with the Switch LABEL layout -- A confirms and
// jumps, B backs out.
// ---------------------------------------------------------------------------

#define GD3_AXIS_LX 0
#define GD3_AXIS_LY 1
#define GD3_AXIS_RX 2
#define GD3_AXIS_RY 3
#define GD3_AXIS_L2 6   // JOY_ANALOG_L2
#define GD3_AXIS_R2 7   // JOY_ANALOG_R2

static PadState pad;

// ZL/ZR go out as both button (JOY_L2/R2) and trigger axis, as a real pad's do.
static const struct { u64 sw; int btn; } s_btnmap[] = {
  { HidNpadButton_A,      0 },    // JOY_BUTTON_0
  { HidNpadButton_B,      1 },    // JOY_BUTTON_1
  { HidNpadButton_X,      2 },    // JOY_BUTTON_2
  { HidNpadButton_Y,      3 },    // JOY_BUTTON_3
  { HidNpadButton_L,      4 },    // JOY_L
  { HidNpadButton_R,      5 },    // JOY_R
  { HidNpadButton_ZL,     6 },    // JOY_L2
  { HidNpadButton_ZR,     7 },    // JOY_R2
  { HidNpadButton_StickL, 8 },    // JOY_L3
  { HidNpadButton_StickR, 9 },    // JOY_R3
  { HidNpadButton_Minus,  10 },   // JOY_SELECT
  { HidNpadButton_Plus,   11 },   // JOY_START
  { HidNpadButton_Up,     12 },   // JOY_DPAD_UP
  { HidNpadButton_Down,   13 },   // JOY_DPAD_DOWN
  { HidNpadButton_Left,   14 },   // JOY_DPAD_LEFT
  { HidNpadButton_Right,  15 },   // JOY_DPAD_RIGHT
};

static u64 s_prev_buttons = 0;
static float s_prev_axis[8] = { 99, 99, 99, 99, 99, 99, 99, 99 }; // force initial send

// A system applet (the keyboard, the browser) is drawn over the game while the
// game keeps running, and on hardware the game went on receiving the buttons
// pressed inside it: typing with A and B clicked Level Share Square behind the
// keyboard, loaded pages and queued a second keyboard. The main thread bumps
// this before and after each applet, so it is odd while one is up.
static volatile u32 s_applet_gen = 0;
static u32 s_applet_seen = 0;
// Buttons still held when an applet closes (the A that pressed OK) stay out of
// the game until they are let go, so they never arrive as a fresh press.
static u64 s_held_back = 0;

// A resting axis reads exactly 0, per axis, the way Android's input stack
// reports a gamepad (InputDevice.MotionRange.getFlat()) -- which is what SM127
// was built and tested against. libnx gives the raw calibrated position, which
// jitters by a few hundredths at rest, so nearly every poll sent a fresh event
// for the axis nobody was touching. SM127's shine select reads
// Input.is_action_just_pressed() inside _input(), which runs once per event, so
// in the frame a stick push fired ui_left/ui_right, that stray event made it
// move twice. Every action in the game has its own deadzone of 0.25-0.5 on top
// of this, so nothing players can bind or feel changes.
#define STICK_FLAT 0.08f

static float stick_norm(s32 v) {
  float f = v / 32767.0f;
  if (f > 1.0f) f = 1.0f;
  if (f < -1.0f) f = -1.0f;
  if (f > -STICK_FLAT && f < STICK_FLAT) f = 0.0f;
  return f;
}

static void send_axis(void *cls, int axis, float v) {
  if (v == s_prev_axis[axis]) return;
  s_prev_axis[axis] = v;
  if (e_joyaxis) e_joyaxis(fake_env, cls, 0, axis, v);
}

static void poll_input(void) {
  void *cls = jni_activity_class();

  // An applet opened or closed since the last poll: release everything the
  // game holds. While one is up nothing is read; after it, what is still held
  // is held back.
  const u32 gen = s_applet_gen;
  if (gen != s_applet_seen) {
    s_applet_seen = gen;
    if (e_joybutton)
      for (unsigned i = 0; i < sizeof(s_btnmap) / sizeof(*s_btnmap); i++)
        if (s_prev_buttons & s_btnmap[i].sw) e_joybutton(fake_env, cls, 0, s_btnmap[i].btn, 0);
    s_prev_buttons = 0;
    for (int a = 0; a < 8; a++)
      if (s_prev_axis[a] != 99) send_axis(cls, a, 0.0f);
    nx_input_suspend(gen & 1);
    s_held_back = ~0ull;   // narrowed to what is actually held at the next read
    debugPrintf("[input] %s\n", (gen & 1) ? "system applet up: game input released and held back"
                                           : "system applet closed: input back to the game");
  }
  if (gen & 1) return;

  padUpdate(&pad);
  const u64 cur = padGetButtons(&pad);

  // Buttons nx_pointer has claimed are held low here, so one press does not
  // both click and send a gamepad button. Masking the STATE makes the edges
  // come out right across a toggle.
  const u64 held = cur & ~nx_input_masked_buttons();
  s_held_back &= held;
  const u64 masked = held & ~s_held_back;

  if (e_joybutton) {
    // The first presses are logged, so a hardware log shows input left the
    // wrapper -- which separates "not sent" from "sent but not acted on".
    static int s_logged;
    for (unsigned i = 0; i < sizeof(s_btnmap) / sizeof(*s_btnmap); i++) {
      const u64 m = s_btnmap[i].sw;
      const int down = (masked & m) && !(s_prev_buttons & m);
      const int up   = !(masked & m) && (s_prev_buttons & m);
      if (!down && !up) continue;
      e_joybutton(fake_env, cls, 0, s_btnmap[i].btn, down);
      if (down && s_logged < 16) {
        s_logged++;
        debugPrintf("[pad] joybutton %d pressed\n", s_btnmap[i].btn);
      }
    }
  }

  // sticks: godot's android convention is Y-down-positive
  HidAnalogStickState l = padGetStickPos(&pad, 0);
  HidAnalogStickState r = padGetStickPos(&pad, 1);
  const int lmask = nx_input_left_stick_masked();
  send_axis(cls, GD3_AXIS_LX, lmask ? 0.0f : stick_norm(l.x));
  send_axis(cls, GD3_AXIS_LY, lmask ? 0.0f : -stick_norm(l.y));
  send_axis(cls, GD3_AXIS_RX, stick_norm(r.x));
  send_axis(cls, GD3_AXIS_RY, -stick_norm(r.y));
  // ZL/ZR are digital on Switch; full-scale is what a trigger action with a
  // 0.5 deadzone expects.
  send_axis(cls, GD3_AXIS_L2, (masked & HidNpadButton_ZL) ? 1.0f : 0.0f);
  send_axis(cls, GD3_AXIS_R2, (masked & HidNpadButton_ZR) ? 1.0f : 0.0f);

  s_prev_buttons = masked;

  // Touchscreen (config.touch) -> Godot 3 touch events (nx_input.c).
  nx_input_update();
}

// ---------------------------------------------------------------------------
// game thread (owns the EGL context and the whole GodotLib lifecycle)
// ---------------------------------------------------------------------------

static Thread s_game_thread;
static volatile int s_game_running = 1;
static volatile int s_focused = 1;
static volatile int s_frames_done = 0; // step() iterations completed (watchdog)

// Steps that must complete before the watchdog will pause-and-dump. Startup
// loading runs inside a step and can legitimately take tens of seconds.
#define WATCHDOG_ARM_STEPS 120

// boot phase timings + gameplay stalls, written to <data_root>/boot_stats.txt
static u64 s_t_boot;
static FILE *s_stats;

// The build this NRO actually is.
//
// This used to be __DATE__ " " __TIME__, which is baked when main.c compiles --
// so a build that only touched imports.c or clocks.c reported the timestamp of
// an older one. Two hardware reports in a row carried a stamp from a build that
// was not the one running, which is worse than no stamp at all. The NRO's own
// mtime cannot drift from the file being executed.
static const char *build_id(void) {
  static char s[64];
  if (s[0]) return s;
  char path[512];
  // app_paths_data_root(), not config.data_root: this runs while boot_stats is
  // being opened, before config is populated, which is why the first build to
  // carry this still printed the compile-time fallback.
  snprintf(path, sizeof(path), "%s/%s.nro", app_paths_data_root(), APP_NAME);
  struct stat st;
  if (stat(path, &st) == 0) {
    struct tm tm;
    if (gmtime_r(&st.st_mtime, &tm))
      strftime(s, sizeof(s), "%Y-%m-%d %H:%M:%SZ", &tm);
  }
  if (!s[0]) snprintf(s, sizeof(s), "%s %s (compile time)", __DATE__, __TIME__);
  return s;
}

static void stats_open(void) {
  char p[300];
  snprintf(p, sizeof(p), "%s/boot_stats.txt", config.data_root);
  s_stats = fopen(p, "w");
  s_t_boot = armGetSystemTick();
  if (s_stats) {
    fprintf(s_stats, "build %s\n", build_id());
    fflush(s_stats);
  }
}

// boot_stats.txt lines are queued and written by main()'s loop, like the log.
// The stall and pacing lines are produced ON the frame thread, and each one
// used to fflush() to the SD card right there -- after a slow frame, making it
// slower, and every 30 s during play.
static char   s_stats_q[8192];
static size_t s_stats_qlen;
static Mutex  s_stats_lock;

static void stats_pump(void) {
  if (!s_stats) return;
  char out[sizeof(s_stats_q)];
  mutexLock(&s_stats_lock);
  const size_t n = s_stats_qlen;
  memcpy(out, s_stats_q, n);
  s_stats_qlen = 0;
  mutexUnlock(&s_stats_lock);
  if (n) {
    fwrite(out, 1, n, s_stats);
    fflush(s_stats);
  }
}

static void stats_emit(const char *fmt, ...) {
  if (!s_stats) return;
  char line[512];
  va_list ap;
  va_start(ap, fmt);
  const int w = vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  if (w <= 0) return;
  const size_t len = (size_t)w < sizeof(line) ? (size_t)w : sizeof(line) - 1;
  mutexLock(&s_stats_lock);
  if (s_stats_qlen + len <= sizeof(s_stats_q)) {
    memcpy(s_stats_q + s_stats_qlen, line, len);
    s_stats_qlen += len;
  }
  mutexUnlock(&s_stats_lock);
  if (threadGetCurHandle() == envGetMainThreadHandle()) stats_pump();
}

static void stats_mark(const char *what) {
  const u64 ms = armTicksToNs(armGetSystemTick() - s_t_boot) / 1000000ull;
  stats_emit("%7llu ms  %s\n", (unsigned long long)ms, what);
}

// ---------------------------------------------------------------------------
// Mesa glthread (EXPERIMENTAL, config glthread 1).
//
// The hardware logs show this Godot 3 build CPU-bound on the frame thread,
// and a large share of that is the GL driver. glthread queues GL calls and
// has a Mesa worker thread execute them, so the driver's CPU work moves to
// another core. switch-mesa 20.1 compiles it in but has no driconf/env switch
// to turn it on, so it is enabled directly on the current context.
//
// Mesa's worker comes from plain pthread_create -- not through imports.c's
// shim -- so it lands on the process default core, core 0. That is why the
// frame thread lives on core 1 (see main()): the two then really run in
// parallel.
//
// eglSwapBuffers talks to the driver directly, so every queued call must be
// executed first: glthread_sync() before each swap.
// ---------------------------------------------------------------------------

extern void *_glapi_get_context(void);
extern void _mesa_glthread_init(void *ctx);
extern void _mesa_glthread_finish(void *ctx);
extern void _mesa_glthread_destroy(void *ctx);

static void *s_glthread_ctx;

static void glthread_start(void) {
  if (!config.glthread) return;
  void *ctx = _glapi_get_context();
  if (!ctx) {
    debugPrintf("[gl] glthread: no current context, not enabled\n");
    return;
  }
  _mesa_glthread_init(ctx);
  s_glthread_ctx = ctx;
  debugPrintf("[gl] glthread ENABLED (experimental)\n");
}

static void glthread_sync(void) {
  if (s_glthread_ctx) _mesa_glthread_finish(s_glthread_ctx);
}

static void glthread_stop(void) {
  if (!s_glthread_ctx) return;
  _mesa_glthread_destroy(s_glthread_ctx);
  s_glthread_ctx = NULL;
}

static void game_thread_fn(void *arg) {
  (void)arg;
  tls_setup_guard(); // bionic stack canary from tpidr_el0+0x28
  g_sm127_game_thread = threadGetCurHandle();   // imports.c's wait accounting

  eglMakeCurrent(s_dpy, s_surf, s_surf, s_ctx);
  eglSwapInterval(s_dpy, 1);   // vsync paces the loop, like GLSurfaceView
  glthread_start();

  void *cls = jni_activity_class();

  if (e_JNI_OnLoad) {
    debugPrintf(">> JNI_OnLoad...\n");
    e_JNI_OnLoad(fake_vm, NULL);
  }

  debugPrintf(">> GodotLib.initialize...\n");
  e_initialize(fake_env, cls, jni_activity_object(),
               jni_godot_object(), jni_assetmgr_object(),
               jni_godot_io_object(), jni_netutils_object(),
               jni_dirhandler_object(), jni_filehandler_object(),
               0 /* use_apk_expansion */);
  stats_mark("GodotLib.initialize");

  const char *args[8];
  int nargs = 0;
  if (s_have_pck) {
    // res:// so it opens through FileAccessAndroid (plain C calls into
    // godot_shim.c's AAssetManager, which pools the pack's handles) rather
    // than the JNI file handler. Godot then reads res://override.cfg -- the
    // controller bindings -- from the same assets/ folder.
    args[nargs++] = "--main-pack";
    args[nargs++] = "res://" PCK_NAME;
  }
  // The renderer follows the project (rendering/quality/driver/driver_name)
  // and the GLES version jni_fake reports (3.2). `gles2 1` forces GLES2.
  if (config.gles2) {
    args[nargs++] = "--video-driver";
    args[nargs++] = "GLES2";
  }
  // --verbose is opt-in (config.txt `verbose 1`): it logs every resource
  // load to the SD card. Engine errors and warnings are printed without it.
  if (config.verbose)
    args[nargs++] = "--verbose";
  void *cmdline = jni_new_string_array(nargs, args);

  debugPrintf(">> GodotLib.setup...\n");
  // 3.6 returns false when Main::setup() failed -- a missing/corrupt pack, a
  // project.binary the engine will not parse, or a video driver that would not
  // start. Without this the engine carries on into step() and the player just
  // gets a black screen, so stop here with something readable instead.
  if (!e_setup(fake_env, cls, cmdline, jni_tts_object()))
    fatal_error("The engine could not start the game.\n\n"
                "Main::setup() failed -- the game data next to the NRO is\n"
                "missing or does not match the engine.\n\n"
                "Re-run scripts/extract_apk.sh with your SM127 APK, and\n"
                "see %s for the engine's own error.",
                LOG_NAME);
  stats_mark("GodotLib.setup (project+drivers)");

  // What the GLSurfaceView renderer thread does on Android: onSurfaceCreated
  // -> newcontext, onSurfaceChanged -> resize, then onDrawFrame -> step.
  debugPrintf(">> newcontext/resize (%dx%d)...\n", screen_width, screen_height);
  e_newcontext(fake_env, cls);
  e_resize(fake_env, cls, screen_width, screen_height);

  debugPrintf(">> entering step loop\n");
  int frames = 0;
  int paused = 0;
  int announced_pad = 0;
  // Frame pacing, measured between presents and summarised into
  // boot_stats.txt every ~30 s: a present more than 20 ms after the last one
  // missed a vblank (the stutter a player sees), more than 34 ms missed two.
  u64 last_present = 0;
  u64 win_frames = 0, win_us = 0, win_slow = 0, win_very_slow = 0, win_worst = 0;
  // Engine time per frame (GodotLib.step: scripts, physics, building the draw
  // lists) against the whole present-to-present interval, so a slow window says
  // whether the CPU ran out of frame or the swap/GPU side did.
  u64 win_cpu_us = 0;
  // Of that engine time: sleeping (Godot's frame limiter) and waiting inside GL
  // calls that block on the GPU. Snapshots of imports.c's running totals.
  u64 win_sleep0 = g_sm127_sleep_us, win_gl0 = g_sm127_glwait_us;
  // The same figures over ~5 s, into the debug log rather than boot_stats.txt,
  // so they interleave with the helper's [port] trace lines (Mario's height,
  // draw calls) and a slowdown can be tied to where in a level it happened.
  u64 p_frames = 0, p_us = 0, p_cpu_us = 0, p_slow = 0;
  u64 p_sleep0 = g_sm127_sleep_us, p_gl0 = g_sm127_glwait_us;

  while (s_game_running && !jni_quit_requested) {
    if (!s_focused) {
      if (!paused) {
        if (e_focusout) e_focusout(fake_env, cls);
        if (e_onRendererPaused) e_onRendererPaused(fake_env, cls);
        paused = 1;
      }
      svcSleepThread(16 * 1000 * 1000);
      continue;
    }
    if (paused) {
      if (e_onRendererResumed) e_onRendererResumed(fake_env, cls);
      if (e_focusin) e_focusin(fake_env, cls);
      paused = 0;
      last_present = 0;   // the time away is not a stutter
    }

    if (announced_pad) poll_input();

    if (frames < 8) debugPrintf(">> step %d begin\n", frames + 1);
    const u64 t0 = armGetSystemTick();
    // Deferred JNI work, on the engine thread between frames: dialog
    // callbacks, rumble expiry, software-keyboard results.
    jni_drain_dialog_queue();
    jni_rumble_tick();
    swkbd_deliver();

    const jboolean drew = e_step(fake_env, cls);
    const u64 t_stepped = armGetSystemTick();
    if (frames < 8) debugPrintf(">> step %d done (swap %d)\n", frames + 1, (int)drew);

    if (config.mem_trend && (frames % 300) == 0) {
      uint64_t total = 0, used = 0;
      svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
      svcGetInfo(&used,  InfoType_UsedMemorySize,  CUR_PROCESS_HANDLE, 0);
      struct mallinfo mi = mallinfo();
      debugPrintf("[mem] frame %d: process %llu/%llu MB, newlib arena %u MB, "
                  "in use %u MB, free %u MB\n", frames,
                  (unsigned long long)(used >> 20), (unsigned long long)(total >> 20),
                  (unsigned)(mi.arena >> 20), (unsigned)(mi.uordblks >> 20),
                  (unsigned)(mi.fordblks >> 20));
      jni_report();
      debugFlush();
    }

    if (frames == 8) {
      lock_trace_report();
      sm127_c11_report();
      if (!config.eager_log) debugSetEagerFlush(0);
    }

    // Present only when the engine drew: step() returns should_swap_buffers,
    // which is exactly what GLSurfaceView's onDrawFrame result means on
    // Android. Swapping an undrawn buffer would flash stale contents. Without
    // a swap there is no vsync wait, so yield a little instead of spinning.
    if (drew) {
      glthread_sync();   // queued GL work must reach the driver before the swap
      nx_input_draw();
      eglSwapBuffers(s_dpy, s_surf);
    } else {
      svcSleepThread(2 * 1000 * 1000);
    }
    const u64 now = armGetSystemTick();
    const u64 step_ms = armTicksToNs(now - t0) / 1000000ull;

    if (step_ms > 100 && announced_pad && s_stats && frames < 100000)
      stats_emit("stall %4llu ms  frame %d\n", (unsigned long long)step_ms, frames);

    if (drew && announced_pad) {
      if (last_present) {
        const u64 us = armTicksToNs(now - last_present) / 1000ull;
        clocks_frame(us > 20000);   // the automatic CPU clock's input
        win_frames++;
        win_us += us;
        win_cpu_us += armTicksToNs(t_stepped - t0) / 1000ull;
        p_frames++;
        p_us += us;
        p_cpu_us += armTicksToNs(t_stepped - t0) / 1000ull;
        if (us > 20000) p_slow++;
        if (us > 20000) win_slow++;
        if (us > 34000) win_very_slow++;
        if (us > win_worst) win_worst = us;
      }
      last_present = now;
      if (win_frames >= 1800 && s_stats) {
        const u64 slept = g_sm127_sleep_us - win_sleep0;
        const u64 glwait = g_sm127_glwait_us - win_gl0;
        stats_emit("pacing  frame %d: %llu frames, avg %.2f ms (%.1f fps), "
                   "engine %.2f ms/frame (slept %.2f, GL wait %.2f), missed vblank %llu, "
                   "missed 2+ %llu, worst %llu ms, cpu %d MHz\n", frames,
                   (unsigned long long)win_frames, win_us / 1000.0 / win_frames,
                   1e6 * win_frames / (double)win_us,
                   win_cpu_us / 1000.0 / win_frames,
                   slept / 1000.0 / win_frames, glwait / 1000.0 / win_frames,
                   (unsigned long long)win_slow, (unsigned long long)win_very_slow,
                   (unsigned long long)(win_worst / 1000), clocks_cpu_mhz());
        win_frames = win_us = win_slow = win_very_slow = win_worst = win_cpu_us = 0;
        win_sleep0 = g_sm127_sleep_us;
        win_gl0 = g_sm127_glwait_us;
      }
      if (p_frames >= 300) {
        debugPrintf("[pace] %.1f fps, engine %.2f ms (slept %.2f, GL wait %.2f), "
                    "missed %llu/%llu, cpu %d MHz\n",
                    1e6 * p_frames / (double)p_us, p_cpu_us / 1000.0 / p_frames,
                    (g_sm127_sleep_us - p_sleep0) / 1000.0 / p_frames,
                    (g_sm127_glwait_us - p_gl0) / 1000.0 / p_frames,
                    (unsigned long long)p_slow, (unsigned long long)p_frames, clocks_cpu_mhz());
        p_frames = p_us = p_cpu_us = p_slow = 0;
        p_sleep0 = g_sm127_sleep_us;
        p_gl0 = g_sm127_glwait_us;
      }
    }

    frames++;
    s_frames_done = frames;
    s_frames_done_public = frames;
    if (frames == 1) stats_mark("step 1 (Main::setup2)");
    // ~3 s into the main loop the title screen is up: the loading clock
    // hands over to the gameplay one (stock by default) on the next tick.
    if (frames == 180) {
      clocks_loading_done();
      stats_mark("loading done (gameplay clocks)");
    }
    if (frames == 2) stats_mark("step 2 (Main::start -- game scene loaded)");
    if (!announced_pad && frames >= 4) {
      // Godot 3 drops joypad events until step 1 has run; announce once the
      // main loop is up.
      if (e_joyconnectionchanged) {
        void *name = jni_new_string("Nintendo Switch Controller");
        e_joyconnectionchanged(fake_env, cls, 0, 1, name);
        jni_release_local(name);
      }
      announced_pad = 1;
      debugPrintf(">> pad announced after %d frames\n", frames);
    }
  }

  debugPrintf(">> leaving step loop (running=%d quit=%d)\n", s_game_running, jni_quit_requested);
  if (e_ondestroy) e_ondestroy(fake_env, cls);
  glthread_stop();
  s_game_running = 0;
}

// ---------------------------------------------------------------------------
// hang watchdog: when the game thread stops completing steps, pause it and
// dump PC/LR plus an FP-chain backtrace so the stall site lands in the log.
// ---------------------------------------------------------------------------

static void log_code_addr(const char *tag, uint64_t a) {
  so_module *m = so_find_module_by_addr((const void *)(uintptr_t)a);
  if (m) {
    debugPrintf("[watchdog]   %s %016llx  %s+0x%llx\n", tag,
                (unsigned long long)a, m->name,
                (unsigned long long)(a - (uintptr_t)m->load_virtbase));
    return;
  }
  // The wrapper itself: offsets usable with `addr2line -e sm127.elf`.
  const uint64_t wrap_base = (uint64_t)sm127_wrapper_base();
  if (a >= wrap_base && a < wrap_base + 0x800000)
    debugPrintf("[watchdog]   %s %016llx  sm127+0x%llx\n", tag,
                (unsigned long long)a, (unsigned long long)(a - wrap_base));
  else
    debugPrintf("[watchdog]   %s %016llx\n", tag, (unsigned long long)a);
}

// Capture while paused, print after resuming -- NEVER both at once. The game
// thread is usually inside debugPrintf holding the log lock when paused, so
// logging from the pause window deadlocks.
static void watchdog_dump_thread(Thread *t, const char *what) {
  if (R_FAILED(threadPause(t))) {
    debugPrintf("[watchdog] could not pause %s\n", what);
    return;
  }

  ThreadContext ctx;
  uint64_t frame[12];
  int nframes = 0;
  const Result rc = svcGetThreadContext3(&ctx, t->handle);

  if (R_SUCCEEDED(rc)) {
    uint64_t fp = ctx.fp;
    const uint64_t sp = ctx.sp;
    for (int i = 0; i < 12; i++) {
      if (fp < sp || fp > sp + (16ull << 20) || (fp & 7)) break;
      const uint64_t next = *(const uint64_t *)fp;
      const uint64_t ret  = *(const uint64_t *)(fp + 8);
      if (!ret) break;
      frame[nframes++] = ret;
      if (next <= fp) break;
      fp = next;
    }
  }

  threadResume(t);   // BEFORE any logging

  if (R_FAILED(rc)) {
    debugPrintf("[watchdog] svcGetThreadContext3(%s) failed: %08x\n", what, rc);
    return;
  }

  debugPrintf("[watchdog] %s:\n", what);
  log_code_addr("PC", ctx.pc.x);
  log_code_addr("LR", ctx.lr);
  for (int i = 0; i < nframes; i++) {
    char tag[8];
    snprintf(tag, sizeof(tag), "#%d", i);
    log_code_addr(tag, frame[i]);
  }
}

// Not static: the crash handler calls it too.
void sm127_dump_all_threads(void) {
  debugPrintf("[watchdog] bases: godot=%p libc++=%p\n",
              game_mod.load_virtbase, cxx_mod.load_virtbase);
  watchdog_dump_thread(&s_game_thread, "game thread");

  Thread *thr[16];
  void *entry[16];
  int n = sm127_engine_threads(thr, entry, 16);
  for (int i = 0; i < n; i++) {
    char what[64];
    snprintf(what, sizeof(what), "engine thread %d (entry godot+0x%lx)", i,
             (unsigned long)((uintptr_t)entry[i] - (uintptr_t)game_mod.load_virtbase));
    watchdog_dump_thread(thr[i], what);
  }
}

static void load_module(so_module *mod, const char *name, void *base, size_t limit) {
  char path[352];
  snprintf(path, sizeof(path), "%s/%s", config.data_root, name);

  int res = so_load(mod, path, base, limit);
  if (res < 0)
    fatal_error("Could not load\n%s\nfrom %s (%d).", name, config.data_root, res);
  debugPrintf("== so_load %s ok (load_size=%u KB, load_virtbase=%p) ==\n",
              path, (unsigned)(mod->load_size >> 10), mod->load_virtbase);
}

// Mesa's GLSL-to-native disk cache: every shader compiles once EVER rather
// than once per boot, which is most of the first-encounter hitching. user://
// maps to save_root (jni_fake's getDataDir); the shader_cache directory there
// is created too, for engines that look for one.
static void make_cache_dirs(void) {
  char p[352];

  snprintf(p, sizeof(p), "%s/shader_cache", config.save_root);  mkdir(p, 0777);

  snprintf(p, sizeof(p), "%s/cache", config.save_root);         mkdir(p, 0777);
  snprintf(p, sizeof(p), "%s/cache/mesa", config.save_root);    mkdir(p, 0777);
  setenv("MESA_SHADER_CACHE_DIR", p, 1);
  setenv("MESA_GLSL_CACHE_DIR",   p, 1);   // pre-21.2 name
  setenv("MESA_SHADER_CACHE_MAX_SIZE", "128M", 1);
  unsetenv("MESA_SHADER_CACHE_DISABLE");
  unsetenv("MESA_GLSL_CACHE_DISABLE");
  // Files already in it: 0 on every boot would mean this mesa build has no
  // disk cache, and every shader is compiled from source every session.
  long long bytes = 0;
  int files = 0;
  DIR *d = opendir(p);
  if (d) {
    struct dirent *e;
    while ((e = readdir(d))) {
      if (e->d_name[0] == '.') continue;
      char sub[352];
      snprintf(sub, sizeof(sub), "%s/%s", p, e->d_name);
      struct stat st;
      if (stat(sub, &st) != 0) continue;
      if (!S_ISDIR(st.st_mode)) { files++; bytes += st.st_size; continue; }
      DIR *d2 = opendir(sub);
      if (!d2) continue;
      struct dirent *e2;
      while ((e2 = readdir(d2))) {
        char f[704];
        snprintf(f, sizeof(f), "%s/%s", sub, e2->d_name);
        if (e2->d_name[0] != '.' && stat(f, &st) == 0 && S_ISREG(st.st_mode)) {
          files++;
          bytes += st.st_size;
        }
      }
      closedir(d2);
    }
    closedir(d);
  }
  debugPrintf("[gl] mesa shader cache: %s (%s; %d file(s), %lld KB)\n", p,
              is_dir(p) ? "present" : "*** MISSING ***", files, bytes / 1024);
}

// user://sm127_nx.cfg -- settings for the in-game helper autoload
// (port/switch_port.gd), which reads them with ConfigFile in its _ready.
// Rewritten every boot from config.txt, so config.txt stays the one place to
// change them. user:// is save_root (jni_fake's getDataDir).
// Physics settings Godot reads once, in Main::setup, from res://override.cfg --
// the loose assets/override.cfg next to the pack. They have to be written there
// before GodotLib.setup, as one marked block this function adds, replaces or
// removes to match config.txt. Both are opt-in:
//
//   physics_thread 1         physics/2d/thread_model=2. SM127 runs scripts AND
//     2D physics on the frame thread (~10 + ~7-9 ms in busy stretches on
//     hardware); this moves the physics server's step to another core. Godot 3
//     calls the mode experimental. A 12 s driven run of Tutorial Hills in desktop
//     Godot 3.6 gave identical errors with and without it.
//
//   physics_interpolation 1  physics/common/physics_interpolation=true. Renders
//     bodies between physics ticks, so motion stays even when a frame is late.
//     SM127's camera.gd already calls reset_physics_interpolation() on every
//     snap, so the game is written for it, but the project leaves it off; it
//     adds up to one tick of display latency, and teleports the game does not
//     reset may smear for a frame. Desktop showed no new warnings with it on.
#define OVR_BEGIN "; sm127_nx managed settings begin\n"
#define OVR_END   "; sm127_nx managed settings end\n"
#define OVR_LEGACY_BEGIN "; sm127_nx physics_thread begin\n"
#define OVR_LEGACY_END   "; sm127_nx physics_thread end\n"

// Remove every begin..end block from buf, in place. Returns how many it removed.
static int strip_block(char *buf, const char *begin, const char *end) {
  int n = 0;
  char *b;
  while ((b = strstr(buf, begin)) != NULL) {
    char *e = strstr(b, end);
    if (!e) { *b = 0; return n + 1; }
    e += strlen(end);
    memmove(b, e, strlen(e) + 1);
    n++;
  }
  return n;
}

// The helper autoload. sm127.nro serves res://switch_port/ from its romfs
// (godot_shim.c), so the scripts always match the NRO. Installs made by an
// earlier build point it at res://sm127_nx/, their copy inside sm127.pck; that
// line is replaced, and the file is created when there is none.
#define HELPER_AUTOLOAD "SwitchPort=\"*res://" PORT_RES_DIR "/switch_port.gd\""

static void ensure_override_cfg(void) {
  char p[352];
  snprintf(p, sizeof(p), "%s/assets/override.cfg", config.data_root);
  static char buf[16384], out[16700];
  buf[0] = 0;
  FILE *f = fopen(p, "rb");
  if (f) {
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
  }
  if (strstr(buf, HELPER_AUTOLOAD)) return;

  const char *line = strstr(buf, "SwitchPort=");
  if (line) {
    const char *eol = strchr(line, '\n');
    snprintf(out, sizeof(out), "%.*s%s%s", (int)(line - buf), buf, HELPER_AUTOLOAD,
             eol ? eol : "\n");
  } else {
    snprintf(out, sizeof(out),
             "; Written by sm127.nro. Registers the port's helper autoload, which the\n"
             "; NRO serves from its own files.\n\n[autoload]\n\n" HELPER_AUTOLOAD "\n%s%s",
             buf[0] ? "\n" : "", buf);
  }
  f = fopen(p, "wb");
  if (!f) {
    debugPrintf("[port] could not write %s\n", p);
    return;
  }
  fputs(out, f);
  fclose(f);
  debugPrintf("[port] override.cfg: helper autoload %s\n",
              line ? "pointed at the NRO's scripts" : "added");
}

static void sync_override_cfg(void) {
  char p[352];
  snprintf(p, sizeof(p), "%s/assets/override.cfg", config.data_root);
  FILE *f = fopen(p, "rb");
  if (!f) return;
  static char buf[16384];
  const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = 0;

  char want[256] = "";
  if (config.physics_thread || config.physics_interpolation)
    snprintf(want, sizeof(want), OVR_BEGIN "\n[physics]\n\n%s%s" OVR_END,
             config.physics_thread ? "2d/thread_model=2\n" : "",
             config.physics_interpolation ? "common/physics_interpolation=true\n" : "");

  // Already exactly right (including "nothing to manage"): leave the file alone.
  const char *b = strstr(buf, OVR_BEGIN);
  const int legacy = strstr(buf, OVR_LEGACY_BEGIN) != NULL;
  const int matches = !legacy &&
      (want[0] ? (b && strncmp(b, want, strlen(want)) == 0 && !strstr(b + 1, OVR_BEGIN))
               : (b == NULL));
  if (matches) {
    debugPrintf("[port] physics thread %s, physics interpolation %s\n",
                config.physics_thread ? "ON" : "off", config.physics_interpolation ? "ON" : "off");
    return;
  }

  strip_block(buf, OVR_BEGIN, OVR_END);
  // The first build to write a block put its blank separator OUTSIDE the
  // markers, so take that one empty line with a legacy block as well.
  for (char *lb; (lb = strstr(buf, OVR_LEGACY_BEGIN)) != NULL; ) {
    if (lb - buf >= 2 && lb[-1] == '\n' && lb[-2] == '\n') lb[-1] = 0, memmove(lb - 1, lb, strlen(lb) + 1);
    strip_block(buf, OVR_LEGACY_BEGIN, OVR_LEGACY_END);
  }
  f = fopen(p, "wb");
  if (!f) {
    debugPrintf("[port] could not update %s\n", p);
    return;
  }
  fputs(buf, f);
  if (want[0]) {
    if (buf[0] && buf[strlen(buf) - 1] != '\n') fputc('\n', f);
    fputs(want, f);   // the blank line lives inside the markers: removal is exact
  }
  fclose(f);
  debugPrintf("[port] physics thread %s, physics interpolation %s (override.cfg updated)\n",
              config.physics_thread ? "ON" : "off", config.physics_interpolation ? "ON" : "off");
}

static void write_port_cfg(void) {
  char p[352], img[352];
  struct stat st;
  snprintf(p, sizeof(p), "%s/sm127_nx.cfg", config.save_root);
  snprintf(img, sizeof(img), "%s/cursor.png", config.data_root);
  const int have_img = stat(img, &st) == 0;
  FILE *f = fopen(p, "w");
  if (!f) {
    debugPrintf("[port] could not write %s\n", p);
    return;
  }
  fprintf(f, "; Written by sm127.nro at every boot from config.txt -- edit that instead.\n\n"
             "[port]\n\n"
             "cursor=%s\n"
             "cursor_height=%d\n"
             "cursor_image=\"%s\"\n"
             "scene_cache=%s\n"
             "prewarm=%s\n"
             "fps_limiter=%s\n"
             "http_threads=%s\n",
          config.cursor ? "true" : "false",
          config.cursor_height,
          have_img ? img : "",
          config.scene_cache ? "true" : "false",
          config.prewarm ? "true" : "false",
          config.fps_limiter ? "true" : "false",
          config.http_threads ? "true" : "false");
  fclose(f);
  debugPrintf("[port] cursor %d, cursor image %s\n", config.cursor,
              have_img ? img : "(the game's own)");
}

int main(void) {
  // Discovery first, then the trace, then config: every failure on a wrong
  // data root otherwise presents as a missing file with no clue why.
  app_paths_log_trace();
  // Any FATAL_NAME on the card is from a previous run. Clear it now so that
  // finding one after this run means this run died, not an older one.
  error_clear_fatal_file();

  debugPrintf("== Super Mario 127 Switch wrapper (Godot 3.6); build %s ==\n", build_id());
  debugPrintf("[mem] heap %u MB, so reserve %u MB (wanted %u MB)\n",
              (unsigned)(s_heap_total >> 20), (unsigned)(s_so_reserve >> 20),
              (unsigned)(SO_HEAP_RESERVE >> 20));
  if (s_heap_squeezed)
    debugPrintf("[mem] WARNING: the reservation was halved to fit the heap.\n"
                "[mem] This is applet mode -- launch the Homebrew Menu by holding\n"
                "[mem] R on a GAME, not from the album, for the full memory pool.\n");

  char cfg_path[352];
  snprintf(cfg_path, sizeof(cfg_path), "%s/%s", app_paths_data_root(), CONFIG_NAME);
  if (read_config(cfg_path) != 0)
    write_config(cfg_path);

  // CPU clock for the whole session (clocks.c). Before module loading, which
  // is CPU-bound, and never through the boost mode that starves the GPU.
  clocks_init();
  clocks_apply(1);

  check_syscalls();
  stats_open();
  const Result romfs_rc = romfsInit();
  debugPrintf("[port] helper scripts: NRO romfs %s (0x%x)\n",
              R_SUCCEEDED(romfs_rc) ? "mounted" : "NOT mounted", romfs_rc);
  // Before check_data, and before EGL takes the window: installing from an APK
  // next to the NRO draws its progress with the text console.
  if (apk_install_run()) stats_mark("game installed from the APK");
  check_data();
  mkdir(config.save_root, 0777);
  setenv("HOME", config.save_root, 1);
  make_cache_dirs();
  write_port_cfg();
  ensure_override_cfg();
  sync_override_cfg();

  set_screen_size(config.screen_width, config.screen_height);

  // Before any engine thread exists: nx_input_init() reads cursor.png and
  // pointer.cfg off the SD card.
  nx_input_init();

  // Mesa checks C11 mtx/cnd results by name; find out now, not mid-compile.
  sm127_c11_probe();

  if (egl_setup() != 0)
    fatal_error("Could not create the EGL/GLES3 context.");
  // EGL now owns the default NWindow; fatal_error must not consoleInit on it.
  error_set_display_owned(1);
  debugPrintf("== EGL/GLES3 context created (%dx%d); data_root=%s ==\n",
              screen_width, screen_height, config.data_root);
  {
    // One-time: what mesa actually gave us.
    eglMakeCurrent(s_dpy, s_surf, s_surf, s_ctx);
    debugPrintf("[gl] %s | %s | %s\n", (const char *)glGetString(GL_VENDOR),
                (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
    // What Godot 3.6's async shader compilation / shader cache need
    // (override.cfg asks for them): a parallel-compile extension, and
    // program binaries. Absent, Godot says "not supported" and compiles
    // synchronously as before.
    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    GLint nbin = 0;
    glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &nbin);
    debugPrintf("[gl] parallel_shader_compile: %s; program binary formats: %d\n",
                ext && (strstr(ext, "GL_KHR_parallel_shader_compile") ||
                        strstr(ext, "GL_ARB_parallel_shader_compile")) ? "yes" : "no",
                (int)nbin);
    eglMakeCurrent(s_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  }

  // libc++ first so libgodot's C++ ABI imports resolve against it
  load_module(&cxx_mod, CXX_SO_NAME, heap_so_base, CXX_SO_SLICE);
  const size_t game_slice = heap_so_limit - CXX_SO_SLICE - NATIVE_SO_SLICE;
  load_module(&game_mod, SO_NAME, (char *)heap_so_base + CXX_SO_SLICE, game_slice);
  // GDNative modules, if a modded APK has any, are loaded later when the
  // engine dlopen()s them; they get the top of the reserve.
  native_modules_set_region((char *)heap_so_base + CXX_SO_SLICE + game_slice,
                            NATIVE_SO_SLICE);

  sm127_resolve_imports(&cxx_mod);
  sm127_resolve_imports(&game_mod);
  debugPrintf("== imports resolved ==\n");
  so_patch(&game_mod);

  // resolve exports before so_finalize maps the code and locks load_base out
  resolve_entry_points();
  // same window, same reason: the callbacks jni_helpers.c / nx_input.c need
  sm127_cache_engine_symbols();

  so_finalize(&cxx_mod);
  so_flush_caches(&cxx_mod);
  so_finalize(&game_mod);
  so_flush_caches(&game_mod);
  debugPrintf("== so_finalize ok; running init arrays ==\n");

  jni_init();
  tls_setup_guard();
  so_execute_init_array(&cxx_mod);
  so_execute_init_array(&game_mod);
  so_free_temp(&cxx_mod);
  so_free_temp(&game_mod);
  debugPrintf("== init arrays done ==\n");
  stats_mark("modules loaded + init arrays");

  // the game sees cwd="/" (getcwd_fake) and stray absolute writes are rebased
  // into save_root (sandbox_path); move the REAL cwd there too.
  if (chdir(config.save_root) != 0)
    debugPrintf("!! chdir(%s) failed\n", config.save_root);

  padConfigureInput(8, HidNpadStyleSet_NpadStandard);
  padInitializeAny(&pad);
  hidInitializeTouchScreen();

  // Core 1 belongs to the frame loop. Core 0 is the process default, where
  // every thread created without an explicit core lands -- this main thread
  // (mostly asleep), Mesa's own threads, and glthread's worker. Engine
  // workers go to cores 0/2 (imports.c thread_trampoline), Godot's audio
  // mixer to core 2 (audio.c) and the audio-out feeder to core 0.
  if (R_FAILED(threadCreate(&s_game_thread, game_thread_fn, NULL, NULL, 8 * 1024 * 1024, 0x2C, 1)) &&
      R_FAILED(threadCreate(&s_game_thread, game_thread_fn, NULL, NULL, 8 * 1024 * 1024, 0x2C, -2)))
    fatal_error("Could not create the game thread.");
  threadStart(&s_game_thread);

  int last_frames = -1;
  int stall_ms = 0;
  int warned = 0;
  while (appletMainLoop() && s_game_running) {
    // A fatal_error() on the game thread parks it and leaves the message for
    // us: the error applet has to be launched from this thread.
    if (fatal_error_pending())
      fatal_error_show_pending();

    AppletFocusState fs = appletGetFocusState();
    const int was_focused = s_focused;
    s_focused = (fs == AppletFocusState_InFocus);

    // On a focus transition, silence the mixer and stop the motors, and keep
    // the pointer's clamp in step with a dock/undock.
    // Clocks: stock while away, targets back on return, and re-checked about
    // once a second because dock/undock and sleep reset them.
    static int s_clock_tick;
    if (++s_clock_tick >= 60) {
      s_clock_tick = 0;
      clocks_tick(s_focused);
      // Once a second: the log and boot_stats.txt onto the card, so a console
      // powered off mid-session loses at most a second. Every card write
      // happens on this thread; the frame thread only queues lines.
      debugPump(1);
      stats_pump();
    } else {
      debugPump(0);   // queued lines into the FILE buffer; no card write
    }

    if (s_focused != was_focused) {
      clocks_apply(s_focused);
      audio_sink_set_paused(!s_focused);
      if (!s_focused) jni_rumble_stop();
      nx_input_set_screen(screen_width, screen_height);
    }

    // Software keyboard: runs HERE, because swkbdShow blocks and the OS
    // suspends the process while it is up.
    if (s_focused && swkbd_is_pending()) {
      s_applet_gen++;   // odd: the keyboard has the controls (poll_input)
      swkbd_pump();
      s_applet_gen++;
      stall_ms = 0;
      last_frames = s_frames_done;
      warned = 0;
    }

    // OS.shell_open (LSS's sign-up / password-reset links): the browser applet
    // blocks the same way, for as long as the page is open.
    if (s_focused && web_is_pending()) {
      s_applet_gen++;
      web_pump();
      s_applet_gen++;
      stall_ms = 0;
      last_frames = s_frames_done;
      warned = 0;
    }

    // Hang watchdog: a flushed warning at 5 s always; a single pause-and-dump
    // only when watchdog_stall_s is set (pausing threads is not risk-free).
    if (s_frames_done != last_frames) {
      last_frames = s_frames_done;
      stall_ms = 0;
      warned = 0;
      clocks_set_stalled(0);
    } else {
      stall_ms += 16;
      // Well under the 5 s warning: a level load is seconds long and the point
      // is to be at the loading clock FOR it, not to notice afterwards. Three
      // missed frames is already far past anything normal play produces.
      if (stall_ms >= 500) clocks_set_stalled(1);
      if (!warned && stall_ms >= 5000) {
        warned = 1;
        debugPrintf("[watchdog] game thread has not completed a step in 5 s "
                    "(steps done: %d, focused: %d)\n", s_frames_done, s_focused);
        debugFlush();
      }
      const int arm = config.watchdog_arm_steps > 0 ? config.watchdog_arm_steps
                                                    : WATCHDOG_ARM_STEPS;
      static int s_dumped;
      if (config.watchdog_stall_s > 0 && !s_dumped && s_frames_done >= arm &&
          stall_ms >= config.watchdog_stall_s * 1000) {
        debugPrintf("[watchdog] STALLED %d s (steps done: %d)\n",
                    stall_ms / 1000, s_frames_done);
        s_dumped = 1;
        sm127_dump_all_threads();
        debugFlush();
      }
    }
    svcSleepThread(16 * 1000 * 1000);
  }

  // Why the loop ended. A clean exit produces no Atmosphere crash report, so
  // without this line a player can only report "it closed itself": engine quit
  // (the game called forceQuit) and system quit (HOME/close) look identical
  // from the outside, and they have completely different causes.
  debugPrintf("== exit: %s (running=%d quit=%d, steps done=%d) ==\n",
              jni_quit_requested  ? "the engine asked to quit"
              : !s_game_running   ? "the game thread ended"
                                  : "the system asked us to close",
              s_game_running, jni_quit_requested, s_frames_done);
  debugFlush();

  s_game_running = 0;
  threadWaitForExit(&s_game_thread);
  threadClose(&s_game_thread);
  stats_pump();

  nx_input_shutdown();
  clocks_restore();

  if (s_ctx != EGL_NO_CONTEXT) {
    eglMakeCurrent(s_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(s_dpy, s_ctx);
    eglDestroySurface(s_dpy, s_surf);
    eglTerminate(s_dpy);
  }

  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
  return 0;
}
