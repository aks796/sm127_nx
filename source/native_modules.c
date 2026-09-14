/* native_modules.c -- GDNative / GDExtension libraries the engine dlopen()s.
 *
 * Super Mario 127 is pure GDScript and ships no GDNative library, so nothing
 * here runs for a stock APK. It is kept because a modded APK can add one, and
 * because the alternative is worse: on Android, Godot 3's GDNative passes just
 * the file name to dlopen ("libraries are located separately from resource
 * assets"), then dlsym()s godot_gdnative_init, godot_nativescript_init and
 * friends.
 *
 * egl_shim.c's dlopen_fake used to answer any unknown name with a catch-all
 * handle whose symbols resolve through eglGetProcAddress -- which for a
 * GDNative entry point is garbage. Now a name that exists next to the NRO is
 * loaded for real: so_util, the same import table as the engine, init arrays,
 * and a symbol cache captured before so_finalize.
 *
 * Same shape as sts2_nx's FMOD/Spine GDExtension loading, reduced to what a
 * GDNative module needs. MIT license; see LICENSE. */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "so_util.h"
#include "imports.h"
#include "native_modules.h"

#define MAX_NATIVE 4

// Entry points GDNative / NativeScript / GDExtension look up, cached before
// so_finalize (the symbol tables live in load_base, which finalize remaps).
// Anything else falls back to so_try_find_addr_rx.
static const char *k_cached[] = {
  "godot_gdnative_init", "godot_gdnative_terminate", "godot_gdnative_singleton",
  "godot_nativescript_init", "godot_nativescript_terminate",
  "godot_nativescript_frame",
  "godot_nativescript_thread_enter", "godot_nativescript_thread_exit",
  "gdextension_init",
  NULL
};
#define NCACHED (sizeof(k_cached) / sizeof(*k_cached))

typedef struct {
  char      name[64];
  int       state;        // 0 free, 1 loaded, -1 failed
  so_module mod;
  uintptr_t cache[NCACHED];
} NativeModule;

static NativeModule s_mods[MAX_NATIVE];
static Mutex  s_lock;     // zero-initialised == unlocked
static char  *s_base;
static size_t s_left;

void native_modules_set_region(void *base, size_t size) {
  s_base = base;
  s_left = size;
}

static const char *basename_of(const char *p) {
  const char *s = strrchr(p, '/');
  return s ? s + 1 : p;
}

int native_module_is_handle(const void *handle) {
  for (int i = 0; i < MAX_NATIVE; i++)
    if (handle == &s_mods[i]) return s_mods[i].state > 0;
  return 0;
}

void *native_module_open(const char *path) {
  if (!path) return NULL;
  const char *name = basename_of(path);

  char full[352];
  snprintf(full, sizeof(full), "%s/%s", config.data_root, name);
  struct stat st;
  if (stat(full, &st) != 0) return NULL;   // not one of ours

  mutexLock(&s_lock);
  NativeModule *slot = NULL;
  for (int i = 0; i < MAX_NATIVE; i++) {
    if (s_mods[i].state && !strcmp(s_mods[i].name, name)) {
      void *h = s_mods[i].state > 0 ? &s_mods[i] : NULL;
      mutexUnlock(&s_lock);
      return h;
    }
    if (!slot && !s_mods[i].state) slot = &s_mods[i];
  }
  if (!slot || !s_base) {
    mutexUnlock(&s_lock);
    debugPrintf("[native] cannot load %s: %s\n", name, slot ? "no region" : "no free slot");
    return NULL;
  }

  memset(slot, 0, sizeof(*slot));
  strncpy(slot->name, name, sizeof(slot->name) - 1);

  so_module *m = &slot->mod;
  int rc = so_load(m, full, s_base, s_left);
  if (rc < 0) {
    slot->state = -1;
    mutexUnlock(&s_lock);
    debugPrintf("[native] so_load %s failed (%d)\n", full, rc);
    return NULL;
  }

  sm127_resolve_imports(m);
  for (unsigned i = 0; k_cached[i]; i++)
    slot->cache[i] = so_try_find_addr_rx(m, k_cached[i]);

  so_finalize(m);
  so_flush_caches(m);
  so_execute_init_array(m);

  const size_t used = ALIGN_MEM(m->load_size, 0x100000);
  s_base += used;
  s_left  = used < s_left ? s_left - used : 0;
  slot->state = 1;
  mutexUnlock(&s_lock);

  debugPrintf("[native] loaded %s (%u KB at %p; %u KB of the region left)\n",
              name, (unsigned)(m->load_size >> 10), m->load_virtbase,
              (unsigned)(s_left >> 10));
  return slot;
}

void *native_module_sym(void *handle, const char *symbol) {
  NativeModule *slot = handle;
  if (!native_module_is_handle(handle) || !symbol) return NULL;
  for (unsigned i = 0; k_cached[i]; i++)
    if (!strcmp(symbol, k_cached[i]))
      return slot->cache[i] ? (void *)slot->cache[i] : NULL;
  uintptr_t a = so_try_find_addr_rx(&slot->mod, symbol);
  if (!a) debugPrintf("[native] dlsym(%s, \"%s\") -> NOT FOUND\n", slot->name, symbol);
  return (void *)a;
}
