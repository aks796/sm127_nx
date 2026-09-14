/* patch.c -- game-specific byte-level patches for libgodot_android.so.
 *
 * Carried over from smwr_nx/smbr_nx as infrastructure only: this game ships
 * a different engine build (Godot 4.3 arm64-v8a, this APK; the smwr_nx table
 * below was for a Godot 4.6.dev4 build) so any vaddr/opcode entry from that
 * project is NOT valid here and must not be reused blindly. The real,
 * version-independent fix for the crash the smwr_nx entry worked around
 * (DisplayServerAndroid calling get_godot_view()->can_capture_pointer() on a
 * NULL view during setup2) is getRenderView() returning a non-null fake
 * singleton in jni_fake.c -- keep that; this table should stay empty unless
 * hardware testing finds a crash that specifically needs a binary hook, in
 * which case: dump bytes at the real vaddr in *this* .so with objdump/
 * addr2line before adding an entry (each entry self-disables if the bytes at
 * vaddr don't match `expect`, so a stale entry is inert, never wrong).
 * MIT license; see LICENSE. */

#include <stdint.h>
#include <string.h>

#include "patch.h"
#include "so_util.h"
#include "util.h"

typedef struct { uint32_t vaddr_word0; uint32_t expect; uintptr_t vaddr; void *repl; const char *name; } GamePatch;

void so_patch(so_module *mod) {
  static const GamePatch patches[] = {
    // (empty until hardware testing identifies a real need -- see file header)
  };

  for (unsigned i = 0; i < sizeof(patches) / sizeof(*patches); i++) {
    const GamePatch *p = &patches[i];
    // hooks are written into the RW backing (load_base) before so_finalize
    uint32_t *insn = (uint32_t *)((uintptr_t)mod->load_base + p->vaddr);
    if (*insn != p->expect) {
      debugPrintf("[patch] %s: unexpected bytes %08x at 0x%lx, skipping\n",
                  p->name, *insn, (unsigned long)p->vaddr);
      continue;
    }
    hook_arm64((uintptr_t)insn, (uintptr_t)p->repl);
    debugPrintf("[patch] %s hooked at vaddr 0x%lx\n", p->name, (unsigned long)p->vaddr);
  }
}
