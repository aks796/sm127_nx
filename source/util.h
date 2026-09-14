/* util.h -- misc utility functions
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __UTIL_H__
#define __UTIL_H__

#include <stdint.h>

// Whether libnx's bsd/socket stack came up in userAppInit. The game's HTTP
// (level thumbnails, the LSS browser) works only when this is true.
int sm127_net_ready(void);

int debugPrintf(char *text, ...);
// call right before any hook that's about to actually terminate the process
// (abort/exit/RaiseFailFastException, nativeaot_shim.c) -- debugPrintf no
// longer flushes every line (too slow once the game is actually running),
// so anything about to end the process needs this first.
void debugFlush(void);
// Move buffered log lines to the SD card; main thread, from its loop. `flush`
// also fflush()es. See util.c.
void debugPump(int flush);

void cpu_boost(int on);

// the engine reads its stack-protector canary from tpidr_el0 + 0x28 (bionic).
void tls_setup_guard(void);

int ret0(void);
int retm1(void);

static inline void* armGetTlsRw(void) {
  void* ret;
  __asm__ ("mrs %x[data], s3_3_c13_c0_2" : [data] "=r" (ret));
  return ret;
}

static inline void armSetTlsRw(void *addr) {
  __asm__  ("msr s3_3_c13_c0_2, %0" : : "r"(addr));
}

static inline uint64_t umin(uint64_t a, uint64_t b) {
  return (a < b) ? a : b;
}


// Called from managed code via P/Invoke; see patches/STS2Switch/SwitchLog.cs.
// Per-line log flushing. ON during boot so a Data Abort cannot truncate the
// tail; main() turns it off once the frame loop is running, because that is
// when asset loading starts and synchronous SD writes get expensive.
void debugSetEagerFlush(int on);

// The wrapper's real load base, found via the NRO header. Offsets from it are
// directly usable with `addr2line -e sts2.elf`. See util.c.
uintptr_t sm127_wrapper_base(void);

void sm127_managed_log(int level, const char *message);

// Native side of LifecyclePatches.cs. sm127_game_is_background() is the game's
// OWN background state, which is independent of applet focus -- the game can
// background itself while the console is still focused.
void sm127_set_game_background(int background);
int  sm127_game_is_background(void);
void sm127_request_quit(void);

#endif
