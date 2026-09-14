/* bionic_extra.c -- see bionic_extra.h.
 *
 * Every function here was hardware-tested in sts2_nx (where most of them
 * served the NativeAOT payload); what changed in the move is only that the
 * logging hooks around them are gone. The comments that remain are the ones
 * that explain a non-obvious ABI decision.
 *
 * MIT license; see LICENSE. */

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "error.h"
#include "bionic_extra.h"

int sm127_trace_all = 0;

// ---------------------------------------------------------------------------
// termination
// ---------------------------------------------------------------------------

__attribute__((noreturn)) void sm127_abort_hook(void) {
  debugPrintf("[abort] engine called abort() from %p\n", __builtin_return_address(0));
  debugFlush();
  abort();
}

__attribute__((noreturn)) void sm127_exit_hook(int code) {
  debugPrintf("[exit] engine called exit(%d)\n", code);
  debugFlush();
  exit(code);
}

// _exit is only reached from abort paths; keep the message on screen instead
// of the process vanishing.
__attribute__((noreturn)) void sm127_exit_now(int status) {
  fatal_error("The engine called _exit(%d).", status);
}

// fd 1/2 only reach nxlink, which is usually not listening. Mirror them into
// the log file so anything printed right before a failure is kept.
ssize_t sm127_write_hook(int fd, const void *buf, size_t count) {
  if ((fd == 1 || fd == 2) && buf && count)
    debugPrintf("[fd%d] %.*s\n", fd, (int)count, (const char *)buf);
  return write(fd, buf, count);
}

// writev: Rust's std (Mesa's NAK shader compiler) prints through it. bionic's struct iovec is
// { void *base; size_t len; }, the same layout as newlib's. Gathered into one
// write per vector so fd 1/2 still reach the log via sm127_write_hook.
ssize_t sm127_writev(int fd, const void *iov, int iovcnt) {
  const struct { const void *base; size_t len; } *v = iov;
  ssize_t total = 0;
  for (int i = 0; i < iovcnt; i++) {
    if (!v[i].len) continue;
    ssize_t n = sm127_write_hook(fd, v[i].base, v[i].len);
    if (n < 0) return total ? total : -1;
    total += n;
    if ((size_t)n < v[i].len) break;
  }
  return total;
}

// ---------------------------------------------------------------------------
// fortify. Clang emits these whenever it can compute a destination size, so
// they are on hot paths, not edge cases. Checking and failing loudly beats a
// silent overflow that corrupts the heap and crashes somewhere unrelated.
// ---------------------------------------------------------------------------

void *sm127_memcpy_chk(void *dst, const void *src, size_t len, size_t dstlen) {
  if (len > dstlen) fatal_error("__memcpy_chk overflow\n%zu into %zu", len, dstlen);
  return memcpy(dst, src, len);
}

void *sm127_memset_chk(void *dst, int c, size_t len, size_t dstlen) {
  if (len > dstlen) fatal_error("__memset_chk overflow\n%zu into %zu", len, dstlen);
  return memset(dst, c, len);
}

char *sm127_strncpy_chk2(char *dst, const char *src, size_t n, size_t dstlen, size_t srclen) {
  (void)srclen;
  if (n > dstlen) fatal_error("__strncpy_chk2 overflow\n%zu into %zu", n, dstlen);
  return strncpy(dst, src, n);
}

// Named sm127_*, not __strcpy_chk: GCC treats that name as a builtin and will
// not let code take its address. The import table exports it under the real
// name.
char *sm127_strcpy_chk(char *dest, const char *src, size_t destlen) {
  (void)destlen;
  return strcpy(dest, src);
}

char *sm127_strcat_chk(char *dest, const char *src, size_t destlen) {
  (void)destlen;
  return strcat(dest, src);
}

// ---------------------------------------------------------------------------
// pthread odds and ends
// ---------------------------------------------------------------------------

// bionic's pthread_condattr_t is a plain 32-bit int, not newlib's struct.
// imports.c's pthread_cond_init_fake reads it back as that int to honour the
// requested clock. Named sm127_* so they do not shadow libsysbase's real
// pthread_condattr_*, which make_cond() in imports.c needs.
int sm127_condattr_init(pthread_condattr_t *attr) { if (attr) *(int *)attr = 0; return 0; }
int sm127_condattr_destroy(pthread_condattr_t *attr) { (void)attr; return 0; }
int sm127_condattr_setclock(pthread_condattr_t *attr, clockid_t clock_id) {
  if (attr) *(int *)attr = (int)clock_id;
  return 0;
}

// Threads here are libnx threads behind newlib's pthread layer; there is no
// real attr to hand back. The caller (pthread_getattr_np_fake) converts this
// into a well-defined EINVAL with zeroed outputs.
int pthread_getattr_np(pthread_t thread, pthread_attr_t *attr) {
  (void)thread; (void)attr;
  errno = ENOSYS;
  return ENOSYS;
}

int sm127_libc_current_sigrtmin(void) { return 32; }
int sm127_libc_current_sigrtmax(void) { return 64; }

int sm127_sched_cpucount(size_t setsize, const void *set) {
  (void)setsize; (void)set;
  return 3;   // matches sched_getaffinity_fake: three cores for applications
}

// ---------------------------------------------------------------------------
// misc libc
// ---------------------------------------------------------------------------

int sm127_asprintf(char **strp, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int needed = vsnprintf(NULL, 0, fmt, args);
  va_end(args);
  if (needed < 0) { *strp = NULL; return -1; }
  *strp = malloc((size_t)needed + 1);
  if (!*strp) return -1;
  va_start(args, fmt);
  int written = vsnprintf(*strp, (size_t)needed + 1, fmt, args);
  va_end(args);
  return written;
}

// bionic's TIMER_ABSTIME is 1; newlib's is 4. The caller's numbering wins.
// An absolute deadline passed to a relative nanosleep sleeps for "seconds
// since boot", i.e. forever -- so convert.
#define BIONIC_TIMER_ABSTIME 1
int sm127_clock_nanosleep(clockid_t clock_id, int flags,
                         const struct timespec *req, struct timespec *rem) {
  if (!(flags & BIONIC_TIMER_ABSTIME)) return sm127_nanosleep_timed(req, rem);
  struct timespec now;
  if (clock_gettime(clock_id, &now) != 0) return -1;
  struct timespec rel;
  rel.tv_sec = req->tv_sec - now.tv_sec;
  rel.tv_nsec = req->tv_nsec - now.tv_nsec;
  if (rel.tv_nsec < 0) { rel.tv_nsec += 1000000000L; rel.tv_sec -= 1; }
  if (rel.tv_sec < 0) return 0;   // deadline already passed
  return sm127_nanosleep_timed(&rel, NULL);
}

int sm127_dladdr(const void *addr, void *info) { (void)addr; (void)info; return 0; }
int sm127_flock(int fd, int operation) { (void)fd; (void)operation; return 0; }
int sm127_prctl(int option, ...) { (void)option; return 0; }
int sm127_pipe2(int pipefd[2], int flags) { (void)pipefd; (void)flags; errno = ENOSYS; return -1; }
int sm127_enosys(void) { errno = ENOSYS; return -1; }

int sm127_getpwuid_r(unsigned uid, void *pwd, char *buf, size_t buflen, void **result) {
  (void)uid; (void)pwd; (void)buf; (void)buflen;
  if (result) *result = NULL;
  return ENOENT;
}

int sm127_getpwnam_r(const char *name, void *pwd, char *buf, size_t buflen, void **result) {
  (void)name; (void)pwd; (void)buf; (void)buflen;
  if (result) *result = NULL;   // "no such user" is the well-defined answer
  return 0;
}

int sm127_getgroups(int n, unsigned *list) {
  if (n == 0) return 1;
  if (n < 1 || !list) { errno = EINVAL; return -1; }
  list[0] = 0;
  return 1;
}

int sm127_getrusage(int who, void *usage) {
  (void)who;
  if (usage) memset(usage, 0, 144);   // aarch64 struct rusage
  return 0;
}

int sm127_sysinfo(void *info) {
  if (!info) return 0;
  memset(info, 0, 112);               // aarch64 struct sysinfo
  unsigned long *p = (unsigned long *)info;
  p[4] = 3200ul << 20;                // totalram
  p[5] = 2048ul << 20;                // freeram
  *(unsigned int *)((char *)info + 104) = 1;   // mem_unit
  return 0;
}

ssize_t sm127_getline(char **lineptr, size_t *n, FILE *stream) {
  extern ssize_t __getline(char **, size_t *, FILE *);  // newlib's, under its own name
  return __getline(lineptr, n, stream);
}

// newlib's arc4random_buf ends in _getentropy_r, which libnx does not provide;
// go straight to Horizon's CSRNG.
void sm127_arc4random_buf(void *buf, size_t n) { randomGet(buf, n); }

// Heap pages are already RW. Try to honour a real change; if the kernel
// refuses (NRO pages are not Heap-typed) but the page already has what was
// asked for, that is success.
int sm127_mprotect(void *addr, size_t len, int prot) {
  uintptr_t page = (uintptr_t)addr & ~0xFFFULL;
  size_t plen = (((uintptr_t)addr + len) - page + 0xFFF) & ~0xFFFULL;
  u32 perm = (prot & 1 ? Perm_R : 0) | (prot & 2 ? Perm_W : 0);
  if (R_SUCCEEDED(svcSetMemoryPermission((void *)page, plen, perm))) return 0;
  MemoryInfo mi; u32 pi;
  if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, page)) && (mi.perm & perm) == perm) return 0;
  errno = EACCES;
  return -1;
}

int sm127_getpagesize(void) { return 0x1000; }

// UTC counterpart of mktime (days-from-civil); no timezone, no DST.
time_t sm127_timegm(struct tm *tm) {
  if (!tm) return (time_t)-1;
  int y = tm->tm_year + 1900;
  int m = tm->tm_mon + 1;
  y -= (m <= 2);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + tm->tm_mday - 1);
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const int64_t days = (int64_t)era * 146097 + (int64_t)doe - 719468;
  return (time_t)(days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec);
}

int sm127_uname(void *buf) {
  struct { char sysname[65], nodename[65], release[65], version[65], machine[65], domain[65]; } *u = buf;
  if (!u) { errno = EFAULT; return -1; }
  memset(u, 0, sizeof(*u));
  strncpy(u->sysname,  "Horizon", sizeof(u->sysname) - 1);
  strncpy(u->nodename, "switch",  sizeof(u->nodename) - 1);
  strncpy(u->release,  "1.0",     sizeof(u->release) - 1);
  strncpy(u->version,  "sm127_nx", sizeof(u->version) - 1);
  strncpy(u->machine,  "aarch64", sizeof(u->machine) - 1);
  return 0;
}

int sm127_msync(void *addr, size_t len, int flags) { (void)addr; (void)len; (void)flags; return 0; }
int sm127_futimens(int fd, const void *times) { (void)fd; (void)times; return 0; }

// FILLS -- the name is the contract. bionic's LP64 sigset_t is one 64-bit word.
// xattr family. errno is written in BIONIC numbering (ENOTSUP == 95), since
// the engine is what reads it.
long sm127_xattr_unsupported(void) {
  errno = 95;
  return -1;
}

int sm127_sigfillset(void *set) {
  if (set) *(uint64_t *)set = ~0ull;
  return 0;
}
