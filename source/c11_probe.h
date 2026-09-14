/* c11_probe.h -- see c11_probe.c.
 *
 * MIT license; see LICENSE. */

#ifndef __C11_PROBE_H__
#define __C11_PROBE_H__

// Checks that mtx_*, cnd_timedwait and call_once actually behave, and names
// any that do not. Mesa's shader compiler and nvkmd-switch both depend on
// them, and nvkmd-switch carries explicit "mtx_init failed" / "cnd_init
// failed" messages -- so the author expected these to be reachable.
//
// Single-threaded and bounded: a probe that hangs at boot would be worse than
// the question it answers.
void sm127_c11_probe(void);

#endif
