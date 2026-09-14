/* crash_handler.h -- user exception handler. See crash_handler.c.
 *
 * Adapted from fruitninja_nx's nx_exception_dump.c. Purely additive: it writes
 * a symbolized dump into sm127_debug.log and returns, so Atmosphere's own crash
 * report is still produced exactly as before.
 *
 * There is deliberately no init call and nothing to register. Addresses resolve
 * through so_find_module_by_addr, which walks so_util's own module list, so
 * every module the port loads is covered automatically -- including any added
 * later, which a hand-maintained registry would silently miss.
 *
 * libnx finds __libnx_exception_handler by name; it is not declared here
 * because its signature uses libnx's ThreadExceptionDump, and pulling
 * <switch.h> into every translation unit that includes this header is not
 * worth it. This header exists so the file is visibly part of the build.
 *
 * MIT license; see LICENSE. */

#ifndef __CRASH_HANDLER_H__
#define __CRASH_HANDLER_H__

#include <stdint.h>

// ILC's __start___managedcode / __stop___managedcode, resolved from the
// payload's .dynsym by main(). Lets the handler say whether a fault is in the
// game's compiled C# or in the .NET runtime -- the most useful single fact
// available when the payload has no symbols.
void crash_set_managed_range(uintptr_t start, uintptr_t stop);

#endif
