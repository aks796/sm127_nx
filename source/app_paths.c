/* app_paths.c -- work out where the app tree actually is.
 *
 * The port must run from ANY directory under /switch/ -- /switch/sts2_nx,
 * /switch/slaythespire2, /switch/sls2nx, whatever the user made. So the data
 * root is discovered rather than compiled in.
 *
 * Four strategies, in order of how much they can be trusted:
 *
 *   1. argv[0]. The Homebrew Menu passes the NRO's full path, so its directory
 *      is the answer with no searching and no ambiguity. This is the normal
 *      case and the only one that is exact.
 *
 *   2. getcwd(). hbmenu also sets the working directory to the NRO's folder,
 *      which is what config.h's existing "fopen'd relative to the NRO's
 *      directory" note relies on. Covers a launcher that sets CWD but not
 *      argv.
 *
 *   3. Scan /switch/ for a directory containing libgodot_android.so. Covers
 *      being launched by something that sets neither -- a forwarder, title
 *      takeover, or nxlink. Slower and it can in principle pick the wrong
 *      directory if two installs exist, so it logs loudly and prefers the
 *      first match in readdir order.
 *
 *   4. The compile-time default, so there is always an answer to print in the
 *      error message.
 *
 * TIMING. This runs from userAppInit, BEFORE the log file is open -- the log
 * path itself depends on the answer. So the trace is buffered and flushed by
 * app_paths_log_trace() once logging is up. Diagnosing a wrong data root is
 * otherwise very hard: every subsequent failure is a missing file.
 *
 * MIT license; see LICENSE. */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "app_paths.h"

// libnx fills these from the homebrew ABI before __appInit runs, so they are
// readable from userAppInit.
extern int    __system_argc;
extern char **__system_argv;

// A directory is the app tree if it holds this. The engine binary is the right
// marker: it is mandatory, it is not something a user would put anywhere else,
// and unlike the .pck its name does not vary.
#define APP_MARKER "libgodot_android.so"

#define TRACE_MAX 1024

static char s_data_root[288];
static char s_save_root[288];
static char s_log_path[352];
static char s_trace[TRACE_MAX];
static int  s_trace_len = 0;
static int  s_resolved = 0;

static void trace(const char *fmt, ...) {
  if (s_trace_len >= TRACE_MAX - 1) return;
  va_list va;
  va_start(va, fmt);
  int n = vsnprintf(s_trace + s_trace_len, (size_t)(TRACE_MAX - s_trace_len), fmt, va);
  va_end(va);
  if (n > 0) s_trace_len += n;
  if (s_trace_len > TRACE_MAX - 1) s_trace_len = TRACE_MAX - 1;
}

// "sdmc:/switch/foo/bar.nro" -> "/switch/foo". Everything downstream uses
// '/'-absolute paths against the default sdmc device, so the device prefix
// has to come off or strncmp(path, "/switch", 7) in sandbox_path never matches.
static int dir_of(const char *path, char *out, size_t sz) {
  if (!path || !*path) return 0;

  const char *p = strchr(path, ':');
  p = p ? p + 1 : path;            // drop "sdmc:" / "romfs:" etc
  if (*p != '/') return 0;

  const char *slash = strrchr(p, '/');
  if (!slash || slash == p) return 0;

  size_t len = (size_t)(slash - p);
  if (len >= sz) return 0;

  memcpy(out, p, len);
  out[len] = '\0';
  return 1;
}

static int has_marker(const char *dir) {
  char probe[512];
  snprintf(probe, sizeof(probe), "%s/%s", dir, APP_MARKER);
  struct stat st;
  return stat(probe, &st) == 0 && S_ISREG(st.st_mode);
}

// Strategy 3. Only reached when neither argv nor cwd worked.
static int scan_switch_dir(char *out, size_t sz) {
  DIR *d = opendir("/switch");
  if (!d) {
    trace("[paths] /switch not readable\n");
    return 0;
  }

  int found = 0;
  int candidates = 0;
  struct dirent *e;

  while ((e = readdir(d)) != NULL) {
    if (e->d_name[0] == '.') continue;

    char cand[512];
    snprintf(cand, sizeof(cand), "/switch/%s", e->d_name);

    struct stat st;
    if (stat(cand, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
    if (!has_marker(cand)) continue;

    candidates++;
    if (!found) {
      strlcpy(out, cand, sz);
      found = 1;
    } else {
      // Two installs. Keep the first but say so -- silently picking one and
      // then failing to find a save is a miserable thing to debug.
      trace("[paths] WARNING: also found %s; using the first match\n", cand);
    }
  }
  closedir(d);

  if (found)
    trace("[paths] scan found %s (%d candidate%s)\n",
          out, candidates, candidates == 1 ? "" : "s");
  else
    trace("[paths] scan of /switch found no directory containing %s\n", APP_MARKER);

  return found;
}

static void resolve_once(void) {
  if (s_resolved) return;
  s_resolved = 1;

  char cand[288];

  // -- 1. argv[0] ---------------------------------------------------------
  if (__system_argc > 0 && __system_argv && __system_argv[0]) {
    trace("[paths] argv[0] = %s\n", __system_argv[0]);
    if (dir_of(__system_argv[0], cand, sizeof(cand))) {
      if (has_marker(cand)) {
        strlcpy(s_data_root, cand, sizeof(s_data_root));
        trace("[paths] using argv[0] directory: %s\n", s_data_root);
        goto done;
      }
      // argv[0] is authoritative about WHERE the NRO is, so a missing marker
      // here means the install is incomplete, not that the path is wrong.
      // Say so rather than silently scanning and landing somewhere else.
      trace("[paths] %s has no %s -- incomplete install?\n", cand, APP_MARKER);
      strlcpy(s_data_root, cand, sizeof(s_data_root));
      goto done;
    }
  } else {
    trace("[paths] no argv (launched without one?)\n");
  }

  // -- 2. cwd -------------------------------------------------------------
  if (getcwd(cand, sizeof(cand))) {
    char norm[288];
    const char *p = strchr(cand, ':');
    strlcpy(norm, p ? p + 1 : cand, sizeof(norm));
    // strip a trailing slash so the marker probe does not double it
    size_t l = strlen(norm);
    while (l > 1 && norm[l - 1] == '/') norm[--l] = '\0';

    trace("[paths] cwd = %s\n", norm);
    if (norm[0] == '/' && has_marker(norm)) {
      strlcpy(s_data_root, norm, sizeof(s_data_root));
      trace("[paths] using cwd: %s\n", s_data_root);
      goto done;
    }
  }

  // -- 3. scan ------------------------------------------------------------
  if (scan_switch_dir(cand, sizeof(cand))) {
    strlcpy(s_data_root, cand, sizeof(s_data_root));
    goto done;
  }

  // -- 4. give up ---------------------------------------------------------
  strlcpy(s_data_root, DEFAULT_DATA_ROOT, sizeof(s_data_root));
  trace("[paths] falling back to the compile-time default: %s\n", s_data_root);

done:
  snprintf(s_save_root, sizeof(s_save_root), "%s/save", s_data_root);
  snprintf(s_log_path,  sizeof(s_log_path),  "%s/%s", s_data_root, LOG_NAME);

  // The save directory must exist before the engine tries to write into it;
  // Godot's DirAccess will not create intermediate directories for a path it
  // was handed. mkdir on an existing directory is a harmless EEXIST.
  mkdir(s_save_root, 0777);
}

const char *app_paths_data_root(void) { resolve_once(); return s_data_root; }
const char *app_paths_save_root(void) { resolve_once(); return s_save_root; }
const char *app_paths_log_file(void)  { resolve_once(); return s_log_path;  }

void app_paths_resolve(char *data_root, size_t data_sz,
                       char *save_root, size_t save_sz) {
  resolve_once();
  if (data_root) strlcpy(data_root, s_data_root, data_sz);
  if (save_root) strlcpy(save_root, s_save_root, save_sz);
}

void app_paths_log_trace(void) {
  resolve_once();
  if (s_trace_len > 0) {
    debugPrintf("%s", s_trace);
    s_trace_len = 0;
  }
  debugPrintf("[paths] data_root = %s\n", s_data_root);
  debugPrintf("[paths] save_root = %s\n", s_save_root);
}
