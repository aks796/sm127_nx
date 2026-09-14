/* bionic_errno.c -- newlib error numbers, as bionic's.
 *
 * libgodot_android.so compares errno, and the error codes pthread returns,
 * against bionic's numbers. Everything that reaches it from newlib or libnx
 * carries newlib's. Values 1..34 (EPERM..ERANGE) agree; from 35 up the two
 * diverge completely, and a number passed through untranslated is read as a
 * different error. For sockets that is fatal to the connection, not cosmetic:
 * a non-blocking connect() reports EINPROGRESS, which is 119 in newlib and 115
 * in bionic, and the engine treats anything it does not recognise as a failure.
 *
 * Every case names the constant, never its number, so the compiler supplies
 * this libc's value. A wrong left-hand side cannot be written (the hand-typed
 * table this replaces mapped newlib's ENOPROTOOPT and ESHUTDOWN as if they were
 * EDESTADDRREQ and EMSGSIZE), and the same file builds against macOS headers
 * for the host test. The right-hand side is bionic's number, from Linux's
 * asm-generic errno.h.
 *
 * MIT license; see LICENSE. */

#include <errno.h>

#include "bionic_errno.h"

int sm127_errno_to_bionic(int e) {
  switch (e) {
    case EAGAIN:          return 11;   // same in newlib; 35 on the macOS host
    case EDEADLK:         return 35;
    case ENAMETOOLONG:    return 36;
    case ENOLCK:          return 37;
    case ENOSYS:          return 38;
    case ENOTEMPTY:       return 39;
    case ELOOP:           return 40;
#ifdef ENOMSG
    case ENOMSG:          return 42;
#endif
#ifdef EIDRM
    case EIDRM:           return 43;
#endif
#ifdef ENOSTR
    case ENOSTR:          return 60;
#endif
#ifdef ENODATA
    case ENODATA:         return 61;
#endif
#ifdef ETIME
    case ETIME:           return 62;
#endif
#ifdef ENOSR
    case ENOSR:           return 63;
#endif
#ifdef ENOLINK
    case ENOLINK:         return 67;
#endif
#ifdef EPROTO
    case EPROTO:          return 71;
#endif
#ifdef EMULTIHOP
    case EMULTIHOP:       return 72;
#endif
#ifdef EBADMSG
    case EBADMSG:         return 74;
#endif
#ifdef EOVERFLOW
    case EOVERFLOW:       return 75;
#endif
#ifdef EILSEQ
    case EILSEQ:          return 84;
#endif
    case ENOTSOCK:        return 88;
    case EDESTADDRREQ:    return 89;
    case EMSGSIZE:        return 90;
    case EPROTOTYPE:      return 91;
    case ENOPROTOOPT:     return 92;
    case EPROTONOSUPPORT: return 93;
#ifdef ESOCKTNOSUPPORT
    case ESOCKTNOSUPPORT: return 94;
#endif
    case EOPNOTSUPP:      return 95;
#if defined(ENOTSUP) && ENOTSUP != EOPNOTSUPP
    case ENOTSUP:         return 95;   // bionic defines ENOTSUP as EOPNOTSUPP
#endif
#ifdef EPFNOSUPPORT
    case EPFNOSUPPORT:    return 96;
#endif
    case EAFNOSUPPORT:    return 97;
    case EADDRINUSE:      return 98;
    case EADDRNOTAVAIL:   return 99;
    case ENETDOWN:        return 100;
    case ENETUNREACH:     return 101;
    case ENETRESET:       return 102;
    case ECONNABORTED:    return 103;
    case ECONNRESET:      return 104;
    case ENOBUFS:         return 105;
    case EISCONN:         return 106;
    case ENOTCONN:        return 107;
#ifdef ESHUTDOWN
    case ESHUTDOWN:       return 108;
#endif
#ifdef ETOOMANYREFS
    case ETOOMANYREFS:    return 109;
#endif
    case ETIMEDOUT:       return 110;
    case ECONNREFUSED:    return 111;
#ifdef EHOSTDOWN
    case EHOSTDOWN:       return 112;
#endif
    case EHOSTUNREACH:    return 113;
    case EALREADY:        return 114;
    case EINPROGRESS:     return 115;
#ifdef ESTALE
    case ESTALE:          return 116;
#endif
#ifdef EDQUOT
    case EDQUOT:          return 122;
#endif
#ifdef ECANCELED
    case ECANCELED:       return 125;
#endif
#ifdef EOWNERDEAD
    case EOWNERDEAD:      return 130;
#endif
#ifdef ENOTRECOVERABLE
    case ENOTRECOVERABLE: return 131;
#endif
    default:              return e;    // 1..34 agree; anything else has no bionic twin
  }
}
