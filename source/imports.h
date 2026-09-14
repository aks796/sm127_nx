/* imports.h -- libgodot_android.so import resolution. MIT license; see LICENSE. */

#ifndef __IMPORTS_H__
#define __IMPORTS_H__

#include "so_util.h"

void sm127_resolve_imports(so_module *mod);

// Resolves a symbol in the engine module (libgodot_android.so). Used by
// jni_helpers.c / nx_input.c for GodotLib.* entry points, which are looked up
// lazily because jni_fake is initialised before the module loads. MUST be
// called before so_finalize(&game_mod) -- see imports.c. main.c calls it from
// resolve_entry_points().
void sm127_cache_engine_symbols(void);

// Reads the cache populated above. Returns 0 (with a log line) for anything
// not on the cached list.
uintptr_t sm127_engine_symbol(const char *name);

// look up a name in the wrapper's import table (0 if absent);
// used by dlsym_fake so dlopen'd modules see the same environment
uintptr_t sm127_find_import(const char *name);

// engine threads created through the pthread shim (for the hang watchdog)
#include <switch.h>
int sm127_engine_threads(Thread **out_thr, void **out_entry, int max);

// Shared lock-slot validation. A pointer we handed out is heap-resident AND
// aligned; anything else in a pthread storage slot is someone else's 32-bit
// write. Used by the mutex/cond paths in imports.c and the rwlock path in
// godot_shim.c, so both apply the same test.
int  sm127_plausible_lock_ptr(uint64_t v);
void sm127_note_bad_lock_slot(const char *what, uint64_t v);
void sm127_note_bad_lock_slot_at(const char *what, uint64_t v, const void *at);
extern uint64_t sm127_bad_lock_slots;

// Largest mmap reservation, published by mmap_fake. Both in imports.c.
extern uintptr_t sm127_biggest_map_base;
extern size_t    sm127_biggest_map_len;

// Pause and dump every thread -- game thread plus engine workers. Used by the
// stall watchdog and by the crash handler; see main.c.
void sm127_dump_all_threads(void);

// Shader activity, for correlating a crash against compilation. A MISS is a
// real compile; a LOAD came from cache and did none. See imports.c.
extern uint32_t sm127_shader_compiles;
extern uint32_t sm127_shader_loads;
extern uint32_t sm127_last_compile_frame;
extern uint32_t sm127_last_load_frame;

// Frame-thread wait accounting (imports.c): the game thread's handle, set by
// main.c, and the time that thread has spent sleeping and inside GL calls that
// can block on the GPU. Read by the pacing log.
extern Handle g_sm127_game_thread;
extern u64 g_sm127_sleep_us;
extern u64 g_sm127_glwait_us;

#endif
