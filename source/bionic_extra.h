/* bionic_extra.h -- bionic/POSIX odds and ends the import table needs that
 * neither newlib nor the other shims provide.
 *
 * Salvaged from sts2_nx's nativeaot_shim.c and dotnet_imports.c with the
 * NativeAOT diagnostics removed: SM127 is GDScript, so there is no .NET
 * runtime here, but libgodot_android.so and libc++_shared.so still import a
 * slice of this surface (fortify checks, condattr, clock_nanosleep, ...), and
 * an import left unresolved is tainted by so_resolve and traps when called.
 *
 * MIT license; see LICENSE. */

#ifndef __BIONIC_EXTRA_H__
#define __BIONIC_EXTRA_H__

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <sys/types.h>
#include <pthread.h>

// Referenced by imports.c's pthread tracing; 0 = quiet.
extern int sm127_trace_all;

// process termination -- flush the log first so the reason is not lost
__attribute__((noreturn)) void sm127_abort_hook(void);
__attribute__((noreturn)) void sm127_exit_hook(int code);
__attribute__((noreturn)) void sm127_exit_now(int status);

// write() that mirrors fd 1/2 into the debug log
ssize_t sm127_write_hook(int fd, const void *buf, size_t count);
ssize_t sm127_writev(int fd, const void *iov, int iovcnt);

// fortify
void *sm127_memcpy_chk(void *dst, const void *src, size_t len, size_t dstlen);
void *sm127_memset_chk(void *dst, int c, size_t len, size_t dstlen);
char *sm127_strncpy_chk2(char *dst, const char *src, size_t n, size_t dstlen, size_t srclen);
char *sm127_strcpy_chk(char *dest, const char *src, size_t destlen);
char *sm127_strcat_chk(char *dest, const char *src, size_t destlen);

// bionic pthread_condattr_t is a plain int (see bionic_extra.c)
int sm127_condattr_init(pthread_condattr_t *attr);
int sm127_condattr_destroy(pthread_condattr_t *attr);
int sm127_condattr_setclock(pthread_condattr_t *attr, clockid_t clock_id);

// newlib has no pthread_getattr_np; imports.c's pthread_getattr_np_fake calls
// this and reports EINVAL to the engine when it fails.
int pthread_getattr_np(pthread_t thread, pthread_attr_t *attr);

int    sm127_libc_current_sigrtmin(void);
int    sm127_libc_current_sigrtmax(void);
int    sm127_sched_cpucount(size_t setsize, const void *set);
int    sm127_asprintf(char **strp, const char *fmt, ...);
// imports.c: nanosleep that counts frame-thread sleep time for the pacing log.
int    sm127_nanosleep_timed(const struct timespec *req, struct timespec *rem);
int    sm127_clock_nanosleep(clockid_t clock_id, int flags,
                            const struct timespec *req, struct timespec *rem);
int    sm127_dladdr(const void *addr, void *info);
int    sm127_flock(int fd, int operation);
int    sm127_prctl(int option, ...);
int    sm127_pipe2(int pipefd[2], int flags);
int    sm127_getpwuid_r(unsigned uid, void *pwd, char *buf, size_t buflen, void **result);
int    sm127_getrusage(int who, void *usage);
int    sm127_sysinfo(void *info);
ssize_t sm127_getline(char **lineptr, size_t *n, FILE *stream);
void   sm127_arc4random_buf(void *buf, size_t n);
int    sm127_mprotect(void *addr, size_t len, int prot);
int    sm127_getpagesize(void);
time_t sm127_timegm(struct tm *tm);
int    sm127_uname(void *buf);
int    sm127_msync(void *addr, size_t len, int flags);
int    sm127_futimens(int fd, const void *times);
int    sm127_sigfillset(void *set);
int    sm127_getgroups(int n, unsigned *list);
int    sm127_getpwnam_r(const char *name, void *pwd, char *buf, size_t buflen, void **result);
int    sm127_enosys(void);
long   sm127_xattr_unsupported(void);

#endif
