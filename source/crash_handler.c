/* crash_handler.c -- user exception handler.
 *
 * Structure and the important safety property are adapted from
 * fruitninja_nx's nx_exception_dump.c, which has been through many rounds of
 * hardware use. The one thing worth stating loudly, because my first attempt
 * at this file got it wrong:
 *
 *   EVERY READ IS svcQueryMemory-GUARDED.
 *
 * A crash handler that dereferences a pointer without checking it can fault
 * *inside the handler*, and a fault in the exception path produces no output
 * at all -- strictly worse than not having a handler. The stack scan below
 * walks memory that is by definition suspect, so each page is checked before
 * it is touched.
 *
 * What this adds over Atmosphere's own report: the report gives offsets from a
 * module base it detects itself, which is NOT libsts2.so's load_virtbase --
 * converting them needed two crash reports agreeing before the delta could be
 * trusted. The wrapper knows every module's base exactly, so it prints
 * <module>+0x<offset> directly, ready for addr2line with no arithmetic.
 *
 * Handler returns normally at the end so Atmosphere still writes its report;
 * that has the full thread list this does not.
 *
 * MIT license; see LICENSE. */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "imports.h"      // sm127_dump_all_threads
#include "so_util.h"
#include "libc_shim.h"   // sm127_recorded_signal_handler
#include "crash_handler.h"
#include "lock_trace.h"

// libnx runs the handler on this stack, not the faulting thread's -- which may
// be exactly what overflowed. 32 KB matches the Unity port; the recursion in
// the stack scan is bounded but the formatting buffers are not tiny.
alignas(16) u8 __nx_exception_stack[0x8000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

// ---------------------------------------------------------------------------
// managed-code bounds
//
// ILC emits __start___managedcode / __stop___managedcode around all compiled
// C#, and both survive stripping because they are in .dynsym. main() resolves
// them at load and hands them here.
//
// This is the single most valuable classification available for a payload with
// no symbols: inside the range is the GAME's code, outside is the .NET runtime
// or PAL. Run 5's fault sat squarely inside, which turned "a null deref
// somewhere in a 123 MB blob" into "a null reference in managed code during
// module initialisation".
// ---------------------------------------------------------------------------

static uintptr_t s_mc_start, s_mc_stop;

void crash_set_managed_range(uintptr_t start, uintptr_t stop) {
  s_mc_start = start;
  s_mc_stop  = stop;
}

static int xd_is_managed(u64 a) {
  return s_mc_start && a >= s_mc_start && a < s_mc_stop;
}

// ---------------------------------------------------------------------------
// guarded reads
// ---------------------------------------------------------------------------

static int xd_readable(uintptr_t addr, size_t len) {
  if (!addr || addr < 0x1000) return 0;
  uintptr_t a = addr, end = addr + len;
  while (a < end) {
    MemoryInfo mi;
    u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a))) return 0;
    if (mi.type == MemType_Unmapped) return 0;
    if ((mi.perm & Perm_R) == 0) return 0;
    uintptr_t be = (uintptr_t)mi.addr + mi.size;
    if (be <= a) return 0;
    a = be;
  }
  return 1;
}

// "libsts2.so+0x3b73c94" style annotation, falling back to raw hex.
static const char *xd_sym(u64 v, char *buf, size_t n) {
  so_module *m = so_find_module_by_addr((const void *)(uintptr_t)v);
  if (m) {
    snprintf(buf, n, "%s+0x%lx", m->name,
             (unsigned long)(v - (uintptr_t)m->load_virtbase));
    return buf;
  }

  // The WRAPPER is not an so_util module, so it was falling through to a bare
  // hex address -- and it is the one module whose symbols we actually have.
  //
  // Run 29 printed "?+0x46a96f7304" for a crash that was sts2.nro+0x444304,
  // i.e. _free_r+0x84. The base was already in the module map two lines below;
  // it just was not used for symbolisation. Resolving it by hand afterwards is
  // exactly the manual step this handler exists to remove.
  const uintptr_t wb = sm127_wrapper_base();
  if (wb && v >= wb && v < wb + 0x1000000ull) {
    snprintf(buf, n, "sm127_nx.nro+0x%lx", (unsigned long)(v - wb));
    return buf;
  }

  snprintf(buf, n, "%016lx", (unsigned long)v);
  return buf;
}

// ---------------------------------------------------------------------------
// unresolved-import detection
//
// so_resolve poisons an unresolved import with the relocation's own r_offset
// (so_util.c):
//
//     if (taint_missing_imports) *ptr = rels[j].r_offset;
//
// So calling one branches to a small address -- a GOT offset, typically a few
// MB at most, never a mapped code address. That is a very distinctive
// signature, and naming it on sight beats inferring it: run 3's Instruction
// Abort and run 4's Data Abort both needed the log's "unresolved import" lines
// cross-referenced by hand to reach the same conclusion.
// ---------------------------------------------------------------------------

static const char *xd_taint_note(u64 v, int is_pc) {
  if (!v || v >= 0x40000000ull) return "";
  if (!so_find_module_by_addr((const void *)(uintptr_t)v)) {
    return is_pc
      ? "   <- SMALL, UNMAPPED: almost certainly a poisoned unresolved import"
         " (so_resolve writes r_offset). Grep the log for 'unresolved import'."
      : "   <- small value, could be a poisoned import pointer";
  }
  return "";
}

static void xd_dump_range(const char *tag, uintptr_t base, size_t bytes) {
  if (!xd_readable(base, bytes)) {
    debugPrintf("[xd] %s %016lx  <unreadable>\n", tag, (unsigned long)base);
    return;
  }
  char b[96];
  for (size_t off = 0; off < bytes; off += 8) {
    const u64 v = *(const u64 *)(base + off);
    if (so_find_module_by_addr((const void *)(uintptr_t)v))
      debugPrintf("[xd] %s+0x%03lx %016lx  %s\n", tag, (unsigned long)off,
                  (unsigned long)v, xd_sym(v, b, sizeof b));
    else
      debugPrintf("[xd] %s+0x%03lx %016lx\n", tag, (unsigned long)off,
                  (unsigned long)v);
  }
}

// ---------------------------------------------------------------------------
// instruction decode
//
// pvzultimate's handoff makes this a rule, having been burned by skipping it:
//
//   "validate the base before building on it: disassemble the PC and confirm
//    the instruction can actually fault the way the dump says. A `mov` between
//    registers cannot raise a data abort. That one check would have caught
//    this immediately."
//
// Applied to our own crash: I have been reporting "field at offset 0x40 of a
// null object", inferred from x8 == 0 and far == 0x40. But x0 is ALSO 0x40, so
// the fault is equally consistent with `ldr w1, [x0]` where x0 was computed as
// 0x40 upstream -- a different bug with a different cause. Only the
// instruction says which, and it costs four bytes to look.
//
// Decodes the load/store unsigned-offset family, which is what a field access
// compiles to:
//
//   size(2) 111 V 01 opc(2) imm12(12) Rn(5) Rt(5)
//   effective address = Xn + (imm12 << size)
// ---------------------------------------------------------------------------

// Where did the null come from?
//
// Walk backwards from the fault looking for the instruction that last wrote
// the base register. For a static field the pair is:
//
//     adrp xN, <page>
//     ldr  xN, [xN, #<off>]     <- the SLOT
//     ldr  wT, [xN, #0x40]      <- faults when the slot holds null
//
// Recovering the slot address turns "something was null" into a specific
// location in libsts2.so, and reading it confirms whether it is still null --
// which distinguishes "the class constructor never ran" from "it ran and
// stored null".
//
// Sixteen instructions back is enough for the common shapes and short enough
// that a false match is unlikely; the register number has to match exactly.
static void xd_trace_base(u64 pc, unsigned Rn, ThreadExceptionDump *ctx) {
  u64 adrp_target = 0;
  int have_adrp = 0;

  for (int k = 1; k <= 16; k++) {
    const u64 a = pc - (u64)(k * 4);
    if (!xd_readable((uintptr_t)a, 4)) break;
    const uint32_t w = *(const volatile uint32_t *)(uintptr_t)a;

    // ldr xRn, [xRm, #off] -- the load that produced the base
    if ((w & 0xFFC00000u) == 0xF9400000u && (w & 0x1F) == Rn) {
      const unsigned Rm = (w >> 5) & 0x1F;
      const u64 off = (u64)((w >> 10) & 0xFFF) << 3;

      // find the adrp that set Rm, searching further back from here
      for (int j = 1; j <= 8; j++) {
        const u64 b = a - (u64)(j * 4);
        if (!xd_readable((uintptr_t)b, 4)) break;
        const uint32_t v = *(const volatile uint32_t *)(uintptr_t)b;
        if ((v & 0x9F000000u) == 0x90000000u && (v & 0x1F) == Rm) {
          int64_t imm = (int64_t)((((v >> 5) & 0x7FFFFu) << 2) | ((v >> 29) & 0x3u));
          imm = (imm << 43) >> 43;
          adrp_target = (b & ~0xFFFULL) + ((u64)imm << 12);
          have_adrp = 1;
          break;
        }
      }

      // Where the value came from, when it came from another register's
      // object rather than a static slot.
      //
      // Run 22 faulted on `ldr x8, [x9, #0x78]` with x9 = 0x0000003200000001 --
      // two 32-bit halves packed into a pointer slot, loaded from
      // `[x20, #0x8]`. Knowing x9 is garbage is far less useful than seeing
      // the OBJECT it was read out of: whether one field is wrong or the whole
      // allocation is, and whether the bytes look like a freed block, a
      // different struct, or a partially written one.
      if (!have_adrp && Rm < 29) {
        const u64 src = ctx->cpu_gprs[Rm].x + off;
        so_module *sm = so_find_module_by_addr((const void *)(uintptr_t)src);
        debugPrintf("[xd]   x%u came from [x%u + 0x%lx] = %016lx%s\n",
                    Rn, Rm, (unsigned long)off, (unsigned long)src,
                    sm ? "  (inside a module)" : "");

        const u64 obj = ctx->cpu_gprs[Rm].x;
        if (xd_readable((uintptr_t)obj, 0x40)) {
          // Read NOW, not at fault time.
          //
          // The other threads keep running while this handler writes -- the log
          // shows [pthread_mutex_init] and [godot] lines interleaved with these
          // -- so a field can change between the fault and the dump. Run 26 is
          // exactly that: the decode computed x8 + 0x38 = 0x39 from a field
          // reading 1, while far was 0x40, meaning the field held 8 when it
          // actually faulted.
          //
          // Still worth printing: the SHAPE (which fields are pointers, which
          // are small integers, whether the vtable survives) is what identifies
          // the corruption, and that does not change under a race.
          // The C++ class name, from the object's own vtable.
          //
          // Itanium ABI: the typeinfo pointer sits one word BEFORE the first
          // virtual method slot, and typeinfo's second word points at the
          // mangled name. Everything is already relocated at runtime, so this
          // is three loads -- far easier than the offline version, which had
          // to parse R_AARCH64_RELATIVE entries out of a stripped .so.
          //
          // Worth doing in the handler because it has been done by hand three
          // times now: RichTextLabel, Node2D, HBoxContainer. Each took a
          // separate round trip to learn something the process already knew.
          const uint64_t vptr = xd_readable((uintptr_t)obj, 8)
                              ? *(const volatile u64 *)(uintptr_t)obj : 0;
          const char *cls = NULL;
          if (vptr && so_find_module_by_addr((const void *)(uintptr_t)vptr)
              && xd_readable((uintptr_t)vptr - 8, 8)) {
            const uint64_t ti = *(const volatile u64 *)(uintptr_t)(vptr - 8);
            if (ti && xd_readable((uintptr_t)ti + 8, 8)) {
              const uint64_t np = *(const volatile u64 *)(uintptr_t)(ti + 8);
              if (np && xd_readable((uintptr_t)np, 1)) {
                const char *n = (const char *)(uintptr_t)np;
                // Mangled names start with a length, e.g. "13RichTextLabel".
                // Skipping the digits is not a demangler, but it is the whole
                // difference between a readable name and a mangled one for the
                // single-component names these are.
                while (*n >= '0' && *n <= '9') n++;
                cls = n;
              }
            }
          }

          debugPrintf("[xd]   object at x%u = %016lx%s%s%s  (read now, not at "
                      "fault time -- other threads are still running):\n",
                      Rm, (unsigned long)obj,
                      cls ? "  class " : "", cls ? cls : "", cls ? "" : "");
          for (int w = 0; w < 8; w++) {
            const u64 v = *(const volatile u64 *)(uintptr_t)(obj + (u64)w * 8);
            char sb[96];
            debugPrintf("[xd]     +0x%02x %016lx%s%s\n", w * 8, (unsigned long)v,
                        so_find_module_by_addr((const void *)(uintptr_t)v)
                          ? "  " : "",
                        so_find_module_by_addr((const void *)(uintptr_t)v)
                          ? xd_sym(v, sb, sizeof sb) : "");
          }
        } else {
          debugPrintf("[xd]   object at x%u = %016lx is unreadable\n",
                      Rm, (unsigned long)obj);
        }
        return;
      }

      if (have_adrp) {
        const u64 slot = adrp_target + off;
        so_module *m = so_find_module_by_addr((const void *)(uintptr_t)slot);
        debugPrintf("[xd]   x%u was loaded from %s+0x%lx\n", Rn,
                    m ? m->name : "(unmapped)",
                    (unsigned long)(m ? slot - (uintptr_t)m->load_virtbase : slot));
        if (xd_readable((uintptr_t)slot, 8)) {
          const u64 val = *(const volatile u64 *)(uintptr_t)slot;
          debugPrintf("[xd]   that slot currently holds %016lx%s\n",
                      (unsigned long)val,
                      val == 0 ? "   <- still NULL: the initialiser never ran"
                               : "   <- non-null NOW; it was null when read");
        }
      } else {
        debugPrintf("[xd]   x%u loaded from [x%u, #0x%lx]; no adrp found for x%u\n",
                    Rn, Rm, (unsigned long)off, Rm);
      }
      return;
    }

    // mov xRn, xzr -- an explicit null, i.e. deliberate and the bug is upstream
    if (w == (0xAA1F03E0u | Rn)) {
      debugPrintf("[xd]   x%u was set to ZERO explicitly (mov x%u, xzr) at -%d insn\n",
                  Rn, Rn, k);
      return;
    }
  }

  debugPrintf("[xd]   could not find what wrote x%u within 16 instructions\n", Rn);
}

static void xd_decode_at_pc(u64 pc, ThreadExceptionDump *ctx) {
  if (!xd_readable((uintptr_t)pc, 4)) {
    debugPrintf("[xd] instruction at pc: <unreadable>\n");
    return;
  }

  const uint32_t insn = *(const volatile uint32_t *)(uintptr_t)pc;
  debugPrintf("[xd] instruction at pc: %08x\n", insn);

  if (((insn >> 27) & 0x7) == 0x7 && ((insn >> 24) & 0x3) == 0x1) {
    const unsigned size  = (insn >> 30) & 0x3;
    const unsigned V     = (insn >> 26) & 0x1;
    const unsigned opc   = (insn >> 22) & 0x3;
    const unsigned imm12 = (insn >> 10) & 0xFFF;
    const unsigned Rn    = (insn >> 5)  & 0x1F;
    const unsigned Rt    =  insn        & 0x1F;
    const u64 off        = (u64)imm12 << size;
    const u64 base       = (Rn == 31) ? ctx->sp.x : ctx->cpu_gprs[Rn].x;

    debugPrintf("[xd]   %s%s %s%u, [x%u, #0x%lx]\n",
                opc == 0 ? "str" : "ldr", V ? "(simd)" : "",
                size == 3 ? "x" : "w", Rt, Rn, (unsigned long)off);
    debugPrintf("[xd]   x%u = %016lx  + 0x%lx = %016lx%s\n",
                Rn, (unsigned long)base, (unsigned long)off,
                (unsigned long)(base + off),
                (base + off) == ctx->far.x
                  ? "  <- matches far, decode CONFIRMED"
                  : "  <- does NOT match far: either the decode is wrong, or "
                    "the register was reloaded from memory that other threads "
                    "have since changed");

    // A live pointer whose low half was overwritten.
    //
    // Every crash in this arc has produced a value of this exact shape:
    //
    //     0x0000000000000001   0x0000003200000001   0x0000005f00000001
    //     0x0000001000000001   0x0000003400000001
    //
    // The high halves are 0, 16, 50, 52 and 95 -- the same magnitude as the
    // high halves of real heap pointers in those same runs (70, 72, 95), and in
    // run 39 the 95 matched one exactly. So these are not random garbage: they
    // are LIVE POINTERS whose low 32 bits were replaced by a small integer,
    // with the high 32 bits left intact.
    //
    // That narrows the cause to a 32-bit store into the address of a 64-bit
    // pointer field, which is a very different search from "memory is
    // corrupted somewhere". Saying so in the log means the next person does not
    // have to rediscover it by comparing five crashes by hand.
    if ((base >> 32) != 0 && (base >> 32) < 0x10000 && (base & 0xFFFFFFFFu) < 0x10000) {
      debugPrintf("[xd]   *** low 32 bits look OVERWRITTEN: high half %#lx is "
                  "pointer-shaped, low half is %lu.\n"
                  "[xd]   *** This is a live pointer with a 32-bit value stored "
                  "over its low word,\n"
                  "[xd]   *** not random corruption. Look for a 32-bit store to "
                  "a 64-bit field.\n",
                  (unsigned long)(base >> 32), (unsigned long)(base & 0xFFFFFFFFu));
    }

    if (base == 0)
      debugPrintf("[xd]   base register is NULL: a null object or a null static base\n");
    else
      debugPrintf("[xd]   base is NON-NULL: the address is bad, not the object --\n"
                  "[xd]   whatever computed x%u is the thing to chase\n", Rn);

    // Traced in BOTH cases now. It used to run only for a null base, which is
    // backwards: a null base is self-explanatory, and a base holding garbage
    // is the one that needs its provenance chased.
    if (Rn < 29) xd_trace_base(pc, Rn, ctx);
    return;
  }

  debugPrintf("[xd]   not a load/store immediate; disassemble by hand:\n"
              "[xd]   aarch64-none-elf-objdump -d --start-address=0x%lx"
              " --stop-address=0x%lx <module>\n",
              (unsigned long)(pc - 16), (unsigned long)(pc + 16));
}

// ---------------------------------------------------------------------------
// The instruction window before the fault.
//
// If the faulting load's base register is null, the interesting question is
// not "what faulted" but "where did that null come from" -- and the answer is
// almost always two or three instructions earlier. A static base access
// compiles to:
//
//     adrp x8, <page>
//     ldr  x8, [x8, #<off>]      <- x8 loaded from a fixed slot
//     ldr  w1, [x8, #0x40]       <- faults if that slot held null
//
// Decoding the ADRP+LDR pair names the SLOT, as a libsts2.so offset. A null
// there means the type's static storage was never allocated, which is what a
// class constructor that did not run leaves behind.
//
// This is the same reasoning the Dr. Mario port used to find its own managed
// startup failure, minus the part we cannot reuse: it hooked GodotSharp's
// ExceptionUtils.LogUnhandledException at a hardcoded vaddr specific to one
// build of its payload. Reading backwards from the PC needs no offsets and
// survives every rebuild.
// ---------------------------------------------------------------------------

static void xd_window(u64 pc, ThreadExceptionDump *ctx) {
  so_module *m = so_find_module_by_addr((const void *)(uintptr_t)pc);
  const u64 base = m ? (uintptr_t)m->load_virtbase : 0;

  debugPrintf("[xd] --- instructions around pc ---\n");
  for (int k = -8; k <= 2; k++) {
    const u64 a = pc + (u64)(k * 4);
    if (!xd_readable((uintptr_t)a, 4)) continue;
    const uint32_t w = *(const volatile uint32_t *)(uintptr_t)a;

    char note[128];
    note[0] = '\0';

    // ADRP: 1 immlo(2) 10000 immhi(19) Rd(5) -- PC-page relative
    if ((w & 0x9F000000u) == 0x90000000u) {
      const unsigned Rd = w & 0x1F;
      // immhi = bits 23:5 (19 bits, high), immlo = bits 30:29 (2 bits, low).
      // The 21-bit result is signed; 64 - 21 = 43.
      int64_t imm = (int64_t)((((w >> 5) & 0x7FFFFu) << 2) | ((w >> 29) & 0x3u));
      imm = (imm << 43) >> 43;
      const u64 target = ((a & ~0xFFFULL) + ((u64)imm << 12));
      snprintf(note, sizeof note, "  adrp x%u, 0x%lx  (+0x%lx)",
               Rd, (unsigned long)target,
               (unsigned long)(base ? target - base : target));
    }
    // LDR (immediate, unsigned offset), 64-bit
    else if ((w & 0xFFC00000u) == 0xF9400000u) {
      const unsigned Rn = (w >> 5) & 0x1F, Rt = w & 0x1F;
      const u64 off = (u64)((w >> 10) & 0xFFF) << 3;
      snprintf(note, sizeof note, "  ldr x%u, [x%u, #0x%lx]",
               Rt, Rn, (unsigned long)off);
    }
    else if ((w & 0xFFC00000u) == 0xB9400000u) {
      const unsigned Rn = (w >> 5) & 0x1F, Rt = w & 0x1F;
      const u64 off = (u64)((w >> 10) & 0xFFF) << 2;
      snprintf(note, sizeof note, "  ldr w%u, [x%u, #0x%lx]",
               Rt, Rn, (unsigned long)off);
    }
    // CBZ/CBNZ -- a null check the compiler DID emit, worth seeing
    else if ((w & 0x7F000000u) == 0x34000000u)
      snprintf(note, sizeof note, "  cbz  x%u, ...", w & 0x1F);
    else if ((w & 0x7F000000u) == 0x35000000u)
      snprintf(note, sizeof note, "  cbnz x%u, ...", w & 0x1F);
    // BL
    else if ((w & 0xFC000000u) == 0x94000000u) {
      int64_t imm = (int64_t)(w & 0x3FFFFFF);
      imm = (imm << 38) >> 38;
      const u64 target = a + ((u64)imm << 2);
      snprintf(note, sizeof note, "  bl   0x%lx  (+0x%lx)",
               (unsigned long)target,
               (unsigned long)(base ? target - base : target));
    }

    debugPrintf("[xd]   %s+0x%-8lx %08x%s%s\n",
                m ? m->name : "?",
                (unsigned long)(base ? a - base : a), w, note,
                k == 0 ? "   <<<< FAULT" : "");
  }
  (void)ctx;
}


// ---------------------------------------------------------------------------

void __libnx_exception_handler(ThreadExceptionDump *ctx) {
  // Flush first. Everything logged before the fault is the context for it, and
  // a buffered tail lost to the crash is exactly what made runs 1-4 end
  // mid-word.
  debugFlush();

  // RE-ENTRANCY GUARD.
  //
  // If this handler faults while reporting, the kernel calls it again, and it
  // prints the banner again, and faults again. One run produced 11,647
  // identical banner lines and NOTHING else -- no desc, no pc, no backtrace --
  // because the very first dereference after the banner faulted every time.
  //
  // A crash reporter that can loop is worse than one that says little: the
  // loop costs the whole log AND buries the real cause under a megabyte of
  // repetition. Second entry prints one line and stops; third does not print
  // at all.
  static volatile int s_depth;
  if (__atomic_add_fetch(&s_depth, 1, __ATOMIC_SEQ_CST) > 1) {
    if (s_depth == 2) {
      debugPrintf("[xd] FAULT INSIDE THE CRASH HANDLER -- stopping here.\n"
                  "[xd] The report above is incomplete; what printed before\n"
                  "[xd] this line is the last thing that worked.\n");
      debugFlush();
    }
    for (;;) svcSleepThread(1000000000ULL);   // park, do not recurse
  }

  char b1[96], b2[96], b3[96];

  debugPrintf("[xd] ================= USER EXCEPTION =================\n");

  // The pointer BEFORE anything it points at, and flushed.
  //
  // Two runs have now died with the banner printed and the very next line --
  // the first dereference of ctx -- faulting. That is consistent with a bad
  // ctx AND with an exhausted stack, and the two need completely different
  // fixes. Printing the address costs nothing and tells them apart: a
  // plausible stack address means ctx is fine and the stack is not, a null or
  // wild value means the opposite.
  debugPrintf("[xd] ctx=%p\n", (void *)ctx);
  debugFlush();
  if (!ctx) {
    debugPrintf("[xd] ctx is NULL -- nothing further can be reported.\n");
    debugFlush();
    for (;;) svcSleepThread(1000000000ULL);
  }

  debugPrintf("[xd] desc=0x%x  %s\n", ctx->error_desc,
              ctx->error_desc == ThreadExceptionDesc_InstructionAbort
                ? "instruction abort (branched somewhere bad)"
              : ctx->error_desc == ThreadExceptionDesc_MisalignedPC ? "misaligned PC"
              : ctx->error_desc == ThreadExceptionDesc_MisalignedSP ? "misaligned SP"
              : ctx->error_desc == ThreadExceptionDesc_SError       ? "SError"
              : ctx->error_desc == ThreadExceptionDesc_BadSVC       ? "bad SVC"
              : ctx->error_desc == ThreadExceptionDesc_Trap         ? "trap"
              : "data abort / other");

  debugPrintf("[xd] pc=%s%s%s\n", xd_sym(ctx->pc.x, b1, sizeof b1),
              xd_taint_note(ctx->pc.x, 1),
              s_mc_start ? (xd_is_managed(ctx->pc.x)
                              ? "   <- MANAGED (compiled C#)"
                              : "   <- native (runtime/PAL/wrapper)")
                         : "");

  // Why a managed null deref arrives here as a hard fault instead of a
  // NullReferenceException, which is worth stating every time it happens:
  //
  // NativeAOT does not null-check field access. It relies on the PAL
  // installing a SIGSEGV handler that catches the fault and rewrites it into a
  // managed NullReferenceException with a stack trace. This port's sigaction
  // is a stub that returns success without installing anything (libc_shim.c),
  // so the runtime believes it has a handler and does not. Every managed null
  // reference is therefore unrecoverable and silent.
  //
  // Fixing that would turn crashes like this one into an exception type and a
  // managed stack trace, which is a much better place to debug from.
  if (xd_is_managed(ctx->pc.x) && ctx->far.x < 0x1000) {
    debugPrintf("[xd] *** NULL DEREFERENCE IN MANAGED CODE ***\n"
                "[xd]     faulting address 0x%lx -- see the instruction decode\n"
                "[xd]     above for whether that is base+offset or a bad address\n"
                "[xd]     This is a hard fault, not a NullReferenceException,\n"
                "[xd]     because sigaction is stubbed (libc_shim.c) so the\n"
                "[xd]     runtime's SIGSEGV handler was never installed.\n",
                (unsigned long)ctx->far.x);

    void *h = sm127_recorded_signal_handler(11);   // SIGSEGV
    const int fl = sm127_recorded_signal_flags(11);

    if (h) {
      char hb[96];
      debugPrintf("[xd]     Runtime SIGSEGV handler: %s  flags=0x%x%s\n",
                  xd_sym((u64)(uintptr_t)h, hb, sizeof hb), fl,
                  (fl & 4) ? " (SA_SIGINFO)" : "");

      // Whether the handler lives inside the payload decides whether feeding
      // the fault to it is even plausible. Inside means it is the PAL's own
      // handler, which knows how to rewrite a null dereference in managed code
      // into a NullReferenceException. Anywhere else means the address is
      // wrong and reading it is still broken.
      so_module *hm = so_find_module_by_addr(h);
      if (hm && s_mc_start && (uintptr_t)h < s_mc_start)
        debugPrintf("[xd]     -> in %s, below __start___managedcode: this is the\n"
                    "[xd]        PAL's own handler, as expected.\n", hm->name);
      else if (hm)
        debugPrintf("[xd]     -> in %s\n", hm->name);
      else
        debugPrintf("[xd]     -> NOT inside any loaded module. The recorded value\n"
                    "[xd]        is still wrong; check the struct sigaction layout\n"
                    "[xd]        in libc_shim.c against this payload's libc.\n");

      debugPrintf("[xd]     Nothing delivers to it. Doing so would need a\n"
                  "[xd]     synthesised siginfo_t + ucontext_t and a way to\n"
                  "[xd]     resume with the registers the handler rewrote --\n"
                  "[xd]     see docs/RUN_06.md.\n");
    } else {
      debugPrintf("[xd]     No SIGSEGV handler was registered.\n");
    }
  }
  debugPrintf("[xd] lr=%s%s\n", xd_sym(ctx->lr.x, b2, sizeof b2),
              xd_taint_note(ctx->lr.x, 1));
  xd_decode_at_pc(ctx->pc.x, ctx);
  xd_window(ctx->pc.x, ctx);

  debugPrintf("[xd] far=%016lx  <- faulting address%s\n",
              (unsigned long)ctx->far.x,
              ctx->far.x == 0 ? " (NULL deref)" : "");
  debugPrintf("[xd] esr=%08x sp=%016lx fp=%016lx\n", ctx->esr,
              (unsigned long)ctx->sp.x, (unsigned long)ctx->fp.x);

  // Module map. Printed every time because the bases move each boot -- an
  // offset is only meaningful next to the base it came from.
  debugPrintf("[xd] --- modules ---\n");
  // The wrapper is not an so_util module, so it never appeared in this map --
  // yet half of every hang dump is wrapper frames. Its real base comes from
  // the NRO header (util.c), so these offsets work with addr2line -e sm127.elf.
  debugPrintf("[xd]   %-40s %016lx  (sm127_nx.nro)\n", "sm127 wrapper",
              (unsigned long)sm127_wrapper_base());
  for (so_module *m = so_get_list(); m; m = m->next)
    debugPrintf("[xd]   %-40s %016lx + %lu KB\n", m->name,
                (unsigned long)(uintptr_t)m->load_virtbase,
                (unsigned long)(m->load_size >> 10));

  debugPrintf("[xd] --- registers ---\n");
  for (int i = 0; i < 28; i += 2) {
    debugPrintf("[xd]   x%-2d %016lx %-28s  x%-2d %016lx %s\n",
                i,     (unsigned long)ctx->cpu_gprs[i].x,
                xd_sym(ctx->cpu_gprs[i].x, b1, sizeof b1),
                i + 1, (unsigned long)ctx->cpu_gprs[i + 1].x,
                xd_sym(ctx->cpu_gprs[i + 1].x, b2, sizeof b2));
  }
  debugPrintf("[xd]   x28 %016lx %s\n", (unsigned long)ctx->cpu_gprs[28].x,
              xd_sym(ctx->cpu_gprs[28].x, b1, sizeof b1));

  // Frame-pointer backtrace. Works for wrapper and engine frames; NOT for the
  // AOT payload -- nativeaot_shim.c records that ILC does not preserve frame
  // pointers there, which is why the stack scan below exists as well.
  debugPrintf("[xd] --- fp backtrace ---\n");
  uintptr_t fp = (uintptr_t)ctx->fp.x;
  int depth = 0;
  for (; depth < 16 && fp; depth++) {
    if (!xd_readable(fp, 16)) break;
    const uintptr_t nfp = ((const uintptr_t *)fp)[0];
    const uintptr_t rlr = ((const uintptr_t *)fp)[1];
    if (!rlr) break;
    debugPrintf("[xd]   bt[%d] %s%s\n", depth, xd_sym(rlr, b3, sizeof b3),
                s_mc_start ? (xd_is_managed(rlr) ? "  [managed]" : "  [native]") : "");
    if (nfp <= fp) break;
    fp = nfp;
  }
  if (!depth)
    debugPrintf("[xd]   (no frame chain -- expected inside libsts2.so)\n");

  // Stack scan. This is how Atmosphere builds its own traces, and it is the
  // only thing that works without frame pointers: every word in a window above
  // SP that lands inside a loaded module is a candidate return address. Some
  // are data that happens to look like one; the sequence is still the most
  // useful artifact available for a payload with no symbols.
  debugPrintf("[xd] --- stack scan (candidate return addresses) ---\n");
  const uintptr_t sp = (uintptr_t)ctx->sp.x;
  int hits = 0;
  for (uintptr_t a = sp; a < sp + 0x4000 && hits < 64; a += 8) {
    if (!xd_readable(a, 8)) break;
    const u64 v = *(const u64 *)a;
    so_module *m = so_find_module_by_addr((const void *)(uintptr_t)v);
    if (!m) continue;
    debugPrintf("[xd]   [sp+0x%04lx] %s\n", (unsigned long)(a - sp),
                xd_sym(v, b1, sizeof b1));
    hits++;
  }
  if (!hits)
    debugPrintf("[xd]   (nothing found -- SP may be invalid)\n");

  // The frame itself, for hand-decoding once a function is identified.
  debugPrintf("[xd] --- stack frame ---\n");
  xd_dump_range("SP", sp, 0x100);

  // Every other thread, not just this one.
  //
  // The crash this matters for is intermittent and looks like a race: a live
  // object (valid vtable) with a single field holding 1 and the rest zeroed,
  // read after a null check that 1 passes. One thread's stack cannot show
  // that -- the question is what the OTHER thread was doing to the same object
  // at the same instant.
  //
  // Deliberately last. If pausing threads goes wrong, everything above is
  // already written and flushed, so a hang here costs the extra detail and
  // nothing that was already known.
  // Was a shader being compiled anywhere near this?
  //
  // The distinction that matters: a MISS is real compilation (NAK, ralloc, the
  // whole compiler), a LOAD is a cache hit that compiles nothing. If the fault
  // follows misses closely, moving compilation earlier is worth doing. If it
  // follows loads just as closely, the trigger is pipeline creation and
  // precompiling would not help.
  {
    extern int s_frames_done_public;
    const uint32_t f = (uint32_t)s_frames_done_public;
    debugPrintf("[xd] --- shader activity ---\n");
    debugPrintf("[xd]   compiles (cache MISS): %lu, last at frame %lu\n",
                (unsigned long)sm127_shader_compiles,
                (unsigned long)sm127_last_compile_frame);
    debugPrintf("[xd]   loads    (cache hit) : %lu, last at frame %lu\n",
                (unsigned long)sm127_shader_loads,
                (unsigned long)sm127_last_load_frame);
    debugPrintf("[xd]   current frame: %lu\n", (unsigned long)f);
    if (sm127_shader_compiles && f >= sm127_last_compile_frame)
      debugPrintf("[xd]   -> %lu frame(s) since the last COMPILE%s\n",
                  (unsigned long)(f - sm127_last_compile_frame),
                  (f - sm127_last_compile_frame) <= 5
                    ? "  *** close enough to be the trigger ***" : "");
  }

  debugPrintf("[xd] --- all threads at fault time ---\n");
  debugFlush();
  sm127_dump_all_threads();

  debugPrintf("[xd] ==================================================\n");
  // Slot history, last.
  //
  // Printed after the register and stack dump because it is long, and because
  // the fault itself is what a reader wants first. But it goes in the same
  // report: the values rejected on hardware were Godot UTF-32 String data
  // sitting in cond slots, which means a lock's storage was freed and reused.
  // When the crash IS that reuse being dereferenced, the transitions leading
  // up to it are the only record of who wrote the slot and when.
  if (config.lock_trace && config.lock_trace_dump > 0)
    lock_trace_dump_recent((unsigned)config.lock_trace_dump);

  debugPrintf("[xd] Paste any <module>+0x<offset> straight into addr2line.\n");
  debugFlush();

  // Return, so Atmosphere still produces its report -- it carries the thread
  // list and register state for every thread, which this does not.
}
