/* util.c -- misc utility functions
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include "util.h"
#include "config.h"
#include "jni_fake.h"
#include "app_paths.h"

// ---------------------------------------------------------------------------
// Network.
//
// The game asks for it. SM127 fetches level thumbnails from Level Share Square
// over HTTP (scenes/menu/levels_list/cards/level/decoration.gd), its level
// browser downloads levels, and the LSSPing autoload polls the site at
// startup. Godot 3 does all of that with native BSD sockets -- the Java
// GodotNetUtils that jni_fake.c answers for only ever held a WiFi multicast
// lock on Android -- so the whole feature rests on libnx's bsd service.
//
// Before this, socketInitializeDefault() was called only as a side effect of
// DEBUG_LOG's nxlink redirect, and socketExit() was called straight back
// whenever no nxlink host answered. So on a normal boot the stack was up for
// microseconds and then gone, every HTTP request failed, and a level card
// whose thumbnail lives on LSS drew blank. A release build (DEBUG_LOG off) had
// no userAppInit at all and so never had a network either.
//
// Failure is not fatal: without a connection the game behaves as it did
// before, which is how it behaves on a phone in aeroplane mode.
static Result s_net_rc;
static int    s_net_ready;

int sm127_net_ready(void) { return s_net_ready; }

static void sm127_net_init(void) {
  s_net_rc = socketInitializeDefault();
  s_net_ready = R_SUCCEEDED(s_net_rc);
}

static void sm127_net_exit(void) {
  if (!s_net_ready) return;
  socketExit();
  s_net_ready = 0;
}

#if DEBUG_LOG

static int s_nxlinkSock = -1;
static FILE *s_log = NULL; // persistent log handle, buffered (see debugFlush)

// 1 until the frame loop is proven healthy; see debugPrintf. Starts on so the
// riskiest phase -- module load, GC init, managed startup -- is never lost to
// a Data Abort truncating the buffer.
static int s_eager_flush = 1;


// ---------------------------------------------------------------------------
// Log sink.
//
// debugPrintf used to vfprintf straight into the log FILE from whichever thread
// called it -- the frame thread included -- while main() fflush()ed that same
// FILE to the SD card once a second. newlib's FILE lock serialises the two, so
// a frame-thread print that landed during a flush sat out an SD write. During
// boot every line was also fflush()ed synchronously, and the tileset load
// (312 identical engine errors, two lines each) turned that into seconds.
//
// Lines now go into an in-memory ring under a mutex that is only ever held for
// a memcpy. The main thread moves them to the FILE (debugPump). The crash and
// fatal paths still drain synchronously through debugFlush, so a fault loses
// nothing already printed. Engine output repeated verbatim is kept the first
// LOG_KEEP_REPEATS times and then counted, with one summary per flush.
// ---------------------------------------------------------------------------
#define LOG_RING_SIZE    (1u << 20)
#define LOG_LINE_MAX     1024      // on the caller's stack, which may be small
#define LOG_KEEP_REPEATS 3
#define LOG_SEEN_SLOTS   2048u

static char   s_ring[LOG_RING_SIZE];
static u64    s_ring_head, s_ring_tail;       // monotonic byte counts
static u64    s_ring_dropped;
static Mutex  s_ring_lock;                     // zero-initialised == unlocked
static struct { uint32_t hash; uint16_t count; } s_seen[LOG_SEEN_SLOTS];
static u64    s_suppressed, s_suppressed_reported;

static uint32_t log_hash(const char *p, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) { h ^= (uint8_t)p[i]; h *= 16777619u; }
  return h | 1;   // 0 marks an empty slot
}

// Engine and fd output only: the wrapper's own lines are rare and each one
// matters. The helper's [port] reports come through the engine and are kept.
static int log_is_engine_line(const char *p) {
  if (strncmp(p, "[godot] [port]", 14) == 0) return 0;
  return strncmp(p, "[godot] ", 8) == 0 || strncmp(p, "[fd", 3) == 0;
}

// Lock held. 1 = drop this line.
static int log_suppress(const char *p, size_t n) {
  const uint32_t h = log_hash(p, n);
  size_t i = h & (LOG_SEEN_SLOTS - 1);
  for (int probe = 0; probe < 32; probe++, i = (i + 1) & (LOG_SEEN_SLOTS - 1)) {
    if (s_seen[i].hash == h) {
      if (s_seen[i].count < 0xFFFF) s_seen[i].count++;
      if (s_seen[i].count > LOG_KEEP_REPEATS) { s_suppressed++; return 1; }
      return 0;
    }
    if (s_seen[i].hash == 0) { s_seen[i].hash = h; s_seen[i].count = 1; return 0; }
  }
  return 0;   // table crowded: never drop what cannot be tracked
}

// Lock held.
static void log_put(const char *p, size_t n) {
  const u64 used = s_ring_head - s_ring_tail;
  if (n > LOG_RING_SIZE - used) { s_ring_dropped += n; return; }
  const size_t at = (size_t)(s_ring_head % LOG_RING_SIZE);
  const size_t first = n < LOG_RING_SIZE - at ? n : LOG_RING_SIZE - at;
  memcpy(s_ring + at, p, first);
  memcpy(s_ring, p + first, n - first);
  s_ring_head += n;
}

// Lock held. Copies up to cap buffered bytes out and consumes them.
static size_t log_take(char *out, size_t cap) {
  u64 avail = s_ring_head - s_ring_tail;
  size_t n = avail < cap ? (size_t)avail : cap;
  const size_t at = (size_t)(s_ring_tail % LOG_RING_SIZE);
  const size_t first = n < LOG_RING_SIZE - at ? n : LOG_RING_SIZE - at;
  memcpy(out, s_ring + at, first);
  memcpy(out + first, s_ring, n - first);
  s_ring_tail += n;
  return n;
}

static int log_on_main_thread(void) {
  return threadGetCurHandle() == envGetMainThreadHandle();
}

// nxlink borrows the socket stack sm127_net_init() already brought up. It must
// NOT tear it down: that is what used to leave the game with no network on
// every boot where no nxlink host was listening -- which is every normal boot.
static void initNxLink(void) {
  if (!sm127_net_ready()) return;
  s_nxlinkSock = nxlinkStdio();   // -1 when nothing is listening; harmless
}

static void deinitNxLink(void) {
  if (s_nxlinkSock >= 0) {
    close(s_nxlinkSock);
    s_nxlinkSock = -1;
  }
}

// sdmc is mounted by the time userAppInit runs, so open the log once here
// instead of reopening it per line (the engine logs thousands of lines).
void userAppInit(void) {
  sm127_net_init();
  initNxLink();
  // Discovered, not compile-time: the app tree can be any directory under
  // /switch/. app_paths resolves from argv[0] / cwd / a scan, and caches.
  s_log = fopen(app_paths_log_file(), "w");
  if (!s_log) s_log = fopen(LOG_NAME, "w"); // fall back to the launch CWD
  // 64 KB: after boot the log is flushed only when this fills, so an SD
  // write happens every few hundred lines instead of every few dozen.
  if (s_log) setvbuf(s_log, NULL, _IOFBF, 64 * 1024);
  if (s_log) {
    fputs("== sm127_nx log open ==\n", s_log);
    fflush(s_log);
  }
  debugPrintf("[net] socket stack %s (0x%x)%s\n",
              s_net_ready ? "up" : "UNAVAILABLE -- online features will fail",
              s_net_rc, s_nxlinkSock >= 0 ? ", nxlink attached" : "");
}

void userAppExit(void) {
  debugPump(1);
  if (s_log) { fclose(s_log); s_log = NULL; }
  deinitNxLink();
  sm127_net_exit();
}

#else   // !DEBUG_LOG -- the network still has to come up.

void userAppInit(void) { sm127_net_init(); }
void userAppExit(void) { sm127_net_exit(); }

#endif

int debugPrintf(char *text, ...) {
#if DEBUG_LOG
  char line[LOG_LINE_MAX];
  va_list list;
  va_start(list, text);
  const int w = vsnprintf(line, sizeof(line), text, list);
  va_end(list);
  if (w <= 0) return 0;
  const size_t len = (size_t)w < sizeof(line) ? (size_t)w : sizeof(line) - 1;

  mutexLock(&s_ring_lock);
  if (!(log_is_engine_line(line) && log_suppress(line, len)))
    log_put(line, len);
  mutexUnlock(&s_ring_lock);

  // Eager (boot): still on the card line by line -- but written only from the
  // main thread, where an SD write cannot cost a frame. Every other thread's
  // lines are written by main()'s loop within one iteration.
  if (s_eager_flush && log_on_main_thread())
    debugPump(1);

  // nxlink mirror only when a host is actually listening.
  if (s_nxlinkSock >= 0)
    fwrite(line, 1, len, stdout);
#else
  (void)text;
#endif
  return 0;
}

void debugSetEagerFlush(int on) {
#if DEBUG_LOG
  s_eager_flush = on ? 1 : 0;
  debugPrintf("== per-line log flushing %s ==\n", on ? "ON" : "OFF (boot complete)");
  if (log_on_main_thread()) debugPump(1);
#else
  (void)on;
#endif
}

// Write buffered lines to the card. Normally the main thread's job, from its
// loop; `flush` also fflush()es and reports repeats that were suppressed.
void debugPump(int flush) {
#if DEBUG_LOG
  char chunk[4096];
  for (;;) {
    mutexLock(&s_ring_lock);
    const size_t n = log_take(chunk, sizeof(chunk));
    const u64 dropped = s_ring_dropped;
    s_ring_dropped = 0;
    const u64 sup = flush ? s_suppressed - s_suppressed_reported : 0;
    if (flush) s_suppressed_reported = s_suppressed;
    mutexUnlock(&s_ring_lock);

    if (s_log && n) fwrite(chunk, 1, n, s_log);
    if (s_log && dropped)
      fprintf(s_log, "[log] %llu bytes dropped: the log ring was full\n", (unsigned long long)dropped);
    if (s_log && sup)
      fprintf(s_log, "[log] %llu repeated engine lines not written (each kept %d times)\n",
              (unsigned long long)sup, LOG_KEEP_REPEATS);
    if (n < sizeof(chunk)) break;
  }
  if (flush && s_log) fflush(s_log);
#else
  (void)flush;
#endif
}

// Everything printed so far, on the card, now -- from any thread. The crash
// handler and fatal_error use this. Off the main thread it waits a bounded time
// for the ring lock: if this very thread faulted while holding it, waiting
// forever would lose the whole report.
void debugFlush(void) {
#if DEBUG_LOG
  if (log_on_main_thread()) { debugPump(1); return; }
  for (int i = 0; i < 200; i++) {
    if (mutexTryLock(&s_ring_lock)) {
      mutexUnlock(&s_ring_lock);
      debugPump(1);
      return;
    }
    svcSleepThread(1000000ull);
  }
  if (s_log) fflush(s_log);
#endif
}

// Shared TLS block for the engine stack-protector guard at tpidr_el0 + 0x28.
static uint8_t s_tls_block[0x1000] __attribute__((aligned(16)));

void tls_setup_guard(void) {
  *(uint64_t *)(s_tls_block + 0x28) = 0x0123456789ABCDEFull;
  armSetTlsRw(s_tls_block);
}

// boost the CPU to 1785MHz while loading
void cpu_boost(int on) {
  appletSetCpuBoostMode(on ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
}

int ret0(void) { return 0; }

int retm1(void) { return -1; }


// ---------------------------------------------------------------------------
// Logging from managed code.
//
// STS2Switch.SwitchLog P/Invokes this so patched game code lands in the same
// log as the wrapper. Interleaving the two is what makes a boot failure
// readable -- a managed error next to the native call that caused it.
//
// Exported (not static) so the NativeAOT payload's "__Internal" DllImport
// resolves it: the payload is loaded into this process image, so the dynamic
// linker finds it without a real shared library.
// ---------------------------------------------------------------------------

void sm127_managed_log(int level, const char *message) {
  if (!message) return;
  debugPrintf("[managed%s] %s\n", level ? " ERROR" : "", message);
}

// ---------------------------------------------------------------------------
// Native side of patches/STS2Switch/LifecyclePatches.cs.
//
// sm127_set_game_background tells the wrapper the GAME has entered its own
// background mode. That is not the same as applet focus loss: the game can
// background itself (a modal, an alt-tab equivalent) while the console is
// still focused. main.c already pauses the mixer on focus loss; this is the
// other direction and the two are deliberately independent.
// ---------------------------------------------------------------------------

static volatile int s_game_background = 0;

void sm127_set_game_background(int background) {
  s_game_background = background ? 1 : 0;
}

int sm127_game_is_background(void) { return s_game_background; }

// NGame.Quit routes here rather than calling OS.Kill, so the frame loop can
// unwind normally: saves flush, GDExtensions get their teardown, and
// nx_input_shutdown writes pointer.cfg. Reuses jni_quit_requested, which
// main.c already polls, instead of adding a second quit path that could
// disagree with it.
void sm127_request_quit(void) {
  jni_quit_requested = 1;
}


// ---------------------------------------------------------------------------
// The wrapper's own load base.
//
// log_code_addr and the crash handler both need to turn a wrapper address into
// an offset usable with `addr2line -e sm127.elf`. The existing anchor is
//
//     ((uintptr_t)&main) & ~0xFFFFF
//
// which is main's address rounded down to 1 MB -- not the module base, so
// every "sts2+0x..." the watchdog has printed resolves to the wrong symbol.
// Run 11's hang dump showed the consequence: one thread's stack read as
// _eglFindDisplay, execute_cfa_program_generic and __ssvfscanf_r together,
// which is not a call chain any program has.
//
// hbloader maps the NRO with its header at the module base, and that header
// carries "NRO0" at offset 0x10. Scanning back a page at a time from a known
// code address finds it exactly. Reads are svcQueryMemory-guarded, because
// walking off the front of the mapping is otherwise a fault inside the very
// code meant to explain faults.
// ---------------------------------------------------------------------------

#define NRO_MAGIC 0x304F524E   /* "NRO0" */

uintptr_t sm127_wrapper_base(void) {
  static uintptr_t cached;
  if (cached) return cached;

  uintptr_t p = ((uintptr_t)&sm127_wrapper_base) & ~0xFFFULL;
  for (int i = 0; i < 8192; i++, p -= 0x1000) {   // up to 32 MB back
    MemoryInfo mi;
    u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, p))) break;
    if (mi.type == MemType_Unmapped || !(mi.perm & Perm_R)) break;
    if (*(const volatile uint32_t *)(p + 0x10) == NRO_MAGIC) {
      cached = p;
      return cached;
    }
  }

  // Fall back to the old anchor rather than returning nothing. It is wrong,
  // but it is wrong the way every previous log was wrong, which is better than
  // a zero that makes every address look like a module offset.
  cached = ((uintptr_t)&sm127_wrapper_base) & ~0xFFFFFULL;
  debugPrintf("[base] NRO header not found; falling back to the 1MB anchor "
              "(%p) -- wrapper offsets will NOT match sm127.elf\n", (void *)cached);
  return cached;
}
