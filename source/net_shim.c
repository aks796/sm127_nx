/* net_shim.c -- the engine's BSD sockets, over libnx's socket stack.
 *
 * Godot 3 does HTTP and HTTPS itself: HTTPRequest/HTTPClient on top of plain
 * BSD sockets (drivers/unix/net_socket_posix.cpp, ip_unix.cpp) and its own
 * mbedtls, with a CA bundle compiled into libgodot_android.so. None of that
 * needs Android. What it lacked here was the sockets: every one of these calls
 * used to be a stub that failed, so each request died at name resolution and
 * SM127 reported "Failed to connect to Level Share Square. Response code: 0" --
 * the level browser, login, ratings, comments and thumbnails alike.
 *
 * The engine was compiled against bionic, so it speaks Linux's socket ABI.
 * libnx's socket layer is FreeBSD's. They agree on the calls and disagree on
 * the bytes, and every disagreement is translated here:
 *
 *   sockaddr   Linux starts with a u16 family; BSD with a u8 length and a u8
 *              family. Everything after those two bytes lines up.
 *   numbers    AF_INET6, SOL_SOCKET and every SO_* option, the IP_/IPV6_/TCP_
 *              options, MSG_* flags, SOCK_NONBLOCK, FIONREAD/FIONBIO, POLL*.
 *   errno      libnx reports newlib's numbers; the engine compares against
 *              bionic's (bionic_errno.c).
 *   addrinfo   Same layout -- bionic kept BSD's field order -- but ai_family
 *              and ai_addr are BSD-shaped, so each result is rebuilt as a list
 *              the engine can walk, and our freeaddrinfo frees it.
 *
 * The engine drives sockets non-blocking with ioctl(FIONBIO) -- Android builds
 * of Godot define NO_FCNTL -- and reads the pending byte count with
 * ioctl(FIONREAD), which is how it notices the server closed the connection.
 * Both used to return 0 without touching anything.
 *
 * Host constants are only ever named, never written as numbers, so this file
 * also builds on macOS (BSD-shaped as well) for the host test.
 *
 * MIT license; see LICENSE. */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

#ifdef __SWITCH__
#include <switch.h>
#include "config.h"
#include "util.h"
#if DEBUG_LOG
#define netLog(...) debugPrintf("[net] " __VA_ARGS__)
#else
#define netLog(...) do {} while (0)
#endif
static uint64_t now_ms(void) { return armTicksToNs(armGetSystemTick()) / 1000000ull; }
#else
#include <time.h>
#define netLog(...) fprintf(stderr, "[net] " __VA_ARGS__)
static uint64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
#endif

#include "bionic_errno.h"
#include "net_shim.h"

// BSD sockaddrs carry their own length; Linux's do not.
#if defined(__SWITCH__) || defined(__APPLE__) || defined(__FreeBSD__)
#define HAVE_SA_LEN 1
#else
#define HAVE_SA_LEN 0
#endif

// ---------------------------------------------------------------------------
// bionic's numbers (Linux asm-generic headers, arm64)
// ---------------------------------------------------------------------------

#define B_AF_UNSPEC        0
#define B_AF_INET          2
#define B_AF_INET6         10
#define B_SIN_LEN          16
#define B_SIN6_LEN         28

#define B_SOCK_TYPE_MASK   0xf
#define B_SOCK_NONBLOCK    00004000
#define B_SOCK_CLOEXEC     02000000

#define B_SOL_SOCKET       1
#define B_SO_DEBUG         1
#define B_SO_REUSEADDR     2
#define B_SO_TYPE          3
#define B_SO_ERROR         4
#define B_SO_DONTROUTE     5
#define B_SO_BROADCAST     6
#define B_SO_SNDBUF        7
#define B_SO_RCVBUF        8
#define B_SO_KEEPALIVE     9
#define B_SO_OOBINLINE     10
#define B_SO_LINGER        13
#define B_SO_REUSEPORT     15
#define B_SO_RCVLOWAT      18
#define B_SO_SNDLOWAT      19
#define B_SO_RCVTIMEO      20
#define B_SO_SNDTIMEO      21

#define B_IPPROTO_IP       0
#define B_IPPROTO_TCP      6
#define B_IPPROTO_UDP      17
#define B_IPPROTO_IPV6     41
#define B_IP_TOS           1
#define B_IP_TTL           2
#define B_IP_MULTICAST_IF  32
#define B_IP_MULTICAST_TTL 33
#define B_IP_MULTICAST_LOOP 34
#define B_IP_ADD_MEMBERSHIP 35
#define B_IP_DROP_MEMBERSHIP 36
#define B_IPV6_UNICAST_HOPS 16
#define B_IPV6_MULTICAST_IF 17
#define B_IPV6_MULTICAST_HOPS 18
#define B_IPV6_MULTICAST_LOOP 19
#define B_IPV6_JOIN_GROUP  20
#define B_IPV6_LEAVE_GROUP 21
#define B_IPV6_V6ONLY      26
#define B_TCP_NODELAY      1
#define B_TCP_KEEPIDLE     4
#define B_TCP_KEEPINTVL    5
#define B_TCP_KEEPCNT      6

#define B_MSG_OOB          0x1
#define B_MSG_PEEK         0x2
#define B_MSG_DONTROUTE    0x4
#define B_MSG_TRUNC        0x20
#define B_MSG_DONTWAIT     0x40
#define B_MSG_EOR          0x80
#define B_MSG_WAITALL      0x100
#define B_MSG_NOSIGNAL     0x4000

#define B_FIONREAD         0x541B
#define B_FIONBIO          0x5421
#define B_FIONCLEX         0x5450
#define B_FIOCLEX          0x5451

#define B_POLLIN           0x001
#define B_POLLPRI          0x002
#define B_POLLOUT          0x004
#define B_POLLERR          0x008
#define B_POLLHUP          0x010
#define B_POLLNVAL         0x020
#define B_POLLRDNORM       0x040
#define B_POLLRDBAND       0x080
#define B_POLLWRNORM       0x100
#define B_POLLWRBAND       0x200

#define B_AI_PASSIVE       0x1
#define B_AI_CANONNAME     0x2
#define B_AI_NUMERICHOST   0x4
#define B_AI_NUMERICSERV   0x8

#define B_EAI_ADDRFAMILY   1
#define B_EAI_AGAIN        2
#define B_EAI_BADFLAGS     3
#define B_EAI_FAIL         4
#define B_EAI_FAMILY       5
#define B_EAI_MEMORY       6
#define B_EAI_NODATA       7
#define B_EAI_NONAME       8
#define B_EAI_SERVICE      9
#define B_EAI_SOCKTYPE     10
#define B_EAI_SYSTEM       11
#define B_EAI_BADHINTS     12
#define B_EAI_PROTOCOL     13
#define B_EAI_OVERFLOW     14

// bionic's struct addrinfo. BSD field order: ai_canonname before ai_addr.
typedef struct BAddrinfo {
  int ai_flags;
  int ai_family;
  int ai_socktype;
  int ai_protocol;
  uint32_t ai_addrlen;
  char *ai_canonname;
  void *ai_addr;
  struct BAddrinfo *ai_next;
} BAddrinfo;
_Static_assert(sizeof(BAddrinfo) == 48, "bionic addrinfo is 48 bytes on arm64");

typedef struct {
  int fd;
  short events;
  short revents;
} BPollfd;
_Static_assert(sizeof(BPollfd) == sizeof(struct pollfd), "pollfd layouts match");

// ---------------------------------------------------------------------------
// which descriptors are sockets, and what we know about each
// ---------------------------------------------------------------------------

// ioctl() and poll() reach here for every descriptor, and before this file
// both returned 0 for all of them. Only sockets are handed to libnx; anything
// else keeps that behaviour.
#define FD_TRACK 4096
static uint64_t s_sock_bits[FD_TRACK / 64];

static int is_socket(int fd) {
  if (fd < 0 || fd >= FD_TRACK) return 0;
  return (s_sock_bits[fd >> 6] >> (fd & 63)) & 1;
}

static void mark_socket(int fd, int on) {
  if (fd < 0 || fd >= FD_TRACK) return;
  const uint64_t bit = 1ull << (fd & 63);
  if (on) __atomic_or_fetch(&s_sock_bits[fd >> 6], bit, __ATOMIC_SEQ_CST);
  else    __atomic_and_fetch(&s_sock_bits[fd >> 6], ~bit, __ATOMIC_SEQ_CST);
}

// For the log only: when a connect started and where to.
#define NOTE_TRACK 1024
typedef struct {
  uint64_t t0;
  char     peer[48];
  uint8_t  state;          // 0 idle, 1 connecting, 2 connected
} FdNote;
static FdNote s_notes[NOTE_TRACK];

static FdNote *note_for(int fd) {
  return (fd >= 0 && fd < NOTE_TRACK) ? &s_notes[fd] : NULL;
}

// Unexpected errors are logged, within a budget: a socket polled every frame
// must not be able to fill the log.
static int s_err_budget = 60;

static int take_budget(int *budget) {
  return __atomic_sub_fetch(budget, 1, __ATOMIC_SEQ_CST) >= 0;
}

// Not worth a line: these are how non-blocking sockets report "not yet".
static int routine_errno(int e) {
  return e == EAGAIN || e == EWOULDBLOCK || e == EINPROGRESS || e == EALREADY ||
         e == EISCONN || e == EINTR;
}

// Fail with errno translated for the engine. Logging happens first, because
// debugPrintf may itself touch errno.
static int fail_errno(int host_errno, const char *what, int fd) {
  if (!routine_errno(host_errno) && take_budget(&s_err_budget))
    netLog("%s(fd %d) failed: %s (errno %d)\n", what, fd, strerror(host_errno), host_errno);
  errno = sm127_errno_to_bionic(host_errno);
  return -1;
}

static int fail(const char *what, int fd) { return fail_errno(errno, what, fd); }

// ---------------------------------------------------------------------------
// sockaddr
// ---------------------------------------------------------------------------

static void describe_bionic_addr(const uint8_t *b, uint32_t len, char *out, size_t n) {
  uint16_t fam;
  memcpy(&fam, b, sizeof(fam));
  if (fam == B_AF_INET && len >= B_SIN_LEN)
    snprintf(out, n, "%u.%u.%u.%u:%u", b[4], b[5], b[6], b[7], (unsigned)((b[2] << 8) | b[3]));
  else if (fam == B_AF_INET6 && len >= B_SIN6_LEN)
    snprintf(out, n, "[ipv6]:%u", (unsigned)((b[2] << 8) | b[3]));
  else
    snprintf(out, n, "(family %u)", fam);
}

// bionic sockaddr -> host. Returns 0, or -1 with errno already translated.
static int addr_to_host(const void *src, uint32_t len,
                        struct sockaddr_storage *dst, socklen_t *dst_len) {
  uint16_t fam;
  if (!src || len < sizeof(fam)) return fail_errno(EINVAL, "sockaddr", -1);
  memcpy(&fam, src, sizeof(fam));
  memset(dst, 0, sizeof(*dst));

  int host_family;
  uint32_t size;
  if (fam == B_AF_INET)       { host_family = AF_INET;  size = B_SIN_LEN; }
  else if (fam == B_AF_INET6) { host_family = AF_INET6; size = B_SIN6_LEN; }
  else return fail_errno(EAFNOSUPPORT, "sockaddr", -1);
  if (len < size) return fail_errno(EINVAL, "sockaddr", -1);

  // Port, address, flow info and scope id sit at the same offsets in both.
  uint8_t *d = (uint8_t *)dst;
  memcpy(d, src, size);
#if HAVE_SA_LEN
  d[0] = (uint8_t)size;
  d[1] = (uint8_t)host_family;
#else
  uint16_t hf = (uint16_t)host_family;
  memcpy(d, &hf, sizeof(hf));
#endif
  *dst_len = (socklen_t)size;
  return 0;
}

// host sockaddr -> bionic, truncated to the caller's buffer as the kernel does;
// *len always receives the full size.
static void addr_to_bionic(const struct sockaddr_storage *src, socklen_t src_len,
                           void *dst, uint32_t *len) {
  if (!dst || !len) return;
  const struct sockaddr *sa = (const struct sockaddr *)src;
  uint8_t tmp[sizeof(struct sockaddr_storage)];
  memset(tmp, 0, sizeof(tmp));

  uint16_t fam;
  uint32_t size;
  if (sa->sa_family == AF_INET)       { fam = B_AF_INET;  size = B_SIN_LEN; }
  else if (sa->sa_family == AF_INET6) { fam = B_AF_INET6; size = B_SIN6_LEN; }
  else {
    fam = sa->sa_family;
    size = (uint32_t)src_len < sizeof(tmp) ? (uint32_t)src_len : (uint32_t)sizeof(tmp);
  }
  memcpy(tmp, src, size);
  memcpy(tmp, &fam, sizeof(fam));

  memcpy(dst, tmp, *len < size ? *len : size);
  *len = size;
}

// ---------------------------------------------------------------------------
// sockets
// ---------------------------------------------------------------------------

static int set_nonblocking(int fd, int on) {
  int fl = fcntl(fd, F_GETFL, 0);
  if (fl < 0) return -1;
  fl = on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK);
  return fcntl(fd, F_SETFL, fl);
}

int sm127_socket(int domain, int type, int protocol) {
  int host_domain;
  if (domain == B_AF_INET)       host_domain = AF_INET;
  else if (domain == B_AF_INET6) host_domain = AF_INET6;
  else return fail_errno(EAFNOSUPPORT, "socket", -1);

  int fd = socket(host_domain, type & B_SOCK_TYPE_MASK, protocol);
  if (fd < 0) {
    // Godot opens a dual-stack IPv6 socket first when it has not been told
    // the family, and falls back to IPv4 when that fails. The Switch has no
    // IPv6, so that failure is expected; say so once instead of as an error.
    if (host_domain == AF_INET6) {
      static int said;
      const int e = errno;
      if (!said) { said = 1; netLog("no IPv6 here (%s); the engine falls back to IPv4\n", strerror(e)); }
      errno = sm127_errno_to_bionic(e);
      return -1;
    }
    return fail("socket", -1);
  }

  mark_socket(fd, 1);
  FdNote *n = note_for(fd);
  if (n) memset(n, 0, sizeof(*n));
  if ((type & B_SOCK_NONBLOCK) && set_nonblocking(fd, 1) < 0) {
    const int e = errno;
    close(fd);
    mark_socket(fd, 0);
    return fail_errno(e, "socket(SOCK_NONBLOCK)", fd);
  }
  // SOCK_CLOEXEC: nothing is ever exec()ed.

  static int first = 1;
  if (first) {
    first = 0;
    netLog("first socket opened (fd %d, %s, type %d)\n", fd,
           host_domain == AF_INET ? "IPv4" : "IPv6", type & B_SOCK_TYPE_MASK);
  }
  return fd;
}

static void note_connected(int fd) {
  FdNote *n = note_for(fd);
  if (!n || n->state == 2) return;
  if (n->state == 1)
    netLog("fd %d connected to %s in %llu ms\n", fd, n->peer,
           (unsigned long long)(now_ms() - n->t0));
  n->state = 2;
}

int sm127_connect(int fd, const void *addr, uint32_t len) {
  struct sockaddr_storage ss;
  socklen_t sl;
  if (addr_to_host(addr, len, &ss, &sl) < 0) return -1;

  FdNote *n = note_for(fd);
  if (n && n->state == 0) {
    n->state = 1;
    n->t0 = now_ms();
    describe_bionic_addr(addr, len, n->peer, sizeof(n->peer));
  }

  // Godot calls connect() again each frame until it stops reporting
  // EINPROGRESS/EALREADY; EISCONN is the call that says it worked.
  if (connect(fd, (struct sockaddr *)&ss, sl) == 0) {
    note_connected(fd);
    return 0;
  }
  const int e = errno;
  if (e == EISCONN) note_connected(fd);
  else if (!routine_errno(e) && n && take_budget(&s_err_budget))
    netLog("fd %d: connect to %s failed after %llu ms: %s (errno %d)\n", fd, n->peer,
           (unsigned long long)(now_ms() - n->t0), strerror(e), e);
  errno = sm127_errno_to_bionic(e);
  return -1;
}

int sm127_bind(int fd, const void *addr, uint32_t len) {
  struct sockaddr_storage ss;
  socklen_t sl;
  if (addr_to_host(addr, len, &ss, &sl) < 0) return -1;
  return bind(fd, (struct sockaddr *)&ss, sl) == 0 ? 0 : fail("bind", fd);
}

int sm127_listen(int fd, int backlog) {
  return listen(fd, backlog) == 0 ? 0 : fail("listen", fd);
}

int sm127_accept(int fd, void *addr, uint32_t *len) {
  struct sockaddr_storage ss;
  socklen_t sl = sizeof(ss);
  memset(&ss, 0, sizeof(ss));
  int nfd = accept(fd, (struct sockaddr *)&ss, &sl);
  if (nfd < 0) return fail("accept", fd);
  mark_socket(nfd, 1);
  FdNote *n = note_for(nfd);
  if (n) { memset(n, 0, sizeof(*n)); n->state = 2; }
  addr_to_bionic(&ss, sl, addr, len);
  return nfd;
}

static int msg_to_host(int f) {
  int h = 0;
  if (f & B_MSG_OOB)       h |= MSG_OOB;
  if (f & B_MSG_PEEK)      h |= MSG_PEEK;
  if (f & B_MSG_DONTROUTE) h |= MSG_DONTROUTE;
  if (f & B_MSG_TRUNC)     h |= MSG_TRUNC;
  if (f & B_MSG_DONTWAIT)  h |= MSG_DONTWAIT;
  if (f & B_MSG_EOR)       h |= MSG_EOR;
  if (f & B_MSG_WAITALL)   h |= MSG_WAITALL;
  // B_MSG_NOSIGNAL, which Godot sets on every stream send, is dropped: there
  // is no SIGPIPE to suppress, and 0x4000 is MSG_NBIO to a BSD stack.
  return h;
}

ssize_t sm127_send(int fd, const void *buf, size_t len, int flags) {
  ssize_t r = send(fd, buf, len, msg_to_host(flags));
  return r >= 0 ? r : (ssize_t)fail("send", fd);
}

ssize_t sm127_recv(int fd, void *buf, size_t len, int flags) {
  ssize_t r = recv(fd, buf, len, msg_to_host(flags));
  return r >= 0 ? r : (ssize_t)fail("recv", fd);
}

ssize_t sm127_sendto(int fd, const void *buf, size_t len, int flags,
                     const void *addr, uint32_t addr_len) {
  if (!addr) return sm127_send(fd, buf, len, flags);
  struct sockaddr_storage ss;
  socklen_t sl;
  if (addr_to_host(addr, addr_len, &ss, &sl) < 0) return -1;
  ssize_t r = sendto(fd, buf, len, msg_to_host(flags), (struct sockaddr *)&ss, sl);
  return r >= 0 ? r : (ssize_t)fail("sendto", fd);
}

ssize_t sm127_recvfrom(int fd, void *buf, size_t len, int flags,
                       void *addr, uint32_t *addr_len) {
  struct sockaddr_storage ss;
  socklen_t sl = sizeof(ss);
  memset(&ss, 0, sizeof(ss));
  ssize_t r = recvfrom(fd, buf, len, msg_to_host(flags), (struct sockaddr *)&ss, &sl);
  if (r < 0) return (ssize_t)fail("recvfrom", fd);
  addr_to_bionic(&ss, sl, addr, addr_len);
  return r;
}

int sm127_shutdown(int fd, int how) {
  // SHUT_RD/WR/RDWR are 0/1/2 everywhere.
  return shutdown(fd, how) == 0 ? 0 : fail("shutdown", fd);
}

int sm127_getsockname(int fd, void *addr, uint32_t *len) {
  struct sockaddr_storage ss;
  socklen_t sl = sizeof(ss);
  memset(&ss, 0, sizeof(ss));
  if (getsockname(fd, (struct sockaddr *)&ss, &sl) != 0) return fail("getsockname", fd);
  addr_to_bionic(&ss, sl, addr, len);
  return 0;
}

int sm127_getpeername(int fd, void *addr, uint32_t *len) {
  struct sockaddr_storage ss;
  socklen_t sl = sizeof(ss);
  memset(&ss, 0, sizeof(ss));
  if (getpeername(fd, (struct sockaddr *)&ss, &sl) != 0) return fail("getpeername", fd);
  addr_to_bionic(&ss, sl, addr, len);
  return 0;
}

// ---------------------------------------------------------------------------
// socket options
// ---------------------------------------------------------------------------

// 0 with the host's level/option, or -1 if the host has no such option.
static int opt_to_host(int level, int opt, int *hl, int *ho) {
  switch (level) {
    case B_SOL_SOCKET:
      *hl = SOL_SOCKET;
      switch (opt) {
        case B_SO_DEBUG:     *ho = SO_DEBUG;     return 0;
        case B_SO_REUSEADDR: *ho = SO_REUSEADDR; return 0;
        case B_SO_TYPE:      *ho = SO_TYPE;      return 0;
        case B_SO_ERROR:     *ho = SO_ERROR;     return 0;
        case B_SO_DONTROUTE: *ho = SO_DONTROUTE; return 0;
        case B_SO_BROADCAST: *ho = SO_BROADCAST; return 0;
        case B_SO_SNDBUF:    *ho = SO_SNDBUF;    return 0;
        case B_SO_RCVBUF:    *ho = SO_RCVBUF;    return 0;
        case B_SO_KEEPALIVE: *ho = SO_KEEPALIVE; return 0;
        case B_SO_OOBINLINE: *ho = SO_OOBINLINE; return 0;
        case B_SO_LINGER:    *ho = SO_LINGER;    return 0;
        case B_SO_REUSEPORT: *ho = SO_REUSEPORT; return 0;
        case B_SO_RCVLOWAT:  *ho = SO_RCVLOWAT;  return 0;
        case B_SO_SNDLOWAT:  *ho = SO_SNDLOWAT;  return 0;
        case B_SO_RCVTIMEO:  *ho = SO_RCVTIMEO;  return 0;   // struct timeval: same layout
        case B_SO_SNDTIMEO:  *ho = SO_SNDTIMEO;  return 0;
      }
      return -1;
    case B_IPPROTO_IP:
      *hl = IPPROTO_IP;
      switch (opt) {
        case B_IP_TOS:             *ho = IP_TOS;             return 0;
        case B_IP_TTL:             *ho = IP_TTL;             return 0;
        case B_IP_MULTICAST_IF:    *ho = IP_MULTICAST_IF;    return 0;
        case B_IP_MULTICAST_TTL:   *ho = IP_MULTICAST_TTL;   return 0;
        case B_IP_MULTICAST_LOOP:  *ho = IP_MULTICAST_LOOP;  return 0;
        case B_IP_ADD_MEMBERSHIP:  *ho = IP_ADD_MEMBERSHIP;  return 0;
        case B_IP_DROP_MEMBERSHIP: *ho = IP_DROP_MEMBERSHIP; return 0;
      }
      return -1;
    case B_IPPROTO_IPV6:
      *hl = IPPROTO_IPV6;
      switch (opt) {
#ifdef IPV6_UNICAST_HOPS
        case B_IPV6_UNICAST_HOPS:   *ho = IPV6_UNICAST_HOPS;   return 0;
#endif
#ifdef IPV6_MULTICAST_IF
        case B_IPV6_MULTICAST_IF:   *ho = IPV6_MULTICAST_IF;   return 0;
#endif
#ifdef IPV6_MULTICAST_HOPS
        case B_IPV6_MULTICAST_HOPS: *ho = IPV6_MULTICAST_HOPS; return 0;
#endif
#ifdef IPV6_MULTICAST_LOOP
        case B_IPV6_MULTICAST_LOOP: *ho = IPV6_MULTICAST_LOOP; return 0;
#endif
#ifdef IPV6_JOIN_GROUP
        case B_IPV6_JOIN_GROUP:     *ho = IPV6_JOIN_GROUP;     return 0;
#endif
#ifdef IPV6_LEAVE_GROUP
        case B_IPV6_LEAVE_GROUP:    *ho = IPV6_LEAVE_GROUP;    return 0;
#endif
#ifdef IPV6_V6ONLY
        case B_IPV6_V6ONLY:         *ho = IPV6_V6ONLY;         return 0;
#endif
      }
      return -1;
    case B_IPPROTO_TCP:
      *hl = IPPROTO_TCP;
      switch (opt) {
        case B_TCP_NODELAY:  *ho = TCP_NODELAY;  return 0;
#ifdef TCP_KEEPIDLE
        case B_TCP_KEEPIDLE: *ho = TCP_KEEPIDLE; return 0;
#endif
#ifdef TCP_KEEPINTVL
        case B_TCP_KEEPINTVL: *ho = TCP_KEEPINTVL; return 0;
#endif
#ifdef TCP_KEEPCNT
        case B_TCP_KEEPCNT:  *ho = TCP_KEEPCNT;  return 0;
#endif
      }
      return -1;
    case B_IPPROTO_UDP:
      *hl = IPPROTO_UDP;
      return -1;
  }
  return -1;
}

int sm127_setsockopt(int fd, int level, int opt, const void *val, uint32_t len) {
  int hl, ho;
  if (opt_to_host(level, opt, &hl, &ho) < 0) {
    if (take_budget(&s_err_budget))
      netLog("setsockopt(fd %d): no equivalent for level %d option %d\n", fd, level, opt);
    errno = sm127_errno_to_bionic(ENOPROTOOPT);
    return -1;
  }
  return setsockopt(fd, hl, ho, val, (socklen_t)len) == 0 ? 0 : fail("setsockopt", fd);
}

int sm127_getsockopt(int fd, int level, int opt, void *val, uint32_t *len) {
  int hl, ho;
  if (opt_to_host(level, opt, &hl, &ho) < 0) {
    errno = sm127_errno_to_bionic(ENOPROTOOPT);
    return -1;
  }
  socklen_t sl = len ? (socklen_t)*len : 0;
  if (getsockopt(fd, hl, ho, val, &sl) != 0) return fail("getsockopt", fd);
  if (len) *len = (uint32_t)sl;
  // SO_ERROR hands back an error number: the engine wants bionic's.
  if (level == B_SOL_SOCKET && opt == B_SO_ERROR && val && sl >= sizeof(int)) {
    int e;
    memcpy(&e, val, sizeof(e));
    e = sm127_errno_to_bionic(e);
    memcpy(val, &e, sizeof(e));
  }
  return 0;
}

// ---------------------------------------------------------------------------
// poll / ioctl
// ---------------------------------------------------------------------------

static short poll_to_host(short b) {
  short h = 0;
  if (b & B_POLLIN)                    h |= POLLIN;
  if (b & B_POLLPRI)                   h |= POLLPRI;
  if (b & (B_POLLOUT | B_POLLWRNORM))  h |= POLLOUT;
  if (b & B_POLLRDNORM)                h |= POLLRDNORM;
  if (b & B_POLLRDBAND)                h |= POLLRDBAND;
  if (b & B_POLLWRBAND)                h |= POLLWRBAND;
  // POLLRDHUP (0x2000) is dropped: to FreeBSD that bit is POLLINIGNEOF.
  return h;
}

static short poll_to_bionic(short h, short asked) {
  short b = 0;
  if (h & POLLIN)     b |= B_POLLIN;
  if (h & POLLPRI)    b |= B_POLLPRI;
  if (h & POLLOUT)    b |= asked & (B_POLLOUT | B_POLLWRNORM);
  if (h & POLLERR)    b |= B_POLLERR;
  if (h & POLLHUP)    b |= B_POLLHUP;
  if (h & POLLNVAL)   b |= B_POLLNVAL;
  if (h & POLLRDNORM) b |= B_POLLRDNORM;
  if (h & POLLRDBAND) b |= B_POLLRDBAND;
  if ((h & POLLWRBAND) && POLLWRBAND != POLLOUT) b |= B_POLLWRBAND;
  return b;
}

int sm127_poll(void *fds_, unsigned long nfds, int timeout) {
  BPollfd *bf = fds_;
  if (nfds == 0) {
    if (timeout > 0) usleep((useconds_t)timeout * 1000);
    return 0;
  }
  if (!bf) return fail_errno(EFAULT, "poll", -1);

  // Not every descriptor a socket: what poll() was for everything before this
  // port had a network -- nothing ready.
  for (unsigned long i = 0; i < nfds; i++) {
    if (!is_socket(bf[i].fd)) {
      for (unsigned long j = 0; j < nfds; j++) bf[j].revents = 0;
      return 0;
    }
  }

  struct pollfd stack[16];
  struct pollfd *hf = nfds <= 16 ? stack : malloc(nfds * sizeof(*hf));
  if (!hf) return fail_errno(ENOMEM, "poll", -1);
  for (unsigned long i = 0; i < nfds; i++) {
    hf[i].fd = bf[i].fd;
    hf[i].events = poll_to_host(bf[i].events);
    hf[i].revents = 0;
  }
  const int rc = poll(hf, (nfds_t)nfds, timeout);
  const int e = errno;
  if (rc >= 0)
    for (unsigned long i = 0; i < nfds; i++)
      bf[i].revents = poll_to_bionic(hf[i].revents, bf[i].events);
  if (hf != stack) free(hf);
  return rc >= 0 ? rc : fail_errno(e, "poll", bf[0].fd);
}

int sm127_ioctl(int fd, int request, ...) {
  va_list ap;
  va_start(ap, request);
  void *arg = va_arg(ap, void *);
  va_end(ap);

  // What ioctl() did for every descriptor before sockets existed here.
  if (!is_socket(fd)) return 0;

  switch ((unsigned)request) {
    case B_FIOCLEX:
    case B_FIONCLEX:
      // Close-on-exec, which Godot sets on every socket it opens. Nothing is
      // ever exec()ed here, so there is nothing to do and nothing to report.
      return 0;
    case B_FIONBIO: {
      // Godot passes an unsigned long; the low half is the flag either way.
      int on = 0;
      if (arg) memcpy(&on, arg, sizeof(on));
      return set_nonblocking(fd, on != 0) == 0 ? 0 : fail("ioctl(FIONBIO)", fd);
    }
    case B_FIONREAD: {
      int count = 0;
      if (ioctl(fd, FIONREAD, &count) == 0) {
        if (arg) memcpy(arg, &count, sizeof(count));
        return 0;
      }
      // Horizon's stack not taking FIONREAD would otherwise read as "no data,
      // peer closed" to the engine, which disconnects on exactly that. A peek
      // gives the same answer: bytes waiting, 0 at end of stream.
      static int said;
      if (!said) { said = 1; netLog("FIONREAD unsupported (%s); counting with MSG_PEEK\n", strerror(errno)); }
      char peek[4096];
      ssize_t r = recv(fd, peek, sizeof(peek), MSG_PEEK | MSG_DONTWAIT);
      if (r < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) return fail("ioctl(FIONREAD)", fd);
        r = 0;
      }
      count = (int)r;
      if (arg) memcpy(arg, &count, sizeof(count));
      return 0;
    }
  }
  if (take_budget(&s_err_budget))
    netLog("ioctl(fd %d, 0x%x): not translated\n", fd, (unsigned)request);
  errno = ENOTTY;   // 25: the same number in both
  return -1;
}

void sm127_net_forget_fd(int fd) {
  if (!is_socket(fd)) return;
  mark_socket(fd, 0);
  FdNote *n = note_for(fd);
  if (n) n->state = 0;
}

// ---------------------------------------------------------------------------
// name resolution
// ---------------------------------------------------------------------------

static int eai_to_bionic(int e) {
  switch (e) {
#ifdef EAI_ADDRFAMILY
    case EAI_ADDRFAMILY: return B_EAI_ADDRFAMILY;
#endif
    case EAI_AGAIN:      return B_EAI_AGAIN;
    case EAI_BADFLAGS:   return B_EAI_BADFLAGS;
    case EAI_FAIL:       return B_EAI_FAIL;
    case EAI_FAMILY:     return B_EAI_FAMILY;
    case EAI_MEMORY:     return B_EAI_MEMORY;
#if defined(EAI_NODATA) && EAI_NODATA != EAI_NONAME
    case EAI_NODATA:     return B_EAI_NODATA;
#endif
    case EAI_NONAME:     return B_EAI_NONAME;
    case EAI_SERVICE:    return B_EAI_SERVICE;
    case EAI_SOCKTYPE:   return B_EAI_SOCKTYPE;
    case EAI_SYSTEM:     return B_EAI_SYSTEM;
#ifdef EAI_BADHINTS
    case EAI_BADHINTS:   return B_EAI_BADHINTS;
#endif
#ifdef EAI_PROTOCOL
    case EAI_PROTOCOL:   return B_EAI_PROTOCOL;
#endif
#ifdef EAI_OVERFLOW
    case EAI_OVERFLOW:   return B_EAI_OVERFLOW;
#endif
  }
  return B_EAI_FAIL;
}

const char *sm127_gai_strerror(int code) {
  switch (code) {
    case 0:                return "Success";
    case B_EAI_ADDRFAMILY: return "Address family for hostname not supported";
    case B_EAI_AGAIN:      return "Temporary failure in name resolution";
    case B_EAI_BADFLAGS:   return "Invalid value for ai_flags";
    case B_EAI_FAIL:       return "Non-recoverable failure in name resolution";
    case B_EAI_FAMILY:     return "ai_family not supported";
    case B_EAI_MEMORY:     return "Memory allocation failure";
    case B_EAI_NODATA:     return "No address associated with hostname";
    case B_EAI_NONAME:     return "hostname nor servname provided, or not known";
    case B_EAI_SERVICE:    return "servname not supported for ai_socktype";
    case B_EAI_SOCKTYPE:   return "ai_socktype not supported";
    case B_EAI_SYSTEM:     return "System error returned in errno";
    case B_EAI_BADHINTS:   return "Invalid value for hints";
    case B_EAI_PROTOCOL:   return "Resolved protocol is unknown";
    case B_EAI_OVERFLOW:   return "Argument buffer overflow";
  }
  return "Unknown error";
}

static int s_dns_budget = 40;

void sm127_freeaddrinfo(void *res) {
  BAddrinfo *n = res;
  while (n) {
    BAddrinfo *next = n->ai_next;
    free(n);   // one allocation per node: the node, its sockaddr, its name
    n = next;
  }
}

int sm127_getaddrinfo(const char *node, const char *service,
                      const void *hints_, void **res) {
  const BAddrinfo *bh = hints_;
  if (res) *res = NULL;

  struct addrinfo h;
  memset(&h, 0, sizeof(h));
  // An unspecified family asks for IPv4: the Switch has no IPv6, and an AAAA
  // answer would only send the engine to a socket it cannot open.
  h.ai_family = AF_INET;
  if (bh) {
    if (bh->ai_family == B_AF_INET6)      h.ai_family = AF_INET6;
    else if (bh->ai_family != B_AF_INET && bh->ai_family != B_AF_UNSPEC)
      return B_EAI_FAMILY;
    // AI_ADDRCONFIG / AI_V4MAPPED / AI_ALL are dropped: they only choose
    // between address families, and the family is already decided above.
    if (bh->ai_flags & B_AI_PASSIVE)     h.ai_flags |= AI_PASSIVE;
    if (bh->ai_flags & B_AI_CANONNAME)   h.ai_flags |= AI_CANONNAME;
    if (bh->ai_flags & B_AI_NUMERICHOST) h.ai_flags |= AI_NUMERICHOST;
    if (bh->ai_flags & B_AI_NUMERICSERV) h.ai_flags |= AI_NUMERICSERV;
    h.ai_socktype = bh->ai_socktype;   // SOCK_STREAM/DGRAM/RAW agree
    h.ai_protocol = bh->ai_protocol;
  }

  const uint64_t t0 = now_ms();
  struct addrinfo *hr = NULL;
  const int rc = getaddrinfo(node, service, &h, &hr);
  const uint64_t ms = now_ms() - t0;
  if (rc != 0) {
    const int e = errno;
    if (take_budget(&s_dns_budget))
      netLog("resolve %s failed after %llu ms: %s\n", node ? node : "(null)",
             (unsigned long long)ms, gai_strerror(rc));
    if (rc == EAI_SYSTEM) errno = sm127_errno_to_bionic(e);
    return eai_to_bionic(rc);
  }

  BAddrinfo *head = NULL, **tail = &head;
  int count = 0;
  char first[48] = "";
  for (struct addrinfo *p = hr; p; p = p->ai_next) {
    if (!p->ai_addr || (p->ai_family != AF_INET && p->ai_family != AF_INET6)) continue;
    const size_t canon = p->ai_canonname ? strlen(p->ai_canonname) + 1 : 0;
    BAddrinfo *n = calloc(1, sizeof(*n) + sizeof(struct sockaddr_storage) + canon);
    if (!n) {
      sm127_freeaddrinfo(head);
      freeaddrinfo(hr);
      return B_EAI_MEMORY;
    }
    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof(ss));
    memcpy(&ss, p->ai_addr, p->ai_addrlen < sizeof(ss) ? p->ai_addrlen : sizeof(ss));
    uint32_t blen = sizeof(struct sockaddr_storage);
    n->ai_addr = (char *)(n + 1);
    addr_to_bionic(&ss, p->ai_addrlen, n->ai_addr, &blen);
    n->ai_addrlen = blen;
    n->ai_family = p->ai_family == AF_INET ? B_AF_INET : B_AF_INET6;
    n->ai_flags = bh ? bh->ai_flags : 0;
    n->ai_socktype = p->ai_socktype;
    n->ai_protocol = p->ai_protocol;
    if (canon) {
      n->ai_canonname = (char *)(n + 1) + sizeof(struct sockaddr_storage);
      memcpy(n->ai_canonname, p->ai_canonname, canon);
    }
    if (!count) describe_bionic_addr(n->ai_addr, blen, first, sizeof(first));
    *tail = n;
    tail = &n->ai_next;
    count++;
  }
  freeaddrinfo(hr);

  if (!head) {
    if (take_budget(&s_dns_budget))
      netLog("resolve %s: no usable address\n", node ? node : "(null)");
    return B_EAI_NODATA;
  }
  if (take_budget(&s_dns_budget)) {
    // The port field is 0 here; the address is what is worth seeing.
    char *colon = strrchr(first, ':');
    if (colon) *colon = 0;
    netLog("resolved %s -> %s%s in %llu ms\n", node ? node : "(null)", first,
           count > 1 ? " (and others)" : "", (unsigned long long)ms);
  }
  *res = head;
  return 0;
}

int sm127_inet_pton(int af, const char *src, void *dst) {
  if (af == B_AF_INET)  return inet_pton(AF_INET, src, dst);
  if (af == B_AF_INET6) return inet_pton(AF_INET6, src, dst);
  errno = sm127_errno_to_bionic(EAFNOSUPPORT);
  return -1;
}
