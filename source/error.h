/* error.h -- error handler
 *
 * Copyright (C) 2021 fgsfds
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __ERROR_H__
#define __ERROR_H__

// Removes any FATAL_NAME left by a previous run. Call once at boot: the
// file existing afterwards then means THIS run died fatally.
void error_clear_fatal_file(void);

void fatal_error(const char *fmt, ...) __attribute__((noreturn));

// main.c calls this once EGL owns the default NWindow: from then on the
// console fallback would fault, so only the error applet is used.
void error_set_display_owned(int owned);

// A fatal_error() raised off the main thread is parked here; main()'s loop
// polls and shows it (applets must be launched from the main thread).
int  fatal_error_pending(void);
void fatal_error_show_pending(void) __attribute__((noreturn));

#endif
