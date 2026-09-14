/* lock_trace.h -- see lock_trace.c.
 *
 * Records every write to a pthread lazy slot into an in-memory ring, and
 * prints it only when something has already gone wrong. Logging each
 * transition as it happened would both cost an SD write per event and shift
 * the timing of the use-after-free being chased.
 *
 * MIT license; see LICENSE. */

#ifndef __LOCK_TRACE_H__
#define __LOCK_TRACE_H__

#include <stddef.h>
#include <stdint.h>

#define LOCK_OP_INIT        1   /* pthread_*_init stored a fresh object      */
#define LOCK_OP_ENSURE_HIT  2   /* lazy path found a plausible pointer       */
#define LOCK_OP_CAS_WIN     3   /* we installed the object                   */
#define LOCK_OP_CAS_LOSE    4   /* another thread installed first            */
#define LOCK_OP_REJECT      5   /* slot held something impossible            */
#define LOCK_OP_DESTROY     6   /* pthread_*_destroy cleared it              */

#define LOCK_KIND_MUTEX     1
#define LOCK_KIND_COND      2
#define LOCK_KIND_RWLOCK    3

/* Hot path: an atomic increment and eight stores. No allocation, no lock --
 * this runs inside the mutex implementation, so taking one would deadlock. */
void lock_trace_record(uint8_t op, uint8_t kind, const void *slot,
                       uint64_t old_val, uint64_t new_val, uint64_t lr);

/* The engine call site, set on entry by each pthread_*_fake shim in imports.c.
 *
 * NOT __builtin_return_address(0) at the record site: the recorders are static
 * inline helpers, and once folded into their callers the builtin returned
 * addresses in no loaded module at all. The entry shims are called directly
 * through the engine's import table, so their return address is real.
 *
 * Stored in a table keyed on armGetTls() rather than __thread -- ELF TLS is
 * unused elsewhere in this port and the game thread comes from threadCreate,
 * so its availability here is an assumption. A register read is not. */
uint64_t sm127_lock_caller_get(void);

#define LOCK_TRACE(op, kind, slot, oldv, newv) \
  lock_trace_record((op), (kind), (slot), (uint64_t)(oldv), (uint64_t)(newv), \
                    sm127_lock_caller_get())

/* Every recorded event for one slot, oldest first. Says so explicitly when
 * there are none -- "we never wrote this slot" is itself the finding. */
void lock_trace_dump_slot(const void *slot, const char *why);

/* The last n events across all slots. For the crash handler. */
void lock_trace_dump_recent(unsigned n);

/* 128 bytes around the slot, svcQueryMemory-guarded, with the slot marked. */
void lock_trace_hexdump_around(const void *slot);

/* What a 64-bit slot value looks like: memset fill, two char32_t (Godot
 * String), ASCII text, exactly 1, a 32-bit value, misaligned. */
void lock_trace_describe_value(uint64_t v, char *out, size_t n);

/* One line: how many transitions were recorded. */
void lock_trace_report(void);

#endif
