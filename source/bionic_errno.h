/* bionic_errno.h -- newlib error numbers, as bionic's. See bionic_errno.c.
 * MIT license; see LICENSE. */

#ifndef __BIONIC_ERRNO_H__
#define __BIONIC_ERRNO_H__

// Translates an error number from this libc's numbering (newlib on the Switch,
// the host's in the host test) to the one the engine was compiled against.
int sm127_errno_to_bionic(int e);

#endif
