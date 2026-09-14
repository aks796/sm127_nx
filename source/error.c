/* error.c -- error handler
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * sm127_nx: the upstream version drew the message with consoleInit(NULL).
 * That claims the default NWindow -- which EGL already owns once the renderer
 * is up -- and on the first hardware run it faulted inside the crash handler
 * instead, leaving a black screen and no message. Now:
 *
 *   1. the message always goes to the log first, flushed;
 *   2. it is shown with the system error applet, which needs no window;
 *   3. applets must be launched from the main thread (swkbd follows the same
 *      rule), so a fatal on any other thread is handed to main()'s loop and
 *      the failing thread parks;
 *   4. the console is only a fallback, and only while EGL does not own the
 *      window (early boot, or applet mode where the error applet is refused).
 */

#include <switch.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "util.h"
#include "config.h"
#include "app_paths.h"
#include "error.h"

static char s_msg[2048];
static volatile int s_pending = 0;
static volatile int s_display_owned = 0;

void error_set_display_owned(int owned) { s_display_owned = owned ? 1 : 0; }

int fatal_error_pending(void) { return s_pending; }

// A fatal nobody sees is the worst outcome available: fatal_error() ends in
// exit(1), which is a CLEAN exit, so Atmosphere writes no crash report and the
// console simply returns to the menu. The player is left with a game that
// "just closed" and nothing to send. So the message is recorded three ways,
// most reliable first -- a file on the SD card, then the log, then the applet.
//
// error_clear_fatal_file() removes this at boot, which makes the file's mere
// existence the answer to "did it die or did it quit?".
static void write_fatal_file(void) {
  char path[512];
  snprintf(path, sizeof(path), "%s/%s", app_paths_data_root(), FATAL_NAME);
  FILE *f = fopen(path, "w");
  if (!f) return;
  fputs("Super Mario 127 (sm127_nx) stopped with a fatal error.\n\n", f);
  fputs(s_msg, f);
  fputs("\n\nThe full log, including the engine's own errors, is in\n"
        LOG_NAME " next to the NRO.\n", f);
  fclose(f);
}

void error_clear_fatal_file(void) {
  char path[512];
  snprintf(path, sizeof(path), "%s/%s", app_paths_data_root(), FATAL_NAME);
  remove(path);
}

// The applet's strings are capped and s_msg is multi-line and up to 2 KB, so a
// rejected message is a real possibility. Retrying with one short line costs
// nothing and is the difference between a visible cause and a silent drop to
// the home menu.
static Result show_applet(void) {
  ErrorApplicationConfig c;
  Result rc = errorApplicationCreate(&c, s_msg, s_msg);
  if (R_SUCCEEDED(rc)) rc = errorApplicationShow(&c);
  if (R_SUCCEEDED(rc)) return rc;

  char brief[224];
  size_t n = 0;
  for (const char *p = s_msg; *p && n < sizeof(brief) - 1; p++)
    brief[n++] = (*p == '\n' || *p == '\r') ? ' ' : *p;
  brief[n] = 0;

  const Result rc2 = errorApplicationCreate(&c, brief, NULL);
  return R_SUCCEEDED(rc2) ? errorApplicationShow(&c) : rc;
}

static NX_NORETURN void show_and_exit(void) {
  write_fatal_file();

  const Result rc = show_applet();
  debugPrintf("[fatal] error applet -> 0x%x\n", rc);
  debugFlush();

  if (R_FAILED(rc) && !s_display_owned) {
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    consoleInit(NULL);
    printf("%s\n\nPress A to exit.", s_msg);
    consoleUpdate(NULL);

    while (appletMainLoop()) {
      padUpdate(&pad);
      if (padGetButtonsDown(&pad) & HidNpadButton_A) break;
      svcSleepThread(16 * 1000 * 1000);
    }
    consoleExit(NULL);
  }
  exit(1);
}

void fatal_error_show_pending(void) {
  show_and_exit();
}

void fatal_error(const char *fmt, ...) {
  va_list list;
  va_start(list, fmt);
  vsnprintf(s_msg, sizeof(s_msg), fmt, list);
  va_end(list);

  debugPrintf("[fatal] %s\n", s_msg);
  debugFlush();

  if (threadGetCurHandle() == envGetMainThreadHandle())
    show_and_exit();

  // Not the main thread: main()'s loop polls fatal_error_pending() and shows
  // it from there. Park forever; the process exits from the main thread.
  s_pending = 1;
  for (;;) svcSleepThread(1000000000ull);
}
