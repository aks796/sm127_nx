/* net_shim_test.c -- host test for source/net_shim.c and source/bionic_errno.c.
 *
 * Calls the shim the way libgodot_android.so does -- bionic constants, Linux
 * sockaddr layout, the engine's own sequence of resolve, ioctl(FIONBIO),
 * non-blocking connect polled to EISCONN, send with MSG_NOSIGNAL, poll +
 * FIONREAD + recv to end of stream -- against levelsharesquare.com, and checks
 * what comes back is bionic-shaped. Needs a network connection.
 *
 * macOS (BSD sockets, like libnx):
 *   cc -std=gnu11 -Wall -Wextra -Isource source/net_shim.c source/bionic_errno.c \
 *      tools/net_shim_test.c -o /tmp/net_shim_test && /tmp/net_shim_test
 *
 * MIT license; see LICENSE. */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include "net_shim.h"
#include "bionic_errno.h"

static int fails;
#define CHECK(c, ...) do { int _ok = (c); if (!_ok) fails++; printf("%s ", _ok ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); } while (0)

typedef struct BAI { int ai_flags, ai_family, ai_socktype, ai_protocol; uint32_t ai_addrlen; char *ai_canonname; void *ai_addr; struct BAI *ai_next; } BAI;
typedef struct { int fd; short events, revents; } BP;
static int berr(void) { return errno; }   // after a shim call, errno is bionic's

static int wait_connected(int fd, uint8_t *sa, int *seq, int nseq) {
  int n = 0;
  for (int i = 0; i < 100; i++) {
    int rc = sm127_connect(fd, sa, 16);
    int e = rc == 0 ? 0 : berr();
    if (n < nseq) seq[n++] = e;
    if (rc == 0 || e == 106) return e == 106 ? 106 : 0;
    if (e != 115 && e != 114) return e;
    BP p = { fd, 0x4, 0 };
    sm127_poll(&p, 1, 100);
  }
  return -1;
}

int main(void) {
  signal(SIGPIPE, SIG_IGN);

  CHECK(sm127_errno_to_bionic(EINPROGRESS) == 115, "EINPROGRESS -> 115");
  CHECK(sm127_errno_to_bionic(EALREADY) == 114, "EALREADY -> 114");
  CHECK(sm127_errno_to_bionic(EISCONN) == 106, "EISCONN -> 106");
  CHECK(sm127_errno_to_bionic(ENOTCONN) == 107, "ENOTCONN -> 107");
  CHECK(sm127_errno_to_bionic(ECONNREFUSED) == 111, "ECONNREFUSED -> 111");
  CHECK(sm127_errno_to_bionic(ETIMEDOUT) == 110, "ETIMEDOUT -> 110");
  CHECK(sm127_errno_to_bionic(EAGAIN) == 11, "EAGAIN -> 11");
  CHECK(sm127_errno_to_bionic(EDEADLK) == 35, "EDEADLK -> 35");
  CHECK(sm127_errno_to_bionic(ENOSYS) == 38, "ENOSYS -> 38");
  CHECK(sm127_errno_to_bionic(ENOPROTOOPT) == 92, "ENOPROTOOPT -> 92");
  CHECK(sm127_errno_to_bionic(EINVAL) == 22, "EINVAL -> 22 (pass-through)");

  // DNS, the way ip_unix.cpp asks: AF_UNSPEC + AI_ADDRCONFIG (bionic 0x400)
  BAI hints; memset(&hints, 0, sizeof hints); hints.ai_family = 0; hints.ai_flags = 0x400;
  void *res = NULL;
  int rc = sm127_getaddrinfo("levelsharesquare.com", NULL, &hints, &res);
  CHECK(rc == 0 && res, "getaddrinfo(levelsharesquare.com) rc=%d", rc);
  if (!res) return 1;
  BAI *ai = res;
  uint16_t fam; memcpy(&fam, ai->ai_addr, 2);
  CHECK(ai->ai_family == 2 && fam == 2 && ai->ai_addrlen == 16, "result is bionic AF_INET (ai_family %d, sa_family u16 %u, addrlen %u)", ai->ai_family, fam, ai->ai_addrlen);
  uint8_t sa[16]; memcpy(sa, ai->ai_addr, 16);
  sm127_freeaddrinfo(res);

  res = NULL;
  rc = sm127_getaddrinfo("no-such-host.invalid", NULL, &hints, &res);
  CHECK(rc != 0 && res == NULL, "unknown host fails: rc=%d (%s)", rc, sm127_gai_strerror(rc));

  // TCP the way net_socket_posix.cpp does it
  int fd = sm127_socket(2, 1, 6);
  CHECK(fd >= 0, "socket(AF_INET, SOCK_STREAM) -> fd %d", fd);
  int one = 1;
  CHECK(sm127_setsockopt(fd, 1, 2, &one, 4) == 0, "setsockopt SOL_SOCKET/SO_REUSEADDR");
  CHECK(sm127_setsockopt(fd, 6, 1, &one, 4) == 0, "setsockopt IPPROTO_TCP/TCP_NODELAY");
  int v = 0; uint32_t vl = 4;
  CHECK(sm127_getsockopt(fd, 1, 7, &v, &vl) == 0 && vl == 4 && v > 0, "getsockopt SO_SNDBUF = %d", v);
  vl = 4; v = 0;
  CHECK(sm127_getsockopt(fd, 1, 3, &v, &vl) == 0 && v == 1, "getsockopt SO_TYPE = %d", v);
  CHECK(sm127_setsockopt(fd, 1, 99, &one, 4) == -1 && berr() == 92, "unknown option -> ENOPROTOOPT (92), got %d", berr());
  unsigned long par = 1;
  CHECK(sm127_ioctl(fd, 0x5421, &par) == 0, "ioctl FIONBIO(unsigned long 1)");
  CHECK((fcntl(fd, F_GETFL) & O_NONBLOCK) != 0, "socket is now non-blocking");

  sa[2] = 0; sa[3] = 80;
  int seq[8] = {0};
  struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
  int e = wait_connected(fd, sa, seq, 8);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  CHECK(e == 106 || e == 0, "connect sequence ends connected (first errno %d, then %d, final %d)", seq[0], seq[1], e);
  CHECK(seq[0] == 115 || seq[0] == 0, "first non-blocking connect reports bionic EINPROGRESS (115): %d", seq[0]);

  uint8_t name[32]; uint32_t nl = sizeof name;
  CHECK(sm127_getsockname(fd, name, &nl) == 0 && nl == 16 && name[0] == 2 && name[1] == 0, "getsockname: bionic layout, len %u, bytes %02x %02x", nl, name[0], name[1]);
  nl = sizeof name;
  CHECK(sm127_getpeername(fd, name, &nl) == 0 && nl == 16 && memcmp(name + 2, sa + 2, 6) == 0, "getpeername matches the address connected to");

  const char *req = "GET / HTTP/1.1\r\nHost: levelsharesquare.com\r\nConnection: close\r\n\r\n";
  ssize_t s = sm127_send(fd, req, strlen(req), 0x4000);
  CHECK(s == (ssize_t)strlen(req), "send with MSG_NOSIGNAL -> %zd", s);

  char buf[65536]; size_t total = 0; int saw_avail = 0, eof_seen = 0;
  for (int i = 0; i < 200 && !eof_seen; i++) {
    BP p = { fd, 0x1, 0 };
    int pr = sm127_poll(&p, 1, 100);
    if (pr <= 0) continue;
    int avail = -1;
    sm127_ioctl(fd, 0x541B, &avail);
    if (avail > 0) saw_avail = 1;
    ssize_t r = sm127_recv(fd, buf + total, sizeof(buf) - 1 - total, 0);
    if (r == 0) { eof_seen = 1; CHECK(avail == 0, "at EOF: poll readable and FIONREAD 0 (Godot's disconnect test), avail %d", avail); }
    else if (r > 0) total += (size_t)r;
    else if (berr() != 11) { CHECK(0, "recv errno %d", berr()); break; }
  }
  buf[total] = 0;
  CHECK(saw_avail, "FIONREAD reported waiting bytes");
  CHECK(total > 12 && strncmp(buf, "HTTP/1.1 ", 9) == 0, "HTTP response over the shim: %.20s (%zu bytes)", buf, total);
  CHECK(eof_seen, "server close seen as recv 0");
  sm127_net_forget_fd(fd); close(fd);

  // refused, with SOCK_NONBLOCK in the type
  int fd2 = sm127_socket(2, 1 | 04000, 6);
  CHECK(fd2 >= 0 && (fcntl(fd2, F_GETFL) & O_NONBLOCK), "socket(SOCK_STREAM|SOCK_NONBLOCK) is non-blocking");
  char tmp[8];
  CHECK(sm127_recv(fd2, tmp, sizeof tmp, 0) == -1 && (berr() == 107 || berr() == 11), "recv before connect -> ENOTCONN (107) or EAGAIN: %d", berr());
  uint8_t lo[16] = { 2, 0, 0, 1, 127, 0, 0, 1 };
  int seq2[8] = {0};
  e = wait_connected(fd2, lo, seq2, 8);
  CHECK(e == 111, "connect to 127.0.0.1:1 ends ECONNREFUSED (111): %d (seq %d %d %d)", e, seq2[0], seq2[1], seq2[2]);
  sm127_net_forget_fd(fd2); close(fd2);

  int fd3 = sm127_socket(2, 1, 6);
  unsigned long cloexec = 1;
  CHECK(sm127_ioctl(fd3, 0x5451, &cloexec) == 0, "ioctl FIOCLEX (Godot sets it on every socket) -> 0, silently");
  sm127_net_forget_fd(fd3); close(fd3);

  // descriptors that are not sockets keep the old behaviour
  int pf[2]; pipe(pf);
  BP pp = { pf[0], 0x1, 0x7f };
  CHECK(sm127_poll(&pp, 1, 0) == 0 && pp.revents == 0, "poll on a pipe: 0, revents cleared");
  int junk = 12345;
  CHECK(sm127_ioctl(pf[0], 0x541B, &junk) == 0 && junk == 12345, "ioctl on a pipe: 0, untouched");
  close(pf[0]); close(pf[1]);

  uint8_t a4[4];
  CHECK(sm127_inet_pton(2, "10.1.2.3", a4) == 1 && a4[0] == 10 && a4[3] == 3, "inet_pton(AF_INET)");
  CHECK(sm127_socket(1, 1, 0) == -1 && berr() == 97, "AF_UNIX -> EAFNOSUPPORT (97)");

  printf("\n%s: %d failure(s)\n", fails ? "FAILED" : "PASSED", fails);
  return fails != 0;
}
