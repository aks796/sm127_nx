/* egl_shim.c -- dlopen/dlsym bridge to mesa EGL/GLES. The engine dlopen()s
 * libEGL/libGLESv3 and resolves entry points by name; we answer with mesa,
 * which is linked statically into the NRO.
 *
 * Reduced from sts2_nx's version: there is no NativeAOT payload, no
 * GDExtension and no Vulkan here, so the only real handles are the
 * main-program one and the catch-all GL bridge.
 *
 * MIT license; see LICENSE. */

#include <string.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "config.h"
#include "util.h"      // debugPrintf
#include "egl_shim.h"
#include "so_util.h"   // so_get_list, so_try_find_addr_rx (main-program dlsym)
#include "imports.h"
#include "native_modules.h"

#define FAKE_DL_HANDLE ((void *)0xE61D1B)

typedef void (*generic_func)(void);
typedef struct { const char *name; generic_func fn; } EglEntry;

#define E(sym) { #sym, (generic_func)sym }

// core EGL entry points (eglGetProcAddress only returns extension/GL functions)
static const EglEntry egl_table[] = {
  E(eglGetError),
  E(eglGetDisplay),
  E(eglInitialize),
  E(eglTerminate),
  E(eglQueryString),
  E(eglGetConfigs),
  E(eglChooseConfig),
  E(eglGetConfigAttrib),
  E(eglCreateWindowSurface),
  E(eglCreatePbufferSurface),
  E(eglCreatePixmapSurface),
  E(eglDestroySurface),
  E(eglQuerySurface),
  E(eglBindAPI),
  E(eglQueryAPI),
  E(eglWaitClient),
  E(eglReleaseThread),
  E(eglCreatePbufferFromClientBuffer),
  E(eglSurfaceAttrib),
  E(eglBindTexImage),
  E(eglReleaseTexImage),
  E(eglSwapInterval),
  E(eglCreateContext),
  E(eglDestroyContext),
  E(eglMakeCurrent),
  E(eglGetCurrentContext),
  E(eglGetCurrentSurface),
  E(eglGetCurrentDisplay),
  E(eglQueryContext),
  E(eglWaitGL),
  E(eglWaitNative),
  E(eglSwapBuffers),
  E(eglCopyBuffers),
  E(eglGetProcAddress),
};

static int ends_with(const char *s, const char *suffix) {
  size_t ls = strlen(s), lx = strlen(suffix);
  return ls >= lx && strcmp(s + (ls - lx), suffix) == 0;
}

void *dlopen_fake(const char *filename, int flag) {
  (void)flag;

  if (!filename) {
    debugPrintf("[dl] dlopen(NULL) -> main-program handle\n");
    return MAIN_PROG_DL_HANDLE;
  }

  debugPrintf("[dl] dlopen(\"%s\")\n", filename);

  // A real native module next to the NRO (GDNative / GDExtension): load it.
  {
    void *h = native_module_open(filename);
    if (h) return h;
  }

  // Libraries we must REFUSE rather than pretend to have.
  //
  // The permissive default below hands back a valid-looking handle for any
  // unknown name, and dlsym_fake then returns NULL for each symbol. For a
  // library the engine only probes that is fine. For one it COMMITS to on a
  // successful dlopen it is the worst answer: it selects a backend and then
  // calls a null function pointer.
  //
  // libvulkan is the one that matters. Godot's stock Android template ships
  // both renderers; this wrapper has GLES3 only, so Vulkan must fail at the
  // dlopen, which is where Godot's own fallback to OpenGL is. main() also
  // pins --rendering-driver opengl3; this is the belt to those braces.
  static const char *refuse[] = {
    "libvulkan.so", "libvulkan.so.1",
    "libopenxr_loader.so",
    "libcamera2ndk.so", "libmediandk.so",
    NULL
  };
  for (int i = 0; refuse[i]; i++) {
    if (ends_with(filename, refuse[i])) {
      debugPrintf("[dl] refusing %s (unavailable by design)\n", refuse[i]);
      return NULL;
    }
  }

  return FAKE_DL_HANDLE; // any GL/EGL library maps to the bridge
}

void *dlsym_fake(void *handle, const char *symbol) {
  if (!symbol)
    return NULL;

  if (native_module_is_handle(handle))
    return native_module_sym(handle, symbol);

  if (handle == MAIN_PROG_DL_HANDLE) {
    // Every loaded module, then the wrapper's own import table. Safe on
    // finalized modules: so_finalize rebases syms/dynstrtab (so_util.c).
    for (so_module *m = so_get_list(); m; m = m->next) {
      uintptr_t a = so_try_find_addr_rx(m, symbol);
      if (a) return (void *)a;
    }
    uintptr_t a = sm127_find_import(symbol);
    if (a) return (void *)a;

    static int misses;
    if (misses < 40) {
      misses++;
      debugPrintf("[dl] dlsym(main-program, \"%s\") -> NOT FOUND\n", symbol);
    }
    return NULL;
  }

  for (unsigned i = 0; i < sizeof(egl_table) / sizeof(*egl_table); i++) {
    if (strcmp(symbol, egl_table[i].name) == 0)
      return (void *)egl_table[i].fn;
  }
  // anything the wrapper already provides via the import table (Godot dlopens
  // itself/other libs and dlsyms plain libc/GL names)
  uintptr_t imp = sm127_find_import(symbol);
  if (imp)
    return (void *)imp;
  return (void *)eglGetProcAddress(symbol); // GL entry points + EGL extensions
}

int dlclose_fake(void *handle) { (void)handle; return 0; }

char *dlerror_fake(void) { return NULL; }
