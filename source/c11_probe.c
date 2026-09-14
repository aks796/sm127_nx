/* c11_probe.c -- does this platform's C11 threading actually work?
 *
 * Mesa leans on C11 threads hard: 53 references to mtx_lock/mtx_unlock in
 * libvulkan.a alone, plus cnd_* and call_once. And nvkmd-switch -- the
 * Switch-specific backend -- carries explicit checks for them failing:
 *
 *     nvkmd-switch: mtx_init failed
 *     nvkmd-switch: cnd_init failed
 *     nvkmd-switch: cnd_broadcast failed
 *     nvkmd-switch: cnd_timedwait failed
 *
 * Someone wrote those because they expected them to be reachable.
 *
 * These come from devkitPro's newlib, not from the futex shim, and "it linked"
 * says nothing about whether it works. A stub that returns success without
 * locking links perfectly and corrupts everything above it; a cnd_timedwait
 * that returns immediately turns every wait into a spin.
 *
 * So this asks. It runs once at startup, costs a few milliseconds, and either
 * says nothing interesting or names the broken primitive before any of Mesa's
 * shader compilation depends on it.
 *
 * DELIBERATELY SINGLE-THREADED. A probe that spawns threads to test locking
 * can hang at boot on exactly the platforms it exists to diagnose, and a hang
 * during startup is far worse than an unanswered question. What can be checked
 * without another thread:
 *
 *   - the calls return success at all
 *   - a recursive/timed wait actually consumes time rather than returning at
 *     once, which is the failure mode that produces spinning
 *   - call_once runs its function exactly once
 *
 * What it cannot check is mutual exclusion under contention. That needs two
 * threads and is not worth a boot hang.
 *
 * MIT license; see LICENSE.
 */

#include <time.h>
#include <switch.h>

#include "util.h"
#include "c11_probe.h"

// <threads.h> is C11-optional and devkitPro's newlib may not ship it, even
// though Mesa links against mtx_lock and friends -- Mesa provides its own
// declarations in that case.
//
// Guarded rather than assumed: a missing header here is a build failure for a
// diagnostic, which is the worst trade available. If it is absent the probe
// says so and the build carries on, and the absence is itself worth knowing --
// it would mean Mesa's C11 symbols resolve to something other than newlib's.
#if defined(__has_include)
#  if __has_include(<threads.h>)
#    define STS2_HAVE_THREADS_H 1
#  endif
#endif

#ifdef STS2_HAVE_THREADS_H
#include <threads.h>

/* Compare against 0, NOT thrd_success.
 *
 * On this platform thrd_success is 4 -- confirmed on hardware:
 *
 *     [c11] NOTE: platform thrd_success=4, we return 0 (Mesa's).
 *
 * Mesa was compiled against its own c11/threads.h where success is 0, and
 * `cbnz w0` after each mtx_init call in vk_instance.c.o proves it treats any
 * non-zero return as failure. nx_c11.c therefore returns Mesa's numbers.
 *
 * Which left this probe comparing the shim's correct 0 against the platform's
 * 4 and reporting "mtx_init FAILED" while everything worked -- the mirror
 * image of the original bug, where the probe and the platform agreed with each
 * other and both disagreed with Mesa.
 *
 * The question worth asking is not "does this match my header" but "does this
 * match what the CONSUMER expects", and the consumer is Mesa. */
#define C11_OK       0
#define C11_TIMEDOUT 4   // Mesa's value, same reasoning as C11_OK above
#endif

static int g_once_calls;
static void once_target(void) { g_once_calls++; }

#ifndef STS2_HAVE_THREADS_H

void sm127_c11_probe(void) {
  debugPrintf("[c11] <threads.h> not available; probe skipped.\n"
              "[c11] Mesa still calls mtx_lock/cnd_wait/call_once, so those "
              "resolve to\n"
              "[c11] something -- just not to a newlib header this build can "
              "see.\n");
  debugFlush();
}

#else

void sm127_c11_probe(void) {
  int bad = 0;

  // --- mtx: init, lock, unlock, destroy -----------------------------------
  {
    mtx_t m;
    if (mtx_init(&m, mtx_plain) != C11_OK) {
      debugPrintf("[c11] mtx_init FAILED -- Mesa checks for this by name "
                  "(\"nvkmd-switch: mtx_init failed\")\n");
      bad++;
    } else {
      if (mtx_lock(&m) != C11_OK)   { debugPrintf("[c11] mtx_lock FAILED\n");   bad++; }
      if (mtx_unlock(&m) != C11_OK) { debugPrintf("[c11] mtx_unlock FAILED\n"); bad++; }
      mtx_destroy(&m);
    }
  }

  // --- cnd_timedwait: does it actually wait? ------------------------------
  //
  // The interesting failure is not an error return, it is SUCCESS returned
  // instantly. Rust and Mesa both treat a wake as possibly spurious and loop,
  // so a cnd_timedwait that returns immediately does not break correctness --
  // it turns every wait into a busy loop, and the CPU cost lands on whichever
  // thread was supposed to be asleep.
  //
  // Waiting on a condvar nobody will signal, with a 20 ms deadline, must take
  // ~20 ms and report thrd_timedout. Anything much faster means it is not
  // waiting.
  {
    mtx_t m;
    cnd_t c;
    if (mtx_init(&m, mtx_plain) != C11_OK) {
      debugPrintf("[c11] mtx_init FAILED (cnd test skipped)\n");
      bad++;
    } else if (cnd_init(&c) != C11_OK) {
      debugPrintf("[c11] cnd_init FAILED -- Mesa checks for this by name\n");
      bad++;
      mtx_destroy(&m);
    } else {
      struct timespec deadline;
      clock_gettime(CLOCK_REALTIME, &deadline);
      deadline.tv_nsec += 20 * 1000 * 1000;          // 20 ms
      if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }

      const uint64_t t0 = armTicksToNs(armGetSystemTick());
      mtx_lock(&m);
      const int r = cnd_timedwait(&c, &m, &deadline);
      mtx_unlock(&m);
      const uint64_t elapsed_ms = (armTicksToNs(armGetSystemTick()) - t0) / 1000000ull;

      if (r != C11_TIMEDOUT) {
        debugPrintf("[c11] cnd_timedwait returned %d, expected timedout "
                    "(%d)\n", r, C11_TIMEDOUT);
        bad++;
      }
      if (elapsed_ms < 10) {
        debugPrintf("[c11] *** cnd_timedwait waited %llu ms for a 20 ms "
                    "deadline.\n"
                    "[c11] *** It is NOT sleeping. Every Mesa wait on this "
                    "path becomes a spin,\n"
                    "[c11] *** which is CPU burnt on threads that should be "
                    "idle.\n",
                    (unsigned long long)elapsed_ms);
        bad++;
      }
      cnd_destroy(&c);
      mtx_destroy(&m);
    }
  }

  // --- call_once: exactly once -------------------------------------------
  {
    static once_flag f = ONCE_FLAG_INIT;
    call_once(&f, once_target);
    call_once(&f, once_target);
    call_once(&f, once_target);
    if (g_once_calls != 1) {
      debugPrintf("[c11] *** call_once ran the initialiser %d times, expected "
                  "1.\n"
                  "[c11] *** 0 means lazily-initialised Mesa state stays "
                  "EMPTY -- that is the\n"
                  "[c11] *** shape of the NULL glsl_type context. More than 1 "
                  "means it is\n"
                  "[c11] *** re-initialised under whoever is already using "
                  "it.\n", g_once_calls);
      bad++;
    }
  }

  if (bad)
    debugPrintf("[c11] %d C11 threading problem(s). Mesa's compiler and "
                "nvkmd-switch both depend on these.\n", bad);
  else
    debugPrintf("[c11] mtx/cnd/call_once behave correctly\n");
  debugFlush();
}

#endif  /* STS2_HAVE_THREADS_H */
