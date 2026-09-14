/* net_shim.h -- the engine's BSD socket calls, on libnx. See net_shim.c.
 *
 * Declared with plain types only: imports.c includes this next to newlib's own
 * <sys/socket.h>, and these take bionic-shaped structures, not newlib's.
 *
 * MIT license; see LICENSE. */

#ifndef __NET_SHIM_H__
#define __NET_SHIM_H__

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

int     sm127_socket(int domain, int type, int protocol);
int     sm127_bind(int fd, const void *addr, uint32_t len);
int     sm127_listen(int fd, int backlog);
int     sm127_accept(int fd, void *addr, uint32_t *len);
int     sm127_connect(int fd, const void *addr, uint32_t len);
ssize_t sm127_send(int fd, const void *buf, size_t len, int flags);
ssize_t sm127_sendto(int fd, const void *buf, size_t len, int flags,
                     const void *addr, uint32_t addr_len);
ssize_t sm127_recv(int fd, void *buf, size_t len, int flags);
ssize_t sm127_recvfrom(int fd, void *buf, size_t len, int flags,
                       void *addr, uint32_t *addr_len);
int     sm127_setsockopt(int fd, int level, int opt, const void *val, uint32_t len);
int     sm127_getsockopt(int fd, int level, int opt, void *val, uint32_t *len);
int     sm127_getsockname(int fd, void *addr, uint32_t *len);
int     sm127_getpeername(int fd, void *addr, uint32_t *len);
int     sm127_shutdown(int fd, int how);
int     sm127_poll(void *fds, unsigned long nfds, int timeout);
int     sm127_ioctl(int fd, int request, ...);

int         sm127_getaddrinfo(const char *node, const char *service,
                              const void *hints, void **res);
void        sm127_freeaddrinfo(void *res);
const char *sm127_gai_strerror(int code);
int         sm127_inet_pton(int af, const char *src, void *dst);

// close() of any descriptor: forget it if it was a socket. Called by
// close_fake before the descriptor number can be reused.
void sm127_net_forget_fd(int fd);

#endif
