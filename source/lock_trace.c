/* lock_trace.c -- every write to a pthread lazy slot, kept in memory.
 *
 * WHY A RING BUFFER AND NOT debugPrintf
 * =====================================
 * The obvious implementation logs each transition as it happens. That does not
 * work here, for two separate reasons, both already demonstrated by this port:
 *
 *   1. Cost. Per-line flushing stays on until frame 8, so during boot every
 *      log line is a synchronous SD card write. pthread_mutex_init tracing
 *      alone once produced 4041 of 6306 lines and pinned two cores. Slot
 *      transitions are rarer than that but still frequent enough to do the
 *      same.
 *
 *   2. It would hide the bug. The values being chased -- Godot UTF-32 string
 *      data sitting in a cond slot -- come from a use-after-free, and a
 *      use-after-free is a race against whoever reuses the memory. Adding a
 *      millisecond of SD I/O to every transition changes that timing. The
 *      instrument would perturb the thing it measures, and the most likely
 *      outcome is a bug that stops reproducing without being fixed.
 *
 * So every transition is recorded into a fixed in-memory ring -- an atomic
 * increment and eight stores, no allocation, no lock -- and the ring is
 * PRINTED only when something has already gone wrong: a rejected slot, or the
 * crash handler. At that point the cost does not matter and the history is
 * exactly what is needed.
 *
 * NO LOCKS IN HERE, EVER. This code runs inside the mutex/cond implementation.
 * Taking a lock to record a lock transition would deadlock on the first call.
 *
 * MIT license; see LICENSE. */

#include <string.h>
#include <stdio.h>
#include <switch.h>

#include "lock_trace.h"
#include "util.h"
#include "config.h"
#include "so_util.h"

// Power of two: the index wrap is a mask, which keeps the hot path to an
// atomic add and an AND.
#define RING_SLOTS 1024u
#define RING_MASK  (RING_SLOTS - 1u)

typedef struct {
  uint64_t tick;      // svcGetSystemTick at record time
  uint64_t slot;      // address of the 8-byte lazy slot
  uint64_t old_val;   // what it held before
  uint64_t new_val;   // what it holds after
  uint64_t lr;        // caller, for attributing the write
  uint32_t tid;       // which thread
  uint8_t  op;
  uint8_t  kind;
} lock_ev;

// ---------------------------------------------------------------------------
// PER-SLOT history
//
// The global ring alone cannot answer "was this slot ever ours". A hardware
// run recorded 39,846,084 transitions through 1024 entries -- 38,912 wraps --
// so every slot reported "NO PRIOR EVENTS" regardless of its actual past. The
// history was overwritten microseconds after being written, and the one line
// the whole mechanism existed to produce was reduced to noise.
//
// This table is keyed on the slot address, so a slot keeps ITS events no
// matter how much unrelated traffic passes. ENSURE_HIT is deliberately NOT
// stored here -- it is the normal case and it is what produced the 39M. It is
// counted instead, which carries the same information ("this slot was in
// healthy use N times") in one integer rather than N records.
//
// Collisions overwrite. The stored address is checked on read, so a collision
// costs history, never a wrong answer attributed to the wrong slot.
// ---------------------------------------------------------------------------
#define SLOT_BUCKETS 512u
#define SLOT_DEPTH   4u

typedef struct {
  uint64_t slot;                 // 0 = empty
  uint32_t hits;                 // ENSURE_HIT count, not stored individually
  uint32_t n;                    // state-changing events recorded
  lock_ev  ev[SLOT_DEPTH];       // most recent first
} slot_hist;

static slot_hist g_slots[SLOT_BUCKETS];

static inline uint32_t slot_bucket(uint64_t slot) {
  // Slots are 8-aligned, so the low three bits carry nothing. Fold high bits
  // in as well: bionic lock storage clusters, and the raw address alone puts
  // neighbours in adjacent buckets.
  uint64_t h = slot >> 3;
  h ^= h >> 17;
  h *= 0x9E3779B185EBCA87ull;
  h ^= h >> 29;
  return (uint32_t)(h & (SLOT_BUCKETS - 1u));
}

static lock_ev  g_ring[RING_SLOTS];
static uint32_t g_seq;          // total events ever; index is g_seq & RING_MASK
static uint32_t g_dropped;      // recorded while a dump was in progress

// Set while dumping. Recording continues (dropping instead of blocking) so a
// dump cannot deadlock against a thread mid-transition.
static volatile int g_dumping;

static const char *op_name(uint8_t op) {
  switch (op) {
    case LOCK_OP_INIT:       return "init";
    case LOCK_OP_ENSURE_HIT: return "ensure-hit";
    case LOCK_OP_CAS_WIN:    return "cas-win";
    case LOCK_OP_CAS_LOSE:   return "cas-lose";
    case LOCK_OP_REJECT:     return "REJECT";
    case LOCK_OP_DESTROY:    return "destroy";
    default:                 return "?";
  }
}

static const char *kind_name(uint8_t k) {
  switch (k) {
    case LOCK_KIND_MUTEX:  return "mutex";
    case LOCK_KIND_COND:   return "cond";
    case LOCK_KIND_RWLOCK: return "rwlock";
    default:               return "?";
  }
}

void lock_trace_record(uint8_t op, uint8_t kind, const void *slot,
                       uint64_t old_val, uint64_t new_val, uint64_t lr) {
  if (!config.lock_trace) return;
  if (g_dumping) { __atomic_add_fetch(&g_dropped, 1, __ATOMIC_RELAXED); return; }

  // Per-slot first.
  {
    slot_hist *h = &g_slots[slot_bucket((uint64_t)(uintptr_t)slot)];
    const uint64_t owner = __atomic_load_n(&h->slot, __ATOMIC_RELAXED);
    if (owner != (uint64_t)(uintptr_t)slot) {
      // New occupant. Reset rather than inherit a stranger's counts.
      __atomic_store_n(&h->slot, (uint64_t)(uintptr_t)slot, __ATOMIC_RELAXED);
      h->hits = 0;
      h->n = 0;
    }
    if (op == LOCK_OP_ENSURE_HIT) {
      h->hits++;                       // counted, not stored
    } else {
      // Shift down; index 0 is the most recent.
      for (uint32_t k = SLOT_DEPTH - 1; k > 0; k--) h->ev[k] = h->ev[k - 1];
      h->ev[0].tick    = armGetSystemTick();
      h->ev[0].slot    = (uint64_t)(uintptr_t)slot;
      h->ev[0].old_val = old_val;
      h->ev[0].new_val = new_val;
      h->ev[0].lr      = lr;
      h->ev[0].tid     = (uint32_t)((uintptr_t)armGetTls() >> 8);
      h->ev[0].op      = op;
      h->ev[0].kind    = kind;
      if (h->n < SLOT_DEPTH) h->n++;
    }
  }

  const uint32_t i = __atomic_fetch_add(&g_seq, 1, __ATOMIC_RELAXED) & RING_MASK;
  lock_ev *e = &g_ring[i];
  e->tick    = armGetSystemTick();
  e->slot    = (uint64_t)(uintptr_t)slot;
  e->old_val = old_val;
  e->new_val = new_val;
  e->lr      = lr;
  // armGetTls(), not a process handle and not svcGetThreadId.
  //
  // The process handle is the same on every thread, so it would label every
  // event identically -- useless for a race. svcGetThreadId is correct but is
  // a syscall, and this sits inside the mutex path. The TLS pointer is a
  // register read, unique per thread, and stable for its lifetime: exactly
  // what is needed to tell two threads apart.
  e->tid     = (uint32_t)((uintptr_t)armGetTls() >> 8);
  e->op      = op;
  e->kind    = kind;
}

// ---------------------------------------------------------------------------
// value shape
//
// The rejected values from hardware were not random. Decoded:
//
//   0101010101010101   a memset fill of 0x01
//   000a64657a696c61   "alized\n" -- ASCII text
//   0000007400000070   U+0070 U+0074  'pt'
//   0000006c00000061   U+0061 U+006C  'al'
//   0000006100000062   U+0062 U+0061  'ba'
//   0000002000000020   U+0020 U+0020  two spaces
//
// Pairs of small codepoints in the two halves of a 64-bit word are char32_t,
// and char32_t is exactly what Godot's String stores. So the memory was a
// cond and is now string data -- a use-after-free, not a wild write.
//
// Naming that shape in the log is the difference between "slot rejected" and
// "a Godot String is living in this cond".
// ---------------------------------------------------------------------------

void lock_trace_describe_value(uint64_t v, char *out, size_t n) {
  if (!out || n < 8) return;
  out[0] = 0;

  if (v == 0) { snprintf(out, n, "zero (uninitialised)"); return; }

  // uniform byte fill
  const uint8_t b0 = (uint8_t)(v & 0xff);
  int uniform = 1;
  for (int i = 1; i < 8; i++) if (((v >> (i * 8)) & 0xff) != b0) { uniform = 0; break; }
  if (uniform) {
    // 0xDE is ours: poison_free memsets every freed block to it. Naming it
    // matters, because "memset fill of 0xde" and "this memory was FREED by us
    // and handed back out" are the same fact stated at different levels of
    // usefulness -- and the second one is the whole reason poison exists.
    if (b0 == 0xDE) {
      snprintf(out, n, "POISON (0xDE) -- freed by us, then reallocated. "
                       "Legitimate reuse if this is first use; a "
                       "use-after-free if it is not.");
      return;
    }
    snprintf(out, n, "memset fill of 0x%02x", b0);
    return;
  }

  const uint32_t lo = (uint32_t)(v & 0xffffffffu);
  const uint32_t hi = (uint32_t)(v >> 32);

  // two char32_t -- Godot String data
  const int lo_ch = (lo == 0) || (lo >= 0x20 && lo < 0x110000);
  const int hi_ch = (hi == 0) || (hi >= 0x20 && hi < 0x110000);
  if (lo_ch && hi_ch && (lo < 0x10000 && hi < 0x10000) && (lo || hi)) {
    const char a = (lo >= 0x20 && lo < 0x7f) ? (char)lo : '.';
    const char b = (hi >= 0x20 && hi < 0x7f) ? (char)hi : '.';
    snprintf(out, n, "TWO char32_t: U+%04X U+%04X '%c%c' -- Godot String data",
             (unsigned)lo, (unsigned)hi, a, b);
    return;
  }

  // Printable ASCII bytes.
  //
  // NULs are counted only to be allowed through -- a short string in an 8-byte
  // word is NUL-padded, so rejecting them would miss exactly the case this
  // detects. The earlier version kept a `nul` tally it never read, which is
  // what -Wunused-but-set-variable was pointing at: not a stray declaration
  // but a condition that was never written.
  int printable = 0;
  for (int i = 0; i < 8; i++) {
    const uint8_t c = (uint8_t)((v >> (i * 8)) & 0xff);
    if (c == 0) continue;                       // NUL padding: allowed
    if (c >= 0x20 && c < 0x7f) { printable++; continue; }
    if (c == '\n' || c == '\t' || c == '\r') continue;   // "alized\n" had one
    printable = -100;                           // a byte no text contains
    break;
  }
  if (printable >= 4) {
    char txt[9];
    for (int i = 0; i < 8; i++) {
      const uint8_t c = (uint8_t)((v >> (i * 8)) & 0xff);
      txt[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
    }
    txt[8] = 0;
    snprintf(out, n, "ASCII TEXT \"%s\" -- string data in a lock slot", txt);
    return;
  }

  if (v == 1) { snprintf(out, n, "EXACTLY 1 -- the crash signature"); return; }
  if (v < 0x10000) { snprintf(out, n, "small integer %llu", (unsigned long long)v); return; }
  if (hi == 0) { snprintf(out, n, "32-bit value 0x%08x in a 64-bit slot", (unsigned)lo); return; }
  if (v & 7u) { snprintf(out, n, "misaligned (low bits %u) -- cannot be from calloc",
                         (unsigned)(v & 7u)); return; }

  // ASK, do not assume.
  //
  // This used to end with a flat "aligned, but outside the newlib heap",
  // reached whenever nothing else matched -- without checking the heap at all.
  // On hardware that printed against 64 ENSURE-HIT events, which by definition
  // are pointers looks_like_our_heap_ptr() ACCEPTED. The log asserted 64 valid
  // pointers were outside the heap, in the middle of a hunt for pointers that
  // really are.
  //
  // A diagnostic that states the opposite of what the code decided is worse
  // than one that says nothing.
  extern char *fake_heap_start, *fake_heap_end;
  const uint64_t hs = (uint64_t)(uintptr_t)fake_heap_start;
  const uint64_t he = (uint64_t)(uintptr_t)fake_heap_end;

  if (v >= hs && v < he) {
    snprintf(out, n, "aligned heap pointer (inside %p..%p)",
             (void *)fake_heap_start, (void *)fake_heap_end);
    return;
  }

  // Outside the ORIGINAL range is not the same as outside the heap: those
  // bounds are fixed at __libnx_initheap and the heap grows. Ask the kernel,
  // exactly as the guard does before rejecting.
  MemoryInfo mi; u32 pg;
  if (R_SUCCEEDED(svcQueryMemory(&mi, &pg, (u64)v)) && mi.type == MemType_Heap &&
      (mi.perm & (Perm_R | Perm_W)) == (Perm_R | Perm_W)) {
    snprintf(out, n, "aligned heap pointer (outside the initial range, "
                     "kernel says type=0x%x)", (unsigned)mi.type);
    return;
  }

  snprintf(out, n, "aligned, and the kernel does not call it heap");
}

// ---------------------------------------------------------------------------
// dumps
// ---------------------------------------------------------------------------

static void dump_one(const lock_ev *e, uint64_t now) {
  char what[96];
  lock_trace_describe_value(e->new_val, what, sizeof(what));

  // Ticks are 19.2 MHz on this hardware; report ms before the dump so the
  // ordering is readable without arithmetic.
  const double ms = now > e->tick ? (double)(now - e->tick) / 19200.0 : 0.0;

  char lr[128] = "";
  if (e->lr) {
    so_module *m = so_find_module_by_addr((const void *)(uintptr_t)e->lr);
    if (m) snprintf(lr, sizeof(lr), "  from %s+0x%lx", m->name,
                    (unsigned long)((uintptr_t)e->lr - (uintptr_t)m->load_virtbase));
    else   snprintf(lr, sizeof(lr), "  from 0x%llx", (unsigned long long)e->lr);
  }

  debugPrintf("[lock]  -%8.2f ms  %-10s %-6s slot=0x%llx\n"
              "[lock]              %016llx -> %016llx  tid=%u%s\n",
              ms, op_name(e->op), kind_name(e->kind),
              (unsigned long long)e->slot,
              (unsigned long long)e->old_val, (unsigned long long)e->new_val,
              (unsigned)e->tid, lr);
  if (e->new_val) debugPrintf("[lock]              new: %s\n", what);
}

void lock_trace_dump_slot(const void *slot, const char *why) {
  if (!config.lock_trace) return;
  g_dumping = 1;

  const uint64_t target = (uint64_t)(uintptr_t)slot;
  const uint64_t now = armGetSystemTick();
  // No ring bookkeeping here any more -- this reads the per-slot table only.

  debugPrintf("[lock] ===== history for slot 0x%llx (%s) =====\n",
              (unsigned long long)target, why ? why : "");

  // The per-slot table, NOT the ring. The ring holds the last 1024 events
  // across every slot, which at ~40M transitions per session is a window of
  // microseconds -- it answers "what just happened", never "what happened to
  // this slot".
  int found = 0;
  slot_hist *h = &g_slots[slot_bucket(target)];
  if (__atomic_load_n(&h->slot, __ATOMIC_RELAXED) == target) {
    for (uint32_t k = 0; k < h->n && k < SLOT_DEPTH; k++) {
      dump_one(&h->ev[k], now);       // most recent first
      found++;
    }
    if (h->hits)
      debugPrintf("[lock]   plus %u successful ensure-hit(s) on this slot --\n"
                  "[lock]   it WAS in healthy use before this.\n", h->hits);
  }

  // No ring scan here. Both tables are written in the same call, so the ring
  // can never hold an event the per-slot table lacks -- scanning it would
  // simply count every event twice and report a number that is wrong in a way
  // no reader could detect.

  // BENIGN vs HARMFUL -- the distinction the per-slot table exists for.
  //
  // No record means this is the FIRST use of that storage. The garbage in it
  // was never ours, we reject it, allocate a real object and CAS it in. That
  // is the mechanism working. Hardware confirms the source: the callers are
  // libfmod.so and libfmodstudio.so, which carve locks out of their own pools,
  // and a pool filled with 0x01 gives 0x0101010101010101 where bionic's static
  // initialiser would have left zero.
  //
  // A record is the opposite. It means we DID store a live object here and
  // something overwrote it -- so a thread may already be waiting on the object
  // we are about to replace, and will not be woken. That is a real lost-wakeup
  // bug and it should be impossible to miss in a log.
  if (!found) {
    debugPrintf("[lock]   first use of this storage (no record of it, and the\n"
                "[lock]   per-slot table survives the transition flood, so this\n"
                "[lock]   is not history that aged out). Rejecting the garbage\n"
                "[lock]   and initialising is correct here.\n");
  } else {
    debugPrintf("[lock]   *** %d recorded event(s): WE OWNED THIS SLOT AND IT\n"
                "[lock]   *** WAS OVERWRITTEN. Any thread waiting on the object\n"
                "[lock]   *** we stored will not be woken. This is the harmful\n"
                "[lock]   *** case, not first-use noise.\n", found);
  }

  g_dumping = 0;
}

void lock_trace_dump_recent(unsigned n) {
  if (!config.lock_trace) return;
  g_dumping = 1;

  const uint64_t now = armGetSystemTick();
  const uint32_t total = __atomic_load_n(&g_seq, __ATOMIC_RELAXED);
  const uint32_t have = total < RING_SLOTS ? total : RING_SLOTS;
  const uint32_t want = n < have ? n : have;

  debugPrintf("[lock] ===== last %u of %u slot transitions =====\n",
              (unsigned)want, (unsigned)total);
  for (uint32_t k = 0; k < want; k++) {
    const uint32_t i = (total - want + k) & RING_MASK;
    dump_one(&g_ring[i], now);
  }
  if (g_dropped)
    debugPrintf("[lock]   %u event(s) dropped during dumps\n", (unsigned)g_dropped);

  g_dumping = 0;
}

void lock_trace_hexdump_around(const void *slot) {
  if (!slot) return;
  const uintptr_t base = ((uintptr_t)slot & ~(uintptr_t)15) - 32;

  // What the kernel says this memory IS.
  //
  // looks_like_our_heap_ptr accepts heap-typed, readable, writable memory and
  // deliberately ignores attributes -- the bug it was fixing was
  // over-rejection, so extra conditions were the wrong direction. Reporting
  // type/perm/attr here is what makes that decision reviewable: if a rejection
  // ever correlates with an attribute, this line says so instead of the guard
  // silently guessing.
  {
    MemoryInfo mi; u32 pg;
    if (R_SUCCEEDED(svcQueryMemory(&mi, &pg, (u64)(uintptr_t)slot))) {
      debugPrintf("[lock]   region: 0x%lx +0x%lx  type=0x%x perm=0x%x attr=0x%x\n",
                  (unsigned long)mi.addr, (unsigned long)mi.size,
                  (unsigned)mi.type, (unsigned)mi.perm, (unsigned)mi.attr);
    } else {
      debugPrintf("[lock]   region: svcQueryMemory FAILED -- address is not mapped\n");
    }
  }

  debugPrintf("[lock]   memory around the slot (slot marked <=):\n");
  for (int row = 0; row < 8; row++) {
    const uintptr_t a = base + (uintptr_t)row * 16;

    // Guarded: the whole point is that this address may be nonsense.
    MemoryInfo mi; u32 pg;
    if (R_FAILED(svcQueryMemory(&mi, &pg, a)) ||
        mi.perm == Perm_None || mi.type == MemType_Unmapped) {
      debugPrintf("[lock]   %016lx  <unmapped>\n", (unsigned long)a);
      continue;
    }

    const uint8_t *p = (const uint8_t *)a;
    char hex[64], asc[24];
    for (int i = 0; i < 16; i++) {
      snprintf(hex + i * 3, 4, "%02x ", p[i]);
      asc[i] = (p[i] >= 0x20 && p[i] < 0x7f) ? (char)p[i] : '.';
    }
    asc[16] = 0;
    const int here = (a <= (uintptr_t)slot && (uintptr_t)slot < a + 16);
    debugPrintf("[lock]   %016lx  %s |%s|%s\n", (unsigned long)a, hex, asc,
                here ? "  <=" : "");
  }
}

void lock_trace_report(void) {
  debugPrintf("[lock] %u slot transition(s) recorded%s\n",
              (unsigned)__atomic_load_n(&g_seq, __ATOMIC_RELAXED),
              config.lock_trace ? "" : " (tracing OFF -- set lock_trace 1)");
}
