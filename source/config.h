/* config.h -- Super Mario 127 Switch wrapper configuration.
 *
 * Forked from sts2_nx (MIT). Everything that existed there for the NativeAOT
 * payload, FMOD/Spine, Vulkan/NVK and the ASTC bake is gone; what is left is
 * the GLES3 path that lineage has run on hardware.
 *
 * MIT license; see LICENSE. */

#ifndef __CONFIG_H__
#define __CONFIG_H__

// The engine pair from the game's Android APK (Godot 3.6): lib/arm64-v8a/.
#define SO_NAME "libgodot_android.so"
#define CXX_SO_NAME "libc++_shared.so"

// The game data is the APK's assets/ tree, packed into
// <data_root>/assets/sm127.pck on the first launch (apk_install.c) and mounted
// as --main-pack res://sm127.pck (one file instead of ~8500 SD-card path
// walks). A loose assets/ tree with project.binary also works. godot_shim.c
// serves AAssetManager (res://) from <data_root>/assets/.
#define PCK_NAME "sm127.pck"

// res://switch_port/ is the port's helper (port/ in the source), served from
// the NRO's romfs rather than the SD card.
#define PORT_RES_DIR "switch_port"

// The NRO's own basename, used to read its mtime as the build id (main.c).
#define APP_NAME "sm127_nx"

#define CONFIG_NAME "config.txt"
#define LOG_NAME "sm127_debug.log"

// Written ONLY when the port stops with a fatal error, and deleted at every
// boot -- so its presence after a run is by itself the answer to "did it die
// or did it quit?". A fatal exit is a clean exit(1), which produces no
// Atmosphere crash report, so without this there is nothing to point at.
#define FATAL_NAME "sm127_fatal.txt"

// LAST-RESORT FALLBACK ONLY. The app tree is DISCOVERED at runtime from
// argv[0], then the cwd, then a scan of /switch/ for a directory containing
// libgodot_android.so (app_paths.c). Use config.data_root / config.save_root.
#define DEFAULT_DATA_ROOT "/switch/sm127"

// The save directory is always <data_root>/save (user:// maps there).
#define SAVE_SUBDIR "save"

// Master debug switch. ON for bring-up; flip to 0 once the game is confirmed
// playable on hardware.
#define DEBUG_LOG 1
// Per-file-operation logging (open/stat/access/fopen). Very noisy and slow;
// requires DEBUG_LOG too.
#define VERBOSE_IO 0
// Audio sink tracing.
#define VERBOSE_AUDIO 0

extern int screen_width;
extern int screen_height;

// locale reported to the engine via GodotIO.getLocale
#define DEVICE_LOCALE "en_US"

typedef struct {
  int screen_width;   // -1 = auto (1080p docked / 720p handheld)
  int screen_height;

  // CPU clock, MHz (official rates up to 1785). boot_cpu_mhz while loading
  // (until the title screen is up), cpu_mhz while playing; 0 = the system's
  // own (1020). See clocks.c for why this replaced appletSetCpuBoostMode,
  // whose loading profile drops the GPU to 76.8 MHz.
  int boot_cpu_mhz;
  int cpu_mhz;          // -1 = automatic (default; clocks.c governor)

  // In-game helper (port/switch_port.gd), via user://sm127_nx.cfg:
  //   cursor          1 = controller cursor (auto while paused, R toggles)
  //   cursor_height   0 = auto; else px at 720p
  int cursor;
  int cursor_height;
  //   scene_cache     1 = keep every scene the game instances loaded, so a
  //                   second visit (pause menu, rooms) skips the .tscn parse
  int scene_cache;
  //   pack_ram        -1 = hold the .pck in RAM only when there is room for it,
  //                    0 = never (stream from the card), 1 = always
  int pack_ram;
  //   keep_apk        1 = keep the APK after installing the game from it
  int keep_apk;
  //   prewarm         1 = pre-load the player and object scenes while in the menus,
  //                    so starting a level does not stall on a white screen
  int prewarm;
  //   mipmaps         0 = runtime textures keep level 0 only (black otherwise on this
  //                    Mesa); 1 = call glGenerateMipmap for real and log GL errors
  int mipmaps;
  //   physics_thread  1 = run SM127's 2D physics on its own thread (experimental)
  int physics_thread;
  //   physics_interpolation 1 = physics/common/physics_interpolation (opt-in)
  int physics_interpolation;
  //   fps_limiter     1 = keep SM127's force_fps 60 limiter on top of vsync
  int fps_limiter;
  //   http_threads    1 = the helper runs each HTTPRequest on a worker thread, so
  //                    TLS handshakes stay off the frame thread (switch_port.gd)
  int http_threads;

  // EXPERIMENTAL. 1 = Mesa glthread: GL calls are queued and run by the
  // driver on another core. Off by default; see main.c.
  int glthread;
  // GPU clock, MHz. 0 (default) = the system's own (384 handheld / 768
  // docked). Capped at 460.8 handheld / 768 docked.
  int gpu_mhz;

  // 1 = keep per-line log flushing on forever, not just through boot.
  int eager_log;

  // Minimum stack for engine threads, in KB. 1024 matches bionic's default,
  // which is what the engine was compiled expecting; newlib's is far smaller
  // and sts2_nx overflowed it during scene loading. 0 = newlib's default.
  int thread_stack_kb;

  // Record pthread lazy-slot transitions into an in-memory ring (printed only
  // on a rejected slot or a crash), and how many events the crash handler
  // prints. See lock_trace.c.
  int lock_trace;
  int lock_trace_dump;

  // 1 = the periodic [mem] line. It calls mallinfo(), which walks the heap
  // under the global malloc lock -- a stall every 300 frames. Diagnostic only.
  int mem_trend;

  // Seconds of no completed step before the watchdog PAUSES the game thread
  // and dumps its stack. 0 (default) = warn only, never pause.
  int watchdog_stall_s;
  // Steps that must complete before that dump can fire. 0 = 120.
  int watchdog_arm_steps;

  // 1 (default) = the handheld touchscreen drives Godot touch events: SM127's
  // on-screen buttons, "Tap the screen to start!", and the menus. 0 = off.
  // The on-screen buttons themselves are the game's; turn them off in its
  // Options menu ("Enable touch controls") for controller-only play.
  int touch;

  // Vertical render resolution, 360..1080. 0 = follow the display (720
  // handheld, 1080 docked). The width is computed as height * 16 / 9.
  // SM127 renders a 640x360 canvas and scales it, so 720 is plenty.
  int resolution_height;

  // Godot 3.x only: 1 = report OpenGL ES 2.0 to the engine, so it uses its
  // GLES2 renderer instead of GLES3. A fallback if GLES3 misrenders or is slow
  // on nouveau. Godot 4 ignores it.
  int gles2;

  // 1 = pass --verbose to the engine: every resource load is logged, and on
  // Godot 4 GL debug output is enabled too (slow on mesa). Diagnostic only.
  int verbose;

  char data_root[256];
  char save_root[256];
} Config;

extern Config config;

int read_config(const char *file);
int write_config(const char *file);

#endif
