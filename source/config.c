/* config.c -- simple configuration parser
 *
 * Copyright (C) 2021 Andy Nguyen, fgsfds
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

#include "config.h"
#include "util.h"
#include "app_paths.h"

// data_root is deliberately NOT persistable: it is discovered at runtime
// (app_paths.c) and a stale value in config.txt would survive a rename of the
// install directory and then point at nothing.
//
// save_root IS persistable, for the one case discovery cannot cover: keeping
// saves outside the app tree so they survive deleting and recopying it. Left
// unset it derives as <data_root>/save and follows a rename automatically.

/* Readable, but not written into a fresh config.txt: settled or diagnostic. */
#define CONFIG_VARS_FIXED \
  CONFIG_VAR_INT(screen_width); \
  CONFIG_VAR_INT(screen_height); \
  CONFIG_VAR_INT(eager_log); \
  CONFIG_VAR_INT(thread_stack_kb); \
  CONFIG_VAR_INT(lock_trace); \
  CONFIG_VAR_INT(lock_trace_dump); \
  CONFIG_VAR_INT(mem_trend); \
  CONFIG_VAR_INT(watchdog_stall_s); \
  CONFIG_VAR_INT(watchdog_arm_steps); \
  CONFIG_VAR_INT(verbose); \
  CONFIG_VAR_INT(cursor_height); \
  CONFIG_VAR_INT(scene_cache); \
  CONFIG_VAR_INT(pack_ram); \
  CONFIG_VAR_INT(prewarm); \
  CONFIG_VAR_INT(mipmaps); \
  CONFIG_VAR_INT(physics_thread); \
  CONFIG_VAR_INT(physics_interpolation); \
  CONFIG_VAR_INT(fps_limiter); \
  CONFIG_VAR_INT(http_threads); \
  CONFIG_VAR_INT(glthread); \
  CONFIG_VAR_STR(save_root);

/* Worth changing per install, so written out. */
#define CONFIG_VARS_WRITTEN \
  CONFIG_VAR_INT(resolution_height); \
  CONFIG_VAR_INT(touch); \
  CONFIG_VAR_INT(gles2); \
  CONFIG_VAR_INT(boot_cpu_mhz); \
  CONFIG_VAR_INT(cpu_mhz); \
  CONFIG_VAR_INT(gpu_mhz); \
  CONFIG_VAR_INT(cursor); \
  CONFIG_VAR_INT(keep_apk);

#define CONFIG_VARS CONFIG_VARS_WRITTEN CONFIG_VARS_FIXED

Config config;

// actual screen size that is in use right now
int screen_width = 1280;
int screen_height = 720;

static inline void parse_var(const char *name, const char *value) {
  #define CONFIG_VAR_INT(var) if (!strcmp(name, #var)) { config.var = atoi(value); return; }
  #define CONFIG_VAR_STR(var) if (!strcmp(name, #var)) { strlcpy(config.var, value, sizeof(config.var)); return; }
  CONFIG_VARS
  #undef CONFIG_VAR_INT
  #undef CONFIG_VAR_STR

  // Reaching here means the key is not one we have. Say so: a silently
  // ignored line looks exactly like a setting that is present and working.
  debugPrintf("[config] UNKNOWN key '%s' (value '%s') -- ignored.\n", name, value);
}

int read_config(const char *file) {
  char line[1024] = { 0 };

  memset(&config, 0, sizeof(Config));
  // Defaults MUST come after the memset.
  config.screen_width = -1; // auto
  config.screen_height = -1;
  config.eager_log = 0;
  config.thread_stack_kb = 1024;
  config.lock_trace = 0;        // sts2_nx race-hunt diagnostic: off, it runs on every lock
  config.boot_cpu_mhz = 1785;   // loading only; see clocks.c
  config.cpu_mhz = -1;          // auto: stock unless frames are missed (clocks.c)
  config.scene_cache = 1;   // repeat level loads (Retry) reparse ~300 text scenes otherwise
  config.prewarm = 1;        // pre-load player + object scenes in the menus (switch_port.gd)
  config.mipmaps = 0;
  config.physics_thread = 0; // experimental in Godot 3 (main.c sync_override_cfg)
  config.physics_interpolation = 0; // opt-in (main.c sync_override_cfg)
  config.fps_limiter = 0;    // 0 = clear SM127's redundant 60 fps cap (switch_port.gd)        // runtime mip chains come out black on this Mesa (imports.c)
  config.http_threads = 1;   // TLS handshakes off the frame thread (switch_port.gd)
  config.pack_ram = -1;      // auto: resident only when memory allows (godot_shim.c)
  config.glthread = 0;
  config.cursor = 0;        // the game's menus are controller-driven; the editor can opt in
  config.keep_apk = 0;      // the APK is deleted once the game is installed from it (apk_install.c)
  config.cursor_height = 0;
  config.gpu_mhz = 0;
  config.lock_trace_dump = 64;  // events printed from the crash handler
  config.mem_trend = 0;
  config.watchdog_stall_s = 0;
  config.watchdog_arm_steps = 0;
  config.touch = 1;
  config.resolution_height = 0; // follow the display

  Config defaults = config;

  // Discovered, not hardcoded: the app tree can live under any directory in
  // /switch/. See app_paths.c.
  app_paths_resolve(config.data_root, sizeof(config.data_root),
                    config.save_root, sizeof(config.save_root));

  FILE *f = fopen(file, "r");
  if (f == NULL)
    return -1;

  do {
    char *name = NULL, *value = NULL, *tmp = NULL;
    if (fgets(line, sizeof(line), f) != NULL) {
      name = line;
      while (*name && isspace((int)*name)) ++name;
      if (name[0] == '#') continue; // skip comments
      for (tmp = name; *tmp && !isspace((int)*tmp); ++tmp);
      if (*tmp != 0) {
        *tmp = 0;
        for (value = tmp + 1; *value && isspace((int)*value); ++value);
        for (tmp = value + strlen(value) - 1; isspace((int)*tmp); --tmp) *tmp = 0;
        parse_var(name, value);
      }
    }
  } while (!feof(f));

  fclose(f);

  // Echo everything that is NOT at its default, so a setting that took effect
  // is visible in the log.
  {
    const Config *dp = &defaults;
    char out[512];
    int off = 0;
    #define CONFIG_VAR_INT(var) \
      if (config.var != dp->var && off < (int)sizeof(out) - 40) \
        off += snprintf(out + off, sizeof(out) - (size_t)off, "%s=%d ", #var, config.var)
    #define CONFIG_VAR_STR(var) /* paths are logged separately */
    CONFIG_VARS;
    #undef CONFIG_VAR_INT
    #undef CONFIG_VAR_STR
    debugPrintf("[config] non-default: %s\n", off ? out : "(none -- all defaults)");
  }

  // An overridden save_root is only useful if it exists.
  char derived[288];
  snprintf(derived, sizeof(derived), "%s/%s", config.data_root, SAVE_SUBDIR);
  if (strcmp(config.save_root, derived) != 0) {
    debugPrintf("[config] save_root overridden: %s\n", config.save_root);
    mkdir(config.save_root, 0777);
  }

  return 0;
}

int write_config(const char *file) {
  FILE *f = fopen(file, "w");
  if (f == NULL)
    return -1;

  fprintf(f, "# sm127_nx settings. Delete this file to restore the defaults.\n"
             "# resolution_height: 0 = follow the display (720 handheld, 1080 docked), else 360..1080\n"
             "# touch:             1 = handheld touchscreen input, 0 = off\n"
             "# gles2:             1 = Godot 3 games use the GLES2 renderer instead of GLES3\n"
             "# boot_cpu_mhz:      CPU clock while loading (official rates up to 1785); 0 = system default\n"
             "# cpu_mhz:           CPU clock while playing: -1 = automatic (stock 1020, raised only while\n"
             "#                    frames are being missed), 0 = always stock, or a fixed MHz up to 1785\n"
             "# gpu_mhz:           GPU clock; 0 = system default (384 handheld / 768 docked), max 460 / 768\n"
             "# cursor:            1 = controller cursor for the level editor (R toggles it; stick\n"
             "#                    moves, A/ZL click, Y/ZR right-click, L recentres). Off by default.\n"
             "# keep_apk:          1 = keep the APK next to the NRO after installing the game from it\n"
             );

  // A DERIVED save_root must not be persisted: it would pin the current
  // install directory into config.txt.
  char derived[288];
  snprintf(derived, sizeof(derived), "%s/%s", config.data_root, SAVE_SUBDIR);
  const int save_root_is_derived = (strcmp(config.save_root, derived) == 0);
  (void)save_root_is_derived;   // used only if save_root joins CONFIG_VARS_WRITTEN

  #define CONFIG_VAR_INT(var) fprintf(f, "%s %d\n", #var, config.var)
  #define CONFIG_VAR_STR(var) \
    if (config.var[0] && !(save_root_is_derived && !strcmp(#var, "save_root"))) \
      fprintf(f, "%s %s\n", #var, config.var)
  CONFIG_VARS_WRITTEN
  #undef CONFIG_VAR_INT
  #undef CONFIG_VAR_STR

  fclose(f);
  return 0;
}
