/* libc_shim.c -- bionic<->newlib libc wrappers for libsotn.so. Converting
 * wrappers where the ABIs differ; matches are forwarded from imports.c.
 * MIT license; see LICENSE. */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <malloc.h>
#include <time.h>
#include <sys/stat.h>
#include <switch.h>

#include "config.h"
#include "libc_shim.h"
#include "util.h"
#include "net_shim.h"

// fortify (_chk): ignore the object-size argument
void *__memcpy_chk_fake(void *dst, const void *src, size_t n, size_t dstlen) {
  (void)dstlen; return memcpy(dst, src, n);
}
void *__memmove_chk_fake(void *dst, const void *src, size_t n, size_t dstlen) {
  (void)dstlen; return memmove(dst, src, n);
}
char *__strcpy_chk_fake(char *dst, const char *src, size_t dstlen) {
  (void)dstlen; return strcpy(dst, src);
}
size_t __strlen_chk_fake(const char *s, size_t slen) {
  (void)slen; return strlen(s);
}
int __vsnprintf_chk_fake(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, va_list va) {
  (void)flag; (void)slen; return vsnprintf(s, maxlen, fmt, va);
}
int __vsprintf_chk_fake(char *s, int flag, size_t slen, const char *fmt, va_list va) {
  (void)flag; (void)slen; return vsprintf(s, fmt, va);
}
ssize_t __read_chk_fake(int fd, void *buf, size_t count, size_t buflen) {
  (void)buflen; return read(fd, buf, count);
}

static int gettid_fake(void) {
  u64 id = 1;
  if (R_SUCCEEDED(svcGetThreadId(&id, CUR_THREAD_HANDLE)) && id)
    return (int)(id & 0x7fffffff);
  return 1;
}

#define ARM64_SYS_GETTID 178
#define ARM64_SYS_FUTEX   98

// futex op numbers (linux/futex.h)
#define FUTEX_WAIT           0
#define FUTEX_WAKE           1
#define FUTEX_REQUEUE        3
#define FUTEX_CMP_REQUEUE    4
#define FUTEX_WAIT_BITSET    9
#define FUTEX_WAKE_BITSET   10
#define FUTEX_PRIVATE_FLAG  128
#define FUTEX_CLOCK_REALTIME 256


// FUTEX_WAIT's timeout is RELATIVE. FUTEX_WAIT_BITSET's is ABSOLUTE, against
// CLOCK_MONOTONIC unless FUTEX_CLOCK_REALTIME is also set.
//
// That distinction is not academic here: Rust uses FUTEX_WAIT only for
// UNTIMED waits, and FUTEX_WAIT_BITSET for every timed one -- exactly because
// it wants an absolute deadline that survives a spurious wake. So every
// Condvar::wait_timeout, every park_timeout, every deadline in the shader
// compiler arrives as op 9, and reading its timespec as a duration would turn
// "wake at 14:32:07" into "wait for 14 hours".
static s64 futex_timeout_ns(int op, const struct timespec *ts) {
  if (!ts) return -1;                       // block indefinitely

  const int64_t want = (int64_t)ts->tv_sec * 1000000000LL + (int64_t)ts->tv_nsec;

  if ((op & ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME)) != FUTEX_WAIT_BITSET)
    return want < 0 ? 0 : (s64)want;        // FUTEX_WAIT: already relative

  // Absolute: subtract now, on whichever clock the caller named.
  struct timespec now;
  const clockid_t clk = (op & FUTEX_CLOCK_REALTIME) ? CLOCK_REALTIME
                                                    : CLOCK_MONOTONIC;
  if (clock_gettime(clk, &now) != 0) return -1;

  const int64_t nownn = (int64_t)now.tv_sec * 1000000000LL + (int64_t)now.tv_nsec;
  const int64_t rel   = want - nownn;
  return rel <= 0 ? 0 : (s64)rel;           // already past: poll, do not block
}

// futex, on Horizon's own address-arbiter syscalls.
//
// This used to return ENOSYS for everything but gettid, and that had a cost
// nobody was looking for.
//
// **Mesa's NAK shader compiler is written in Rust**, and Rust's std::sync --
// Mutex, Condvar, RwLock, Once -- is futex-based on Linux. The symbols are
// right there in libvulkan.a:
//
//   _RNvMNtNtNtNtCs..._3std3sys4sync5mutex5futexNtB2_5Mutex14lock_contended
//   _RNvNtNtNtNtCs..._3std3sys3pal4unix5futex10futex_wake
//
// Rust's futex_wait treats an unrecognised errno as a spurious wakeup and
// returns immediately, so lock_contended falls back to spinning:
//
//     loop { spin(); futex_wait(...); }   // futex_wait returns at once
//
// Correctness survives -- the lock itself is a CAS, futex is only the parking
// mechanism -- but every contended lock in the shader compiler became a busy
// wait. That is the 100% CPU during shader compilation, and it is why the
// stutter is worst exactly where NAK is working hardest.
//
// Horizon has the primitive; it was simply never connected.
// svcWaitForAddress with ArbitrationType_WaitIfEqual IS futex_wait, and
// svcSignalToAddress with SignalType_Signal IS futex_wake, down to operating
// on a 32-bit word at a user address.
static long futex_shim(int32_t *uaddr, int op, int32_t val,
                       const struct timespec *ts) {
  if (!uaddr) { errno = EFAULT; return -1; }

  switch (op & ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME)) {
    case FUTEX_WAIT:
    case FUTEX_WAIT_BITSET: {
      // Both sleep only if *uaddr still equals val; they differ only in how
      // the timeout is expressed, which futex_timeout_ns() resolves.
      //
      // The bitset itself (6th argument) is ignored. Rust always passes
      // FUTEX_BITSET_MATCH_ANY, and honouring bitsets would mean tracking
      // which waiter registered which mask -- Horizon's arbiter has no such
      // notion, and no caller in this process uses a narrower mask.
      const s64 timeout = futex_timeout_ns(op, ts);

      // An already-expired deadline is answered here, not by the kernel.
      //
      // futex_timeout_ns returns 0 for a deadline in the past, and it is not
      // safe to assume a 0 nanosecond timeout means "poll" -- several Horizon
      // APIs read 0 as "no timeout", which would turn an expired
      // wait_timeout into a permanent sleep. That is the worst possible
      // failure for this shim: a hang, in the one path a caller added a
      // timeout specifically to avoid.
      //
      // The value is still checked first, because a futex whose word has
      // already changed must report EAGAIN rather than ETIMEDOUT even when
      // the deadline has passed.
      if (ts && timeout == 0) {
        if (__atomic_load_n(uaddr, __ATOMIC_ACQUIRE) != val) { errno = EAGAIN; return -1; }
        errno = ETIMEDOUT;
        return -1;
      }

      Result rc = svcWaitForAddress(uaddr, ArbitrationType_WaitIfEqual,
                                    (s64)val, timeout);
      if (R_SUCCEEDED(rc)) return 0;

      // Classified by kernel result, matching libnx's own use of this syscall
      // in nx/source/kernel/levent.c:
      //
      //   if (R_VALUE(res) == KERNELRESULT(TimedOut))     -> timed out
      //   if (R_VALUE(res) != KERNELRESULT(InvalidState)) -> "should not happen"
      //
      // so InvalidState is "the value did not match", which is futex's EAGAIN.
      //
      // An earlier version inferred these by re-reading the word, on the
      // grounds that naming KERNELRESULT identifiers was a guess. The libnx
      // headers say otherwise -- KernelError_TimedOut=117,
      // KernelError_InvalidState=125 -- and inference could misreport a value
      // that changed between the syscall returning and the re-read.
      if (R_VALUE(rc) == KERNELRESULT(InvalidState)) { errno = EAGAIN;    return -1; }
      if (R_VALUE(rc) == KERNELRESULT(TimedOut))     { errno = ETIMEDOUT; return -1; }

      // levent calls this case "should not happen" and asserts. A futex caller
      // has somewhere better to go: EINTR, which every one of them retries.
      debugPrintf("[futex] svcWaitForAddress -> unexpected 0x%x; reporting "
                  "EINTR\n", R_VALUE(rc));
      errno = EINTR;
      return -1;
    }

    case FUTEX_WAKE:
    case FUTEX_WAKE_BITSET: {
      // val is the MAXIMUM number to wake; INT32_MAX is how Rust spells "all".
      //
      // -1 for "all" is libnx's own idiom -- levent.c passes exactly that to
      // svcSignalToAddress -- so this is no longer a guess and the arbitrary
      // 4096 bound is gone with it.
      s32 count = (val < 0 || val == INT32_MAX) ? -1 : (s32)val;

      Result rc = svcSignalToAddress(uaddr, SignalType_Signal, 0, count);
      if (R_SUCCEEDED(rc)) return count < 0 ? 0 : count;

      // A wake with nobody parked is not an error to the caller. Horizon may
      // report it as one, and returning -1 there would make Rust think the
      // wake failed -- so report zero woken and carry on. Every futex caller
      // treats "woke nobody" as normal; none treats a failed wake as normal.
      return 0;
    }

    case FUTEX_REQUEUE:
    case FUTEX_CMP_REQUEUE:
      // Requeue moves waiters from one futex to another. Horizon's arbiter
      // cannot do that, and there is no correct emulation -- but waking them
      // all IS safe: every futex caller must already tolerate spurious wakes,
      // and a woken waiter re-checks its condition and re-parks on the right
      // address by itself. Slower than requeue, never wrong.
      //
      // Rust's Condvar does not use this (notify_all is fetch_add + wake_all),
      // so it should not fire; it is here so that if some other Mesa component
      // does, the result is a thundering herd rather than a permanent sleep.
      {
        static int warned;
        if (!warned) {
          warned = 1;
          debugPrintf("[futex] REQUEUE emulated as wake-all -- correct but "
                      "slower; nothing here was expected to use it\n");
        }
      }
      if (R_SUCCEEDED(svcSignalToAddress(uaddr, SignalType_Signal, 0, -1)))
        return 0;
      errno = EINVAL;
      return -1;

    default:
      // Unhandled op. Reported once per op rather than per call: Rust retries,
      // and a per-call line would bury the log.
      {
        static int seen[8], nseen;
        int known = 0;
        for (int i = 0; i < nseen; i++) if (seen[i] == op) { known = 1; break; }
        if (!known && nseen < 8) {
          seen[nseen++] = op;
          debugPrintf("[futex] unhandled op %d -- returning ENOSYS. Rust will "
                      "treat this as a spurious wake and spin.\n", op);
        }
      }
      errno = ENOSYS;
      return -1;
  }
}

long syscall_fake(long number, ...) {
  if (number == ARM64_SYS_GETTID) return gettid_fake();

  if (number == ARM64_SYS_FUTEX) {
    // Announced once. Without it the next log looks identical to the last
    // one -- a futex that works is silent, which is the point, and there
    // would be no way to tell the shim was reached at all.
    static int announced;
    if (!announced) {
      announced = 1;
      debugPrintf("[futex] SYS_futex is implemented (svcWaitForAddress / "
                  "svcSignalToAddress).\n"
                  "[futex] Rust locks in Mesa's NAK shader compiler now sleep "
                  "instead of spinning.\n");
      debugFlush();
    }

    va_list ap;
    va_start(ap, number);
    int32_t *uaddr = va_arg(ap, int32_t *);
    int      op    = va_arg(ap, int);
    int32_t  val   = (int32_t)va_arg(ap, int);
    const struct timespec *ts = va_arg(ap, const struct timespec *);
    va_end(ap);
    return futex_shim(uaddr, op, val, ts);
  }

  debugPrintf("[syscall] number=%ld -> ENOSYS\n", number);
  errno = ENOSYS;
  return -1;
}

void sincos_fake(double x, double *s, double *c) { *s = sin(x); *c = cos(x); }

// bionic clockids (REALTIME=0, MONOTONIC=1, ...) differ from newlib's, so the
// engine's clock_gettime(0) was rejected as EINVAL -> uncaught std::system_error.
// Translate the id; fall back to the libnx tick so it never fails. bionic and
// newlib timespec match on arm64 LP64.
int clock_gettime_fake(int clk, void *ts_) {
  struct timespec *ts = ts_;
  if (!ts) { errno = EFAULT; return -1; }
  clockid_t real = (clk == 0 || clk == 5) ? CLOCK_REALTIME : CLOCK_MONOTONIC;
  if (clock_gettime(real, ts) == 0)
    return 0;
  uint64_t ns = armTicksToNs(armGetSystemTick());
  ts->tv_sec = (int64_t)(ns / 1000000000ull);
  ts->tv_nsec = (int64_t)(ns % 1000000000ull);
  return 0;
}

// android_set_abort_message.
//
// bionic's hook for recording WHY a process is about to abort; the crash
// reporter reads it afterwards. The .NET PAL calls it on its fatal paths, and
// libc++ calls it from std::terminate.
//
// Discarding it threw away the one string that says what went wrong, on
// exactly the paths where nothing else gets to speak. Same silent-promise
// shape as the stdio writers above: it took the message, reported nothing, and
// the failure surfaced somewhere with no explanation attached.
void android_set_abort_message_fake(const char *msg) {
  if (!msg) return;
  debugPrintf("[abort-msg] %s\n", msg);
  debugFlush();   // an abort is imminent; do not let this sit in the buffer
}

size_t __ctype_get_mb_cur_max_fake(void) { return 1; }

#define BIONIC_SC_PAGESIZE 39
#define BIONIC_SC_PAGE_SIZE 40
#define BIONIC_SC_NPROCESSORS_CONF 96
#define BIONIC_SC_NPROCESSORS_ONLN 97
#define BIONIC_SC_PHYS_PAGES 98
#define BIONIC_SC_AVPHYS_PAGES 99

long sysconf_fake(int name) {
  long ret;
  switch (name) {
    case BIONIC_SC_PAGESIZE:
    case BIONIC_SC_PAGE_SIZE: ret = 0x1000; break;
    case BIONIC_SC_NPROCESSORS_CONF:
    case BIONIC_SC_NPROCESSORS_ONLN: ret = 3; break;
    case BIONIC_SC_PHYS_PAGES: ret = (3ll * 1024 * 1024 * 1024) / 0x1000; break;
    // "available" (as opposed to total) physical pages: the GC's periodic
    // memory-load check polls this. -1 (unsupported) previously here is a
    // plausible contributor to the tight-looking poll loop seen on
    // hardware (mprotect toggling the same page back to back) -- report a
    // generous, static fraction of total, same as no memory pressure ever
    // changing; still not real telemetry, but a value beats a hard error.
    case BIONIC_SC_AVPHYS_PAGES: {
      // REAL free memory, not a static 2 GB.
      //
      // The GC polls this to decide whether it is under memory pressure. A
      // constant answer means it believes 2 GB is free even when the process
      // is at 3185 of 3189 MB, so it never backs off and never triggers the
      // collection that would have saved it.
      //
      // Run 30 shows four of these polls immediately before the crash, which
      // is the GC asking a question it was being lied to about.
      //
      // svcGetInfo gives the truth for the same cost.
      uint64_t total = 0, used = 0;
      if (R_SUCCEEDED(svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0)) &&
          R_SUCCEEDED(svcGetInfo(&used,  InfoType_UsedMemorySize,  CUR_PROCESS_HANDLE, 0)) &&
          total >= used) {
        ret = (long)((total - used) / 0x1000);
      } else {
        ret = (2ll * 1024 * 1024 * 1024) / 0x1000;   // previous behaviour
      }
      break;
    }
    default: ret = -1; break;
  }
  debugPrintf("[sysconf] name=%d -> %ld\n", name, ret);
  return ret;
}

// signals: the engine installs a crash handler we never trigger; stub them
// sigaction.
//
// Returning 0 tells the caller "handler installed" when nothing was. That is
// survivable for most callers and NOT survivable for one: NativeAOT does not
// emit null checks on field access. It relies on the PAL installing a SIGSEGV
// handler that catches the resulting fault and rewrites it into a managed
// NullReferenceException with a stack trace.
//
// With this stub, the runtime believes it has that handler and does not, so
// every managed null reference is an unrecoverable hard fault with no
// exception, no message and no trace. Run 5's crash is exactly that.
//
// Still returns 0 -- reporting failure would make the PAL abort at startup,
// which is worse. But the handler is now RECORDED, so the crash handler can
// report that the runtime wanted one, and so that a future attempt at
// delivering the fault to it has somewhere to read it from. Signal delivery
// proper is not implemented: Horizon has no signals, and the exception handler
// would have to synthesise siginfo_t/ucontext_t and resume into the handler.
#define SIG_SLOTS 32
static void *s_sig_handlers[SIG_SLOTS];

// BIONIC's struct sigaction is NOT glibc's.
//
//   glibc aarch64:   handler first, then mask, then flags
//   bionic LP64:     int sa_flags FIRST, 4 bytes padding, THEN the handler
//                    union at offset 8, then sa_mask, then sa_restorer
//
// The first version of this read offset 0 and called it the handler. Hardware
// showed exactly what that produces:
//
//   [signal] sigaction(11) handler=0x4010000004
//   [signal] sigaction(8)  handler=0x10000004
//
// Neither is a code address. The low 32 bits of both are 4 -- SA_SIGINFO --
// i.e. sa_flags, with uninitialised padding in the top half. The payload is
// built for linux-bionic-arm64, so bionic's layout is the one that applies.
//
// Recording the right value matters beyond tidiness: it is the prerequisite
// for ever delivering a fault to the runtime's handler, which is what would
// turn a managed null dereference from an unrecoverable hard fault into a
// NullReferenceException with a stack trace.
struct bionic_sigaction {
  int   sa_flags;
  int   _pad;
  void *sa_handler_or_sigaction;   // offset 8
  // sa_mask, sa_restorer follow; not needed here
};

#define SA_SIGINFO_BIT 4

static int s_sig_flags[SIG_SLOTS];

int sigaction_fake(int sig, const void *act, void *oldact) {
  (void)oldact;
  if (act && sig > 0 && sig < SIG_SLOTS) {
    const struct bionic_sigaction *sa = act;
    void *h = sa->sa_handler_or_sigaction;
    s_sig_handlers[sig] = h;
    s_sig_flags[sig] = sa->sa_flags;

    // 11 = SIGSEGV, 7 = SIGBUS, 4 = SIGILL, 8 = SIGFPE
    if (sig == 11 || sig == 7 || sig == 4 || sig == 8)
      debugPrintf("[signal] sigaction(%d) handler=%p flags=0x%x%s"
                  " -- recorded, NOT installed\n",
                  sig, h, sa->sa_flags,
                  (sa->sa_flags & SA_SIGINFO_BIT)
                    ? " (SA_SIGINFO: 3-arg handler)" : " (1-arg handler)");
  }
  return 0;
}

int sm127_recorded_signal_flags(int sig) {
  return (sig > 0 && sig < SIG_SLOTS) ? s_sig_flags[sig] : 0;
}

void *sm127_recorded_signal_handler(int sig) {
  return (sig > 0 && sig < SIG_SLOTS) ? s_sig_handlers[sig] : NULL;
}
void *signal_fake(int sig, void *handler) { (void)sig; (void)handler; return NULL; }
// These returned success without touching the set, leaving the caller's
// sigset_t whatever the stack held. A caller that does sigemptyset() and then
// sigismember() gets garbage rather than "empty" -- a wrong answer, not a
// missing one, which is the harder kind to notice.
//
// bionic's sigset_t is 8 bytes on LP64, so one word covers signals 1..64.
// pvzultimate implements them the same way.
int sigemptyset_fake(void *set) {
  if (set) *(uint64_t *)set = 0;
  return 0;
}
int sigaddset_fake(void *set, int sig) {
  if (set && sig > 0 && sig <= 64) *(uint64_t *)set |= (1ull << (sig - 1));
  return 0;
}
int sigdelset_fake(void *set, int sig) {
  if (set && sig > 0 && sig <= 64) *(uint64_t *)set &= ~(1ull << (sig - 1));
  return 0;
}
int sigismember_fake(const void *set, int sig) {
  if (!set || sig <= 0 || sig > 64) return 0;
  return (*(const uint64_t *)set >> (sig - 1)) & 1;
}
int pthread_sigmask_fake(int how, const void *set, void *oldset) {
  (void)how; (void)set; (void)oldset; return 0;
}

// struct stat conversion (bionic aarch64 layout)
struct bionic_timespec { int64_t tv_sec; int64_t tv_nsec; };
struct bionic_stat {
  uint64_t st_dev, st_ino;
  uint32_t st_mode, st_nlink, st_uid, st_gid;
  uint64_t st_rdev, __pad1;
  int64_t st_size;
  int32_t st_blksize, __pad2;
  int64_t st_blocks;
  struct bionic_timespec st_atim, st_mtim, st_ctim;
  uint32_t __unused4, __unused5;
};

int stat_fake(const char *path, void *st) {
  struct stat in;
  char sb[640];
  path = sandbox_path(path, sb, sizeof(sb));
  char buf[640];
  const char *p = obb_resolve(path, buf, sizeof(buf));
  int rc = stat(p, &in);
#if VERBOSE_IO
  debugPrintf("stat(\"%s\"%s) -> %d\n", path, p != path ? " [obb->main.obb]" : "", rc);
#endif
  if (rc != 0) return -1;
  struct bionic_stat *out = st;
  memset(out, 0, sizeof(*out));
  out->st_dev = in.st_dev; out->st_ino = in.st_ino;
  out->st_mode = in.st_mode; out->st_nlink = in.st_nlink;
  out->st_uid = in.st_uid; out->st_gid = in.st_gid;
  out->st_rdev = in.st_rdev; out->st_size = in.st_size;
  out->st_blksize = in.st_blksize; out->st_blocks = in.st_blocks;
  out->st_atim.tv_sec = in.st_atime;
  out->st_mtim.tv_sec = in.st_mtime;
  out->st_ctim.tv_sec = in.st_ctime;
  return 0;
}

// locale: ignore the locale argument, use the C versions
void *newlocale_fake(int mask, const char *locale, void *base) {
  (void)mask; (void)locale; (void)base; return (void *)1;
}
void freelocale_fake(void *loc) { (void)loc; }
void *uselocale_fake(void *loc) { (void)loc; return (void *)1; }

long double strtold_l_fake(const char *s, char **end, void *loc) {
  (void)loc; return strtold(s, end);
}
long long strtoll_l_fake(const char *s, char **end, int base, void *loc) {
  (void)loc; return strtoll(s, end, base);
}
unsigned long long strtoull_l_fake(const char *s, char **end, int base, void *loc) {
  (void)loc; return strtoull(s, end, base);
}

int posix_memalign_fake(void **out, size_t align, size_t size) {
  void *p = memalign(align, size);
  if (!p) return ENOMEM;
  *out = p;
  return 0;
}

// stdio over the fake bionic __sF: libc++/SDL bind std streams to &__sF[N];
// these absorb accesses to those fake FILEs and forward everything else.
uint8_t fake_sF[3][0x100];

static int is_fake_file(const void *f) {
  const uint8_t *p = f, *base = (const uint8_t *)fake_sF;
  return p >= base && p < base + sizeof(fake_sF);
}

// Which of the three: 0 = stdin, 1 = stdout, 2 = stderr, -1 = not ours.
static int fake_file_index(const void *f) {
  const uint8_t *p = f, *base = (const uint8_t *)fake_sF;
  if (p < base || p >= base + sizeof(fake_sF)) return -1;
  return (int)((size_t)(p - base) / sizeof(fake_sF[0]));
}

// Mirror stdout/stderr into the log instead of discarding it.
//
// Every stdio writer below used to `return` a success value for a fake FILE
// and drop the bytes. That is the exact shape pvzultimate's handoff calls a
// SILENT PROMISE -- "a nop, a hardcoded constant, a dropped callback or an
// unsent one, and it always fails far from where it was made".
//
// It also made the port inconsistent with itself: vprintf_fake already routes
// printf() into the log, so bare printf was visible while fprintf(stderr, ...)
// was not. Anything the payload or libc++ printed on its way down went
// nowhere, which is part of why eight hardware runs produced no managed
// output at all.
//
// Truncation is deliberate: a runaway writer should cost a long line, not the
// log.
static void mirror_fake_write(int idx, const char *data, size_t len) {
#if DEBUG_LOG
  if (idx < 1 || !data || !len) return;         // stdin, or nothing to say
  if (len > 0x400) len = 0x400;
  debugPrintf("[fd%d] %.*s%s", idx, (int)len, data, data[len - 1] == '\n' ? "" : "\n");
#else
  (void)idx; (void)data; (void)len;
#endif
}

// Android-style filesystem sandbox: the app cannot touch "/" on a real
// device, and with the faked cwd of "/" godot builds cwd-derived write paths
// like "/saves/...". Rebase absolute paths that are outside the app's own
// directories into save_root; app paths (/switch/...) and device-prefixed
// paths pass through untouched.
const char *sandbox_path(const char *path, char *buf, size_t sz) {
  if (!path || path[0] != '/') return path;
  if (strncmp(path, "/switch", 7) == 0) return path;
  if (strncmp(path, "/dev", 4) == 0 || strncmp(path, "/proc", 5) == 0) return path;
  // Collapse a leading run of slashes.
  //
  // The engine builds paths from a cwd of "/" (getcwd_fake) and often ends up
  // with "//shader_cache/...", so a naive join gives
  // "/switch/sls2_nx/save//shader_cache/...". FatFs through newlib tolerates
  // that today -- the shader cache does get written -- but an empty path
  // component is not something to rely on, and it makes every log line
  // slightly wrong to read.
  while (path[0] == '/' && path[1] == '/') path++;

  snprintf(buf, sz, "%s%s", config.save_root, path);
  return buf;
}

// /dev/urandom | /dev/random: mbedtls seeds its CTR_DRBG from it (fopen +
// fread and/or open + read). Serve both through the system csrng.
static uint8_t urandom_marker; // its address doubles as the fake FILE *
#define URANDOM_FD 0x7F0FA7E

static int is_urandom_path(const char *p) {
  return p && strncmp(p, "/dev/", 5) == 0 && strstr(p + 5, "random") != NULL;
}
static int is_urandom_file(const void *f) { return f == (const void *)&urandom_marker; }

// Asked by imports.c's fstat_fake. A predicate, so URANDOM_FD stays defined in
// exactly one place.
int sm127_is_urandom_fd(int fd) { return fd == URANDOM_FD; }

// chdir.
//
// Every other path-taking POSIX call in the import table goes through a
// sandbox_path() wrapper -- open, fopen, stat, lstat, access, mkdir, rmdir,
// unlink, remove, rename, opendir, truncate, statvfs, realpath, readlink.
// chdir was the single exception, pointing straight at newlib.
//
// That asymmetry is what broke the shader cache. getcwd_fake reports "/" to
// mimic Android, so Godot's DirAccessUnix::change_dir builds paths like
//     /shader_cache/CanvasSdfShaderRD
// and calls chdir() on them. Unrebased, that is the SD card root, where no
// such directory exists -- while mkdir(), which IS rebased, had created the
// real one under <save_root>/shader_cache/.
//
// So the engine created 128 cache directories and then failed to enter any of
// them, retried make_dir on every launch, got ERR_ALREADY_EXISTS, and printed
// "Unable to create shader cache directory" for each. Every shader recompiled
// from source on every boot.
//
// Zero makeDir/dirOpen JNI calls in the log is what gave it away: the engine
// was not using DirAccessJAndroid for this at all, so all the work spent on
// the JNI directory surface could never have fixed it.
int chdir_fake(const char *path) {
  char buf[512];
  const char *real = sandbox_path(path, buf, sizeof(buf));
  const int rc = chdir(real);
  if (rc != 0)
    debugPrintf("[fs] chdir(\"%s\") -> \"%s\" failed (errno %d)\n",
                path ? path : "(null)", real, errno);
  return rc;
}

size_t fwrite_fake(const void *ptr, size_t size, size_t n, FILE *f) {
  if (!f || !ptr) return 0;
  if (is_fake_file(f)) {
    mirror_fake_write(fake_file_index(f), ptr, size * n);
    return n;
  }
  return fwrite(ptr, size, n, f);
}

// NULL-safe: the engine fread()s into a malloc'd buffer without checking the
// FILE* or the buffer, so guard against a write-to-0x0 Data Abort.
size_t fread_fake(void *ptr, size_t size, size_t n, FILE *f) {
  if (!f || !ptr) return 0;
  if (is_urandom_file(f)) {
    if (size && n) randomGet(ptr, size * n);
    return n;
  }
  if (is_fake_file(f)) return 0;
  return fread(ptr, size, n, f);
}

int fputc_fake(int c, FILE *f) {
  if (!f) return -1;
  if (is_fake_file(f)) {
    // Buffered per stream: a caller emitting a message one character at a time
    // would otherwise produce one log line per character. Flushed on newline
    // or when full.
    static char line[3][512];
    static size_t len[3];
    const int idx = fake_file_index(f);
    if (idx >= 1) {
      line[idx][len[idx]++] = (char)c;
      if (c == '\n' || len[idx] >= sizeof(line[0]) - 1) {
        mirror_fake_write(idx, line[idx], len[idx]);
        len[idx] = 0;
      }
    }
    return c;
  }
  return fputc(c, f);
}
int fflush_fake(FILE *f) {
  if (!f || is_fake_file(f) || is_urandom_file(f)) return 0;
  return fflush(f);
}
int fclose_fake(FILE *f) {
  if (!f || is_fake_file(f) || is_urandom_file(f)) return 0;
  return fclose(f);
}
int ferror_fake(FILE *f) {
  if (!f || is_fake_file(f) || is_urandom_file(f)) return 0;
  return ferror(f);
}
int fprintf_fake(FILE *f, const char *fmt, ...) {
  if (is_fake_file(f)) {
    char buf[0x400];
    va_list va; va_start(va, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);
    if (n > 0) mirror_fake_write(fake_file_index(f), buf, (size_t)n);
    return n < 0 ? 0 : n;
  }
  va_list va; va_start(va, fmt);
  int ret = vfprintf(f, fmt, va);
  va_end(va);
  return ret;
}
int vfprintf_fake(FILE *f, const char *fmt, va_list va) {
  if (is_fake_file(f)) {
    char buf[0x400];
    int n = vsnprintf(buf, sizeof(buf), fmt, va);
    if (n > 0) mirror_fake_write(fake_file_index(f), buf, (size_t)n);
    return n < 0 ? 0 : n;
  }
  return vfprintf(f, fmt, va);
}
int fseek_fake(FILE *f, long off, int whence) {
  if (!f || is_fake_file(f)) return -1;
  if (is_urandom_file(f)) return 0;
  return fseek(f, off, whence);
}
int putchar_fake(int c) { return c; }
// Dropped its argument unconditionally, while vprintf_fake right above routes
// printf() into the log. Same stream, opposite behaviour, and puts() is what a
// simple diagnostic in native code most often uses.
int puts_fake(const char *s) {
  if (s) mirror_fake_write(1, s, strlen(s));
  return 0;
}

// remaining FILE surface Godot/libc++ touch, with fake-std-stream guards
int is_fake_std_file(const void *f) { return is_fake_file(f); }

char *fgets_fake(char *s, int n, FILE *f) {
  if (!f || !s || is_fake_file(f) || is_urandom_file(f)) return NULL;
  return fgets(s, n, f);
}
int getc_fake(FILE *f) {
  if (is_urandom_file(f)) {
    uint8_t b;
    randomGet(&b, 1);
    return b;
  }
  if (!f || is_fake_file(f)) return -1; // EOF
  return getc(f);
}
int ungetc_fake(int c, FILE *f) {
  if (!f || is_fake_file(f) || is_urandom_file(f)) return -1;
  return ungetc(c, f);
}
void setbuf_fake(FILE *f, char *buf) {
  if (f && !is_fake_file(f) && !is_urandom_file(f)) setbuf(f, buf);
}
void rewind_fake(FILE *f) {
  if (f && !is_fake_file(f) && !is_urandom_file(f)) rewind(f);
}
// bionic fpos_t is a 64-bit offset; carry it as one
int fgetpos_fake(FILE *f, void *pos) {
  if (is_urandom_file(f)) { if (pos) *(int64_t *)pos = 0; return 0; }
  if (!f || !pos || is_fake_file(f)) return -1;
  long p = ftell(f);
  if (p < 0) return -1;
  *(int64_t *)pos = p;
  return 0;
}
int fsetpos_fake(FILE *f, const void *pos) {
  if (is_urandom_file(f)) return 0;
  if (!f || !pos || is_fake_file(f)) return -1;
  return fseek(f, (long)*(const int64_t *)pos, SEEK_SET);
}
int fseeko_fake(FILE *f, int64_t off, int whence) {
  if (is_urandom_file(f)) return 0;
  if (!f || is_fake_file(f)) return -1;
  return fseek(f, (long)off, whence);
}
int64_t ftello_fake(FILE *f) {
  if (!f || is_fake_file(f) || is_urandom_file(f)) return is_urandom_file(f) ? 0 : -1;
  return ftell(f);
}
long ftell_fake(FILE *f) {
  if (is_urandom_file(f)) return 0;
  if (!f || is_fake_file(f)) return -1;
  return ftell(f);
}
int feof_fake(FILE *f) {
  if (is_urandom_file(f)) return 0; // never runs dry
  if (!f || is_fake_file(f)) return 1;
  return feof(f);
}
int fileno_fake(FILE *f) {
  if (is_urandom_file(f)) return URANDOM_FD;
  if (!f || is_fake_file(f)) return -1;
  return fileno(f);
}
int fputs_fake(const char *s, FILE *f) {
  if (!f) return 0;
  if (is_fake_file(f)) {
    if (s) mirror_fake_write(fake_file_index(f), s, strlen(s));
    return 0;
  }
  return fputs(s ? s : "", f);
}
int vprintf_fake(const char *fmt, va_list va) {
#if DEBUG_LOG
  char buf[0x800];
  int n = vsnprintf(buf, sizeof(buf), fmt, va);
  debugPrintf("%s", buf);
  return n;
#else
  (void)fmt; (void)va;
  return 0;
#endif
}

// The game builds the standard Android OBB name (main.<versionCode>.<pkg>.obb)
// and opens it under getObbPath(). Our release ships it as plain "main.obb", so
// when a "main.*.obb" open fails, retry with "main.obb" in the same directory.
// Returns 1 and fills `out` when a rewrite applies.
int obb_fallback_path(const char *path, char *out, size_t outsz) {
  if (!path) return 0;
  const char *slash = strrchr(path, '/');
  const char *base = slash ? slash + 1 : path;
  size_t blen = strlen(base);
  if (strncmp(base, "main.", 5) != 0 || blen < 5 || strcmp(base + blen - 4, ".obb") != 0)
    return 0;
  if (blen == 8) return 0; // already exactly "main.obb"
  if (slash)
    snprintf(out, outsz, "%.*s/main.obb", (int)(slash - path), path);
  else
    snprintf(out, outsz, "main.obb");
  return 1;
}

// If `path` is a versioned "main.*.obb", resolve it to the shipped "main.obb"
// in the same directory (our release ships it un-versioned). No stat/access
// probe here: it's on the hot asset-load path, so keep it syscall-free.
const char *obb_resolve(const char *path, char *buf, size_t bufsz) {
  if (path && obb_fallback_path(path, buf, bufsz)) return buf;
  return path;
}

// OBB read-ahead helpers (defined below)
static int path_is_obb(const char *p);
static void obb_track(int fd);

// Bionic O_* -> newlib O_*.
//
// These are #defines the caller compiled in, and the two libcs share almost
// none of them. Worse, several COLLIDE -- the same bit means different things:
//
//     bionic O_APPEND  0x400  ==  newlib O_TRUNC   0x400
//     bionic O_TRUNC   0x200  ==  newlib O_CREAT   0x200
//     bionic O_CREAT   0x040  ==  (nothing useful in newlib)
//
// So an engine opening a file to APPEND was asking newlib to TRUNCATE it, and
// an engine asking to truncate was asking newlib to create. Passing the word
// through unchanged is not a no-op; it is a different request.
//
// The low two bits (O_RDONLY/WRONLY/RDWR) and O_ACCMODE are the same on both
// and pass through as-is.
//
// Anything not listed is dropped rather than forwarded: an unrecognised bit in
// newlib's namespace could mean something, and asking for a flag the caller
// did not is worse than missing one it did.
static int oflags_bionic_to_newlib(int f) {
  int out = f & 0x3;                       // O_RDONLY/O_WRONLY/O_RDWR
  if (f & 0x000040) out |= O_CREAT;        // bionic O_CREAT
  if (f & 0x000080) out |= O_EXCL;         // bionic O_EXCL
  if (f & 0x000200) out |= O_TRUNC;        // bionic O_TRUNC
  if (f & 0x000400) out |= O_APPEND;       // bionic O_APPEND
#ifdef O_NONBLOCK
  if (f & 0x000800) out |= O_NONBLOCK;
#endif
#ifdef O_DIRECTORY
  if (f & 0x010000) out |= O_DIRECTORY;
#endif
#ifdef O_CLOEXEC
  if (f & 0x080000) out |= O_CLOEXEC;
#endif
#ifdef O_NOCTTY
  if (f & 0x000100) out |= O_NOCTTY;
#endif
  return out;
}

// open() with the OBB-name fallback (PhysFS on Android opens archives via the
// POSIX open path, not fopen).
int open_fake(const char *path, int flags, ...) {
  mode_t mode = 0;
  if (flags & BIONIC_O_CREAT) {
    va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap);
  }
  if (is_urandom_path(path)) {
    // Logged unconditionally, not under VERBOSE_IO. Guid.NewGuid() failing
    // with CryptographicException (hardware run 12) means .NET's RNG path is
    // returning failure, and the first thing to establish is whether it comes
    // through here at all.
    debugPrintf("[rng] open(\"%s\") -> URANDOM_FD\n", path);
    return URANDOM_FD;
  }
  char sb[640];
  path = sandbox_path(path, sb, sizeof(sb));
  char buf[640];
  const char *p = ((flags & O_ACCMODE) == O_RDONLY) ? obb_resolve(path, buf, sizeof(buf)) : path;
  const int nflags = oflags_bionic_to_newlib(flags);
  int fd = open(p, nflags, mode);
  if (fd >= 0 && (flags & O_ACCMODE) == O_RDONLY && path_is_obb(p))
    obb_track(fd); // enable read-ahead for this OBB descriptor
#if VERBOSE_IO
  debugPrintf("open(\"%s\"%s) bionic flags=%x -> newlib %x -> %d\n",
              path, p != path ? " [obb->main.obb]" : "", flags, nflags, fd);
#endif
  return fd;
}

int access_fake(const char *path, int mode) {
  // The game's bundled PhysFS probes access("/proc") to decide whether it can
  // resolve its own executable path via readlink("/proc/self/exe"). Switch has
  // no /proc, so claim it exists and satisfy the readlink below; otherwise
  // PHYSFS_init's calcBaseDir returns NULL and PHYSFS_init fails, leaving its
  // stateLock NULL -> PHYSFS_mount then crashes locking a NULL mutex.
  if (path && strcmp(path, "/proc") == 0)
    return 0;
  char sb[640];
  path = sandbox_path(path, sb, sizeof(sb));
  char buf[640];
  const char *p = obb_resolve(path, buf, sizeof(buf));
  return access(p, mode);
}

// ---------------------------------------------------------------------------
// OBB read-ahead: the game reads its (stored, uncompressed) OBB through PhysFS
// with many small read()/lseek() calls. Each is a round trip to the SD card.
// We wrap read()/lseek()/close() for OBB file descriptors with a large
// per-fd read-ahead buffer, collapsing the small reads into few big ones.
// ---------------------------------------------------------------------------

#define OBB_NFD 8
#define OBB_RA_MIN (32 * 1024)
#define OBB_RA_MAX (256 * 1024)

#define OBB_BUF_SZ (OBB_RA_MAX + 0x2000) // + alignment slack

typedef struct {
  int fd;            // -1 = free slot
  off_t pos;         // logical file position
  off_t size;        // file size
  off_t buf_start;   // file offset of the buffered region
  size_t buf_len;    // valid bytes in buf
  uint8_t *buf;      // OBB_BUF_SZ, 0x1000-aligned (allocated once per slot)
  Mutex lock;
  size_t ra;         // adaptive read-ahead window (grows on sequential access)
  off_t seq_next;    // file offset expected for the next sequential read
} ObbFd;

static ObbFd s_obb[OBB_NFD];
static Mutex s_obb_table_lock;
static int s_obb_inited;

// global I/O instrumentation (per-fd is too short-lived to be meaningful)
static uint64_t g_obb_opens;    // times the OBB was opened
static uint64_t g_obb_reads;    // read() calls the game made on the OBB
static uint64_t g_obb_refills;  // actual SD reads we issued (read-ahead misses)
static uint64_t g_obb_bytes;    // bytes served to the game from the OBB
static uint64_t g_obb_sd_bytes; // bytes actually read from the SD (over-read = this - served)
static uint64_t g_fopen_calls;  // total fopen() attempts
static uint64_t g_fopen_fail;   // fopen() attempts that missed (loose-file probes)
#if DEBUG_LOG
static uint64_t g_next_bytes;   // next g_obb_bytes milestone to log at
static uint64_t g_next_fopen;   // next g_fopen_calls milestone to log at
#endif

// total bytes served from the OBB so far; the main loop samples this to detect
// "loading" (heavy OBB reads) and boost the CPU during those windows.
uint64_t obb_bytes_total(void) { return g_obb_bytes; }

static void io_maybe_log(void) {
#if DEBUG_LOG
  if (g_obb_bytes >= g_next_bytes || g_fopen_calls >= g_next_fopen) {
    g_next_bytes = g_obb_bytes + 16 * 1024 * 1024;
    g_next_fopen = g_fopen_calls + 4000;
    debugPrintf("[io] obb: opens=%llu reads=%llu refills=%llu served=%lluMB sd=%lluMB | fopen: %llu (%llu miss)\n",
                (unsigned long long)g_obb_opens, (unsigned long long)g_obb_reads,
                (unsigned long long)g_obb_refills, (unsigned long long)(g_obb_bytes >> 20),
                (unsigned long long)(g_obb_sd_bytes >> 20),
                (unsigned long long)g_fopen_calls, (unsigned long long)g_fopen_fail);
  }
#endif
}

static void obb_io_init(void) {
  if (!s_obb_inited) {
    mutexInit(&s_obb_table_lock);
    for (int i = 0; i < OBB_NFD; i++) s_obb[i].fd = -1;
    s_obb_inited = 1;
  }
}

static ObbFd *obb_find(int fd) {
  if (!s_obb_inited) return NULL;
  for (int i = 0; i < OBB_NFD; i++) if (s_obb[i].fd == fd) return &s_obb[i];
  return NULL;
}

static int path_is_obb(const char *p) {
  const char *s = strrchr(p, '/');
  s = s ? s + 1 : p;
  return strcmp(s, "main.obb") == 0;
}

static void obb_track(int fd) {
  obb_io_init();
  mutexLock(&s_obb_table_lock);
  for (int i = 0; i < OBB_NFD; i++) {
    if (s_obb[i].fd < 0) {
      if (!s_obb[i].buf) { s_obb[i].buf = memalign(0x1000, OBB_BUF_SZ); mutexInit(&s_obb[i].lock); }
      s_obb[i].pos = 0; s_obb[i].buf_start = 0; s_obb[i].buf_len = 0;
      s_obb[i].ra = OBB_RA_MIN; s_obb[i].seq_next = -1;
      struct stat st;
      s_obb[i].size = (fstat(fd, &st) == 0) ? st.st_size : 0;
      if (s_obb[i].buf) { s_obb[i].fd = fd; g_obb_opens++; } // publish last
      break;
    }
  }
  mutexUnlock(&s_obb_table_lock);
}

// getrandom / getentropy.
//
// This payload imports neither -- it has zero unresolved imports -- so these
// are not the current failure. They are here because whether a NativeAOT build
// reaches for them depends on what the NDK's headers advertised at ILC time
// (HAVE_GETRANDOM / HAVE_GETENTROPY), which changes with the toolchain and not
// with anything in this repo. A future payload can pick either without warning,
// and an unresolved RNG import is tainted into a trap that faults rather than
// failing cleanly.
ssize_t sm127_getrandom(void *buf, size_t len, unsigned flags) {
  (void)flags;
  if (!buf) { errno = EFAULT; return -1; }
  if (len) randomGet(buf, len);
  return (ssize_t)len;
}

int sm127_getentropy(void *buf, size_t len) {
  // POSIX caps a single call at 256 bytes and callers loop; enforcing it keeps
  // a caller that checks honest rather than silently over-serving.
  if (!buf || len > 256) { errno = EIO; return -1; }
  if (len) randomGet(buf, len);
  return 0;
}

ssize_t read_fake(int fd, void *dst, size_t count) {
  if (fd == URANDOM_FD) {
    if (dst && count) randomGet(dst, count);
    static int logged;
    if (logged < 4) {
      logged++;
      debugPrintf("[rng] read(URANDOM_FD, %zu) -> %zu\n", count, count);
    }
    return (ssize_t)count;
  }
  ObbFd *o = obb_find(fd);
  if (!o) return read(fd, dst, count);
  mutexLock(&o->lock);
  uint8_t *out = dst;
  size_t done = 0;
  while (done < count && o->pos < o->size) {
    if (o->pos < o->buf_start || o->pos >= o->buf_start + (off_t)o->buf_len) {
      // adaptive window: grow while the game reads sequentially, shrink on a
      // seek, so scattered tiny reads don't over-read hundreds of MB.
      if (o->pos == o->seq_next) { o->ra <<= 1; if (o->ra > OBB_RA_MAX) o->ra = OBB_RA_MAX; }
      else                        { o->ra = OBB_RA_MIN; }
      off_t start = o->pos & ~(off_t)0xFFF;         // page-align the backing read
      size_t rsize = o->ra + (size_t)(o->pos - start); // cover the alignment slack
      if (rsize > OBB_BUF_SZ) rsize = OBB_BUF_SZ;
      if (lseek(fd, start, SEEK_SET) != start) break;
      ssize_t got = read(fd, o->buf, rsize);
      if (got <= 0) break;
      o->buf_start = start; o->buf_len = (size_t)got;
      o->seq_next = start + (off_t)got;             // next sequential read lands here
      g_obb_refills++; g_obb_sd_bytes += (uint64_t)got;
    }
    size_t off = (size_t)(o->pos - o->buf_start);
    size_t avail = o->buf_len - off;
    size_t n = count - done;
    if (n > avail) n = avail;
    memcpy(out + done, o->buf + off, n);
    done += n; o->pos += n;
  }
  g_obb_reads++; g_obb_bytes += done;
  io_maybe_log();
  mutexUnlock(&o->lock);
  return (ssize_t)done;
}

off_t lseek_fake(int fd, off_t off, int whence) {
  // /dev/urandom is a character device: seeking is meaningless but must not
  // fail, because a caller that treats -1 as "this fd is broken" will discard
  // it. Reporting position 0 is what a real character device does.
  if (fd == URANDOM_FD) return 0;
  ObbFd *o = obb_find(fd);
  if (!o) return lseek(fd, off, whence);
  mutexLock(&o->lock);
  off_t np = (whence == SEEK_SET) ? off
           : (whence == SEEK_CUR) ? o->pos + off
                                  : o->size + off; // SEEK_END
  if (np < 0) np = 0;
  o->pos = np;
  mutexUnlock(&o->lock);
  return np;
}

int close_fake(int fd) {
  if (fd == URANDOM_FD) return 0;
  ObbFd *o = obb_find(fd);
  if (o) { mutexLock(&s_obb_table_lock); o->fd = -1; mutexUnlock(&s_obb_table_lock); }
  sm127_net_forget_fd(fd);   // before close(): the number is reusable after it
  return close(fd);
}

// readlink("/proc/self/exe" and friends): hand PhysFS a plausible absolute
// executable path so calcBaseDir yields "<data_root>/" as the base dir.
ssize_t readlink_fake(const char *path, char *buf, size_t bufsz) {
  if (path && strncmp(path, "/proc", 5) == 0) {
    int n = snprintf(buf, bufsz, "%s/sts2.nro", config.data_root);
    if (n < 0) return -1;
    if ((size_t)n > bufsz) n = (int)bufsz; // readlink returns the truncated count
    return n;
  }
  // Rebased like every other path call. This was passing the caller's path
  // straight to newlib.
  char sb[640];
  return readlink(sandbox_path(path, sb, sizeof(sb)), buf, bufsz);
}

// Some data lives in a region/language subfolder (pspbin/eu/, pack/jp/,
// sound/xa/en/) the engine sometimes omits; retry with each inserted.
static FILE *fopen_region_fallback(const char *path, const char *mode) {
  const char *slash = strrchr(path, '/');
  if (!slash || slash == path) return NULL;
  static const char *regions[] = { "eu", "us", "jp", "en" };
  char cand[640];
  const int dirlen = (int)(slash - path);
  for (unsigned i = 0; i < sizeof(regions) / sizeof(*regions); i++) {
    snprintf(cand, sizeof(cand), "%.*s/%s/%s", dirlen, path, regions[i], slash + 1);
    FILE *f = fopen(cand, mode);
    if (f) return f;
  }
  return NULL;
}

// large stream buffer: the engine issues many small reads/seeks against the
// game archives and fsdev round trips dominate otherwise.
// Negative directory cache: the game probes each asset in ~6 override
// directories, 5 of which don't exist on the SD (their content lives in the
// OBB). A failed fopen into a missing directory still costs an SD directory
// lookup; remember which directories exist and short-circuit the rest.
#define DIRCACHE_N 64
static struct { char dir[256]; int exists; } s_dircache[DIRCACHE_N];
static int s_dircache_n;
static Mutex s_dircache_lock; // zero-initialized == unlocked (libnx)

static int dir_exists_cached(const char *path) {
  const char *slash = strrchr(path, '/');
  if (!slash || slash == path) return 1; // no dir / root -> treat as existing
  size_t len = (size_t)(slash - path);
  if (len >= 256) return 1;
  char dir[256];
  memcpy(dir, path, len);
  dir[len] = 0;

  mutexLock(&s_dircache_lock);
  for (int i = 0; i < s_dircache_n; i++)
    if (!strcmp(s_dircache[i].dir, dir)) { int e = s_dircache[i].exists; mutexUnlock(&s_dircache_lock); return e; }
  struct stat st;
  int e = (stat(dir, &st) == 0 && S_ISDIR(st.st_mode));
  if (s_dircache_n < DIRCACHE_N) {
    strcpy(s_dircache[s_dircache_n].dir, dir);
    s_dircache[s_dircache_n].exists = e;
    s_dircache_n++;
  }
  mutexUnlock(&s_dircache_lock);
  return e;
}

FILE *fopen_fake(const char *path, const char *mode) {
  if (!path) return NULL;
  if (is_urandom_path(path))
    return (FILE *)&urandom_marker;
  char sb[640];
  path = sandbox_path(path, sb, sizeof(sb));
  // read probes into a non-existent directory can't succeed -> skip the SD hit
  if (strchr(mode, 'r') && !dir_exists_cached(path)) {
    g_fopen_calls++; g_fopen_fail++; io_maybe_log();
    return NULL;
  }
  FILE *f = fopen(path, mode);
  if (!f && strchr(mode, 'r')) {
    f = fopen_region_fallback(path, mode);
    if (!f) {
      char cand[640];
      if (obb_fallback_path(path, cand, sizeof(cand)))
        f = fopen(cand, mode);
    }
  }
  if (f && strchr(mode, 'r'))
    setvbuf(f, NULL, _IOFBF, 64 * 1024);
  g_fopen_calls++;
  if (!f) g_fopen_fail++;
  io_maybe_log();
#if VERBOSE_IO
  debugPrintf("fopen(\"%s\", \"%s\") -> %p\n", path, mode, (void *)f);
#endif
  return f;
}

// ANativeWindow -> NWindow: hand SDL the real Switch window so mesa's
// eglCreateWindowSurface lands on it.
void *ANativeWindow_fromSurface_fake(void *env, void *surface) {
  (void)env; (void)surface;
  NWindow *win = nwindowGetDefault();
  nwindowSetDimensions(win, screen_width, screen_height);
  return win;
}
void ANativeWindow_release_fake(void *win) { (void)win; }
int ANativeWindow_setBuffersGeometry_fake(void *win, int w, int h, int format) {
  (void)format;
  if (w > 0 && h > 0) nwindowSetDimensions((NWindow *)win, w, h);
  return 0;
}

// POSIX semaphores via pointer indirection (bionic sem_t is 16 bytes on LP64,
// so a heap FakeSem* fits in the caller's storage)
typedef struct { Semaphore sem; } FakeSem;

int sem_init_fake(void **s, int pshared, unsigned int value) {
  (void)pshared;
  FakeSem *fs = calloc(1, sizeof(*fs));
  if (!fs) return -1;
  semaphoreInit(&fs->sem, value);
  *s = fs;
  return 0;
}
int sem_destroy_fake(void **s) {
  if (s && *s) { free(*s); *s = NULL; }
  return 0;
}
int sem_post_fake(void **s) {
  if (s && *s) semaphoreSignal(&((FakeSem *)*s)->sem);
  return 0;
}
int sem_wait_fake(void **s) {
  if (s && *s) semaphoreWait(&((FakeSem *)*s)->sem);
  return 0;
}
int sem_trywait_fake(void **s) {
  if (s && *s && semaphoreTryWait(&((FakeSem *)*s)->sem)) return 0;
  errno = EAGAIN;
  return -1;
}
int sem_getvalue_fake(void **s, int *val) {
  *val = (s && *s) ? (int)((FakeSem *)*s)->sem.count : 0;
  return 0;
}
