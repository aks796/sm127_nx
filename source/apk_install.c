/* apk_install.c -- installs the game from its Android APK on the console.
 *
 * Players put the Super Mario 127 APK next to sm127.nro. When one is there and
 * it is not what is already installed, this runs before anything else starts:
 * a text screen shows progress while apk_unpack.c writes the engine libraries
 * and sm127.pck, the APK's name, size and date are recorded, and the APK is
 * deleted unless config.txt has keep_apk 1.
 *
 * The NRO carries the port's own scripts (port/, in its romfs), so what is
 * installed here is only the game, and a new NRO never needs the APK again.
 *
 * MIT license; see LICENSE. */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "error.h"
#include "godot_shim.h"   // sm127_rename_replacing
#include "apk_unpack.h"
#include "apk_install.h"

#define MARKER "assets/installed_from.txt"

static int has_file(const char *rel) {
  char p[512];
  struct stat st;
  snprintf(p, sizeof(p), "%s/%s", config.data_root, rel);
  return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

// The newest *.apk in the app folder.
static int find_apk(char *path, size_t sz, struct stat *out) {
  DIR *d = opendir(config.data_root);
  if (!d) return 0;
  int found = 0;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    const size_t l = strlen(e->d_name);
    if (l <= 4 || strcasecmp(e->d_name + l - 4, ".apk") != 0) continue;
    char p[512];
    struct stat st;
    snprintf(p, sizeof(p), "%s/%s", config.data_root, e->d_name);
    if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) continue;
    found++;
    if (found > 1 && st.st_mtime <= out->st_mtime) continue;
    snprintf(path, sz, "%s", p);
    *out = st;
  }
  closedir(d);
  if (found > 1) debugPrintf("[install] %d APKs in the folder; using the newest\n", found);
  return found > 0;
}

static int marker_matches(const char *want) {
  char p[512], got[768];
  snprintf(p, sizeof(p), "%s/" MARKER, config.data_root);
  FILE *f = fopen(p, "rb");
  if (!f) return 0;
  const size_t n = fread(got, 1, sizeof(got) - 1, f);
  fclose(f);
  got[n] = 0;
  return strcmp(got, want) == 0;
}

static void remove_apk(const char *apk, const char *name) {
  if (config.keep_apk) {
    debugPrintf("[install] keeping %s (keep_apk 1)\n", name);
    return;
  }
  if (remove(apk) == 0) debugPrintf("[install] deleted %s\n", name);
  else                  debugPrintf("[install] could not delete %s\n", name);
}

typedef struct {
  u64 last;
} Ui;

static void ui_progress(void *user, uint64_t done, uint64_t total) {
  Ui *ui = user;
  const u64 now = armGetSystemTick();
  if (done < total && ui->last && armTicksToNs(now - ui->last) < 100000000ull) return;
  ui->last = now;
  const unsigned pct = total ? (unsigned)(done * 100 / total) : 100;
  char bar[41];
  for (unsigned i = 0; i < 40; i++) bar[i] = i < pct * 40 / 100 ? '#' : '-';
  bar[40] = 0;
  printf("\x1b[9;3H[%s] %3u%%\x1b[10;3H%llu of %llu MB    ", bar, pct,
         (unsigned long long)(done >> 20), (unsigned long long)(total >> 20));
  consoleUpdate(NULL);
}

// Between files, so HOME and the system's request to close keep working.
static int ui_keep_going(void *user) {
  (void)user;
  return appletMainLoop();
}

int apk_install_run(void) {
  const int installed = has_file(SO_NAME) && has_file(CXX_SO_NAME) &&
                        (has_file("assets/" PCK_NAME) || has_file("assets/project.binary"));
  char apk[512];
  struct stat st;
  memset(&st, 0, sizeof(st));
  if (!find_apk(apk, sizeof(apk), &st)) {
    if (installed) return 0;
    fatal_error("Super Mario 127 is not installed yet.\n\n"
                "Put the game's Android APK in\n%s\nnext to sm127.nro, then start it again.",
                config.data_root);
  }

  const char *name = strrchr(apk, '/') + 1;
  char want[640];
  snprintf(want, sizeof(want), "%s\n%lld\n%lld\n", name, (long long)st.st_size,
           (long long)st.st_mtime);
  if (installed && marker_matches(want)) {
    debugPrintf("[install] %s is already installed\n", name);
    remove_apk(apk, name);
    return 0;
  }

  debugPrintf("[install] installing from %s (%lld MB)\n", name, (long long)(st.st_size >> 20));
  debugFlush();
  appletSetAutoSleepDisabled(true);
  consoleInit(NULL);
  printf("\x1b[3;3HSuper Mario 127"
         "\x1b[5;3HInstalling the game from"
         "\x1b[6;3H%.72s"
         "\x1b[13;3HThis only happens once. Keep the console on until the game starts.",
         name);
  consoleUpdate(NULL);

  Ui ui = { 0 };
  const ApkUnpackIo io = { &ui, ui_progress, ui_keep_going, sm127_rename_replacing };
  ApkUnpackResult res;
  const u64 t0 = armGetSystemTick();
  const int rc = apk_unpack(apk, config.data_root, PCK_NAME, &io, &res);
  const u64 ms = armTicksToNs(armGetSystemTick() - t0) / 1000000ull;
  consoleExit(NULL);
  appletSetAutoSleepDisabled(false);

  if (rc == APK_UNPACK_STOPPED) {
    debugPrintf("[install] stopped after %llu ms: the system asked the app to close\n", ms);
    debugFlush();
    exit(0);
  }
  if (rc != APK_UNPACK_OK)
    fatal_error("Could not install Super Mario 127 from\n%s\n\n%s", name, res.error);

  debugPrintf("[install] %u files packed, %llu MB written in %llu.%llu s\n", res.files,
              (unsigned long long)(res.bytes >> 20), ms / 1000, ms % 1000 / 100);
  char p[512];
  snprintf(p, sizeof(p), "%s/" MARKER, config.data_root);
  FILE *f = fopen(p, "wb");
  if (f) {
    fputs(want, f);
    fclose(f);
  }
  remove_apk(apk, name);
  return 1;
}
