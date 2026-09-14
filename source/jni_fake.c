/* jni_fake.c -- fake JNI environment implementing the JNI surface Godot 4.6's
 * android glue calls back into: the Godot/GodotIO wrappers (locale, dirs, DPI,
 * clipboard, lifecycle notifications), the DirectoryAccessHandler and
 * FileAccessHandler used for dir listing and filesystem access, and the
 * generic object/string/array machinery the runtime touches.
 *
 * Object model, local-reference registry and JNIEnv/JavaVM tables are taken
 * from the SOTN Switch wrapper (Andy Nguyen / fgsfds lineage); the method
 * dispatch follows godot's platform/android/java_godot_*_wrapper.cpp and
 * {dir,file}_access_*_jandroid.cpp. MIT license; see LICENSE. */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "libc_shim.h"
#include "godot_shim.h"   // sm127_rename_replacing
#include "jni_unimpl.h"
#include "jni_fake.h"
#include "jni_godot_ext.h"
#include "audio_sink.h"

#define JNI_OK 0
#define JNI_VERSION_1_6 0x00010006

typedef uint64_t juint;

// quit flag set when the game calls forceQuit(); main.c polls it
volatile int jni_quit_requested = 0;

// ---------------------------------------------------------------------------
// fake object model
// ---------------------------------------------------------------------------

enum {
  TAG_OBJECT = 0x4f424a31, // 'OBJ1'  heap object (freeable)
  TAG_STRING = 0x53545231, // 'STR1'
  TAG_OBJARR = 0x4f415231, // 'OAR1'
  TAG_PRIARR = 0x50415231, // 'PAR1'
  TAG_ID     = 0x4d494431, // 'MID1'  pooled, never freed
  TAG_CLASS  = 0x434c5331, // 'CLS1'  singleton, never freed
};

enum obj_kind {
  K_GENERIC = 0, K_ACTIVITY, K_ASSETMGR,
  K_GODOT, K_GODOT_IO, K_NETUTILS, K_DIRHANDLER, K_FILEHANDLER, K_TTS,
  K_SURFACE, K_BYTEBUF, K_FILE,
};

typedef struct { uint32_t tag; int kind; void *ptr; int64_t cap; char str[300]; } FakeObject;
typedef struct { uint32_t tag; char *utf; } FakeString;
typedef struct { uint32_t tag; int len; int elem_size; void *data; } FakePriArray;
typedef struct { uint32_t tag; int len; void **items; } FakeObjArray;
// `cls` records which class GetMethodID was called on. jni_fake originally
// dispatched on method NAME alone, which was safe while every caller was
// Godot -- but FMOD's org/fmod/{FMOD,AudioDevice,MediaCodec} all have an
// init() and a close(), and org/fmod/FMOD.init collides with Godot's own.
// FindClass now records the name so dispatch can discriminate.
// Empty cls means the ID came from GetObjectClass, i.e. the old path; those
// still dispatch by name, which preserves every existing handler.
typedef struct { uint32_t tag; char cls[96]; char name[64]; char sig[112]; } FakeID;

// ---------------------------------------------------------------------------
// local reference registry
// ---------------------------------------------------------------------------

#define MAX_LOCALS 8192
// 32 was too few and failed silently. Local frames nest with the engine's call
// depth, and a UI tree walk goes deeper than 32 without difficulty. 256 ints
// costs a kilobyte.
#define MAX_FRAMES 256
static void *locals[MAX_LOCALS];
static int locals_top = 0;
static int frames[MAX_FRAMES];
static int frame_top = 0;
static Mutex locals_lock;

static uint64_t locals_dropped, frames_dropped, unbalanced_pops;

static void *reg_local(void *ref) {
  if (ref) {
    mutexLock(&locals_lock);
    if (locals_top < MAX_LOCALS) {
      locals[locals_top++] = ref;
    } else {
      // Untracked, so PopLocalFrame will never release it. A leak rather than
      // a fault, but it also means the table no longer describes reality, and
      // it used to happen in complete silence.
      if (locals_dropped++ == 0)
        debugPrintf("[jni] local ref table full at %d -- refs from here on are "
                    "untracked and will leak\n", MAX_LOCALS);
    }
    mutexUnlock(&locals_lock);
  }
  return ref;
}

static void free_ref(void *ref) {
  if (!ref) return;
  switch (*(uint32_t *)ref) {
    case TAG_STRING: { FakeString *s = ref; free(s->utf); free(s); break; }
    case TAG_PRIARR: { FakePriArray *a = ref; free(a->data); free(a); break; }
    case TAG_OBJARR: { FakeObjArray *a = ref; free(a->items); free(a); break; }
    case TAG_OBJECT: free(ref); break;
    default: break; // TAG_ID / TAG_CLASS are never freed
  }
}

static void delete_local(void *ref) {
  if (!ref) return;
  mutexLock(&locals_lock);
  for (int i = locals_top - 1; i >= 0; i--) {
    if (locals[i] == ref) { locals[i] = locals[--locals_top]; free_ref(ref); break; }
  }
  mutexUnlock(&locals_lock);
}

// ---------------------------------------------------------------------------
// constructors
// ---------------------------------------------------------------------------

static void *jni_make_string(const char *utf) {
  FakeString *s = calloc(1, sizeof(*s));
  s->tag = TAG_STRING;
  s->utf = strdup(utf ? utf : "");
  return reg_local(s);
}

static void *make_pri_array_adopt(void *data, int len, int elem_size) {
  FakePriArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_PRIARR;
  a->len = len;
  a->elem_size = elem_size;
  a->data = data;
  return reg_local(a);
}

static void *new_pri_array(int len, int elem_size) {
  void *data = calloc(len ? len : 1, elem_size);
  return make_pri_array_adopt(data, len, elem_size);
}

static void *new_obj_array(int len) {
  FakeObjArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_OBJARR;
  a->len = len;
  a->items = calloc(len ? len : 1, sizeof(void *));
  return reg_local(a);
}

static void *make_generic(int kind) {
  FakeObject *o = calloc(1, sizeof(*o));
  o->tag = TAG_OBJECT;
  o->kind = kind;
  return reg_local(o);
}

static const char *obj_str(void *jstr) {
  FakeString *s = jstr;
  if (s && s->tag == TAG_STRING) return s->utf;
  return "";
}

// singletons handed to GodotLib.initialize (never freed)
static FakeObject g_cls_generic, g_activity, g_assetmgr, g_surface, g_renderview;
static FakeObject g_godot, g_godot_io, g_netutils, g_dirhandler, g_filehandler, g_tts;

static void init_singleton(FakeObject *o, int kind) {
  o->tag = TAG_CLASS; // class-tag: never freed by DeleteRef
  o->kind = kind;
}

static void init_singletons(void) {
  init_singleton(&g_cls_generic, K_GENERIC);
  init_singleton(&g_activity, K_ACTIVITY);
  init_singleton(&g_assetmgr, K_ASSETMGR);
  init_singleton(&g_surface, K_SURFACE);
  init_singleton(&g_renderview, K_GENERIC);
  init_singleton(&g_godot, K_GODOT);
  init_singleton(&g_godot_io, K_GODOT_IO);
  init_singleton(&g_netutils, K_NETUTILS);
  init_singleton(&g_dirhandler, K_DIRHANDLER);
  init_singleton(&g_filehandler, K_FILEHANDLER);
  init_singleton(&g_tts, K_TTS);
}

void *jni_activity_class(void)      { return &g_cls_generic; }
void *jni_activity_object(void)     { return &g_activity; }
void *jni_godot_object(void)        { return &g_godot; }
void *jni_godot_io_object(void)     { return &g_godot_io; }
void *jni_netutils_object(void)     { return &g_netutils; }
void *jni_dirhandler_object(void)   { return &g_dirhandler; }
void *jni_filehandler_object(void)  { return &g_filehandler; }
void *jni_tts_object(void)          { return &g_tts; }
void *jni_assetmgr_object(void)     { return &g_assetmgr; }
void *jni_surface_object(void)      { return &g_surface; }
void *jni_new_string(const char *s) { return jni_make_string(s); }

void *jni_new_string_array(int n, const char **items) {
  FakeObjArray *a = new_obj_array(n);
  for (int i = 0; i < n; i++)
    a->items[i] = jni_make_string(items[i]);
  return a;
}

void *jni_new_float_array(int n, const float *data) {
  float *copy = calloc(n ? n : 1, sizeof(float));
  if (data) memcpy(copy, data, (size_t)n * sizeof(float));
  return make_pri_array_adopt(copy, n, sizeof(float));
}

void jni_release_local(void *ref) { delete_local(ref); }

// ---------------------------------------------------------------------------
// method/field ID pool
// ---------------------------------------------------------------------------

#define MAX_IDS 512
static FakeID id_pool[MAX_IDS];
static int id_count = 0;
static Mutex id_lock;

// Named class objects, so FindClass("org/fmod/FMOD") can be told apart from
// FindClass("org/godotengine/godot/Godot"). Tagged TAG_CLASS so DeleteRef
// never frees them; the name lives in FakeObject's existing str[].
#define MAX_CLASSES 64
static FakeObject class_pool[MAX_CLASSES];
static int class_count = 0;
static Mutex class_lock;

static void *find_class_obj(const char *name) {
  if (!name || !*name) return &g_cls_generic;

  mutexLock(&class_lock);
  for (int i = 0; i < class_count; i++)
    if (!strcmp(class_pool[i].str, name)) {
      mutexUnlock(&class_lock);
      return &class_pool[i];
    }
  if (class_count >= MAX_CLASSES) {
    mutexUnlock(&class_lock);
    debugPrintf("[jni] class pool full, falling back to generic for %s\n", name);
    return &g_cls_generic;
  }
  FakeObject *c = &class_pool[class_count++];
  c->tag = TAG_CLASS;
  c->kind = K_GENERIC;
  strncpy(c->str, name, sizeof(c->str) - 1);
  mutexUnlock(&class_lock);
  return c;
}

static FakeID *get_id(const char *cls, const char *name, const char *sig) {
  if (!cls) cls = "";
  mutexLock(&id_lock);
  for (int i = 0; i < id_count; i++)
    if (!strcmp(id_pool[i].name, name) &&
        !strcmp(id_pool[i].sig, sig ? sig : "") &&
        !strcmp(id_pool[i].cls, cls)) {
      mutexUnlock(&id_lock);
      return &id_pool[i];
    }
  // Overflow. Upstream returned &id_pool[0] silently, which means every
  // further lookup dispatches to whatever the FIRST method registered was --
  // wrong behaviour with no diagnostic, in a codebase where the only debugging
  // channel is this log.
  //
  // Keying on the class as well as name+signature raised the entry count (the
  // same method name reached through two different FindClass results is now
  // two entries), so this is more reachable than it was. It still should not
  // happen -- ~130 distinct methods across the Godot and FMOD surfaces against
  // MAX_IDS 512 -- but if it ever does, saying so is the difference between a
  // five-minute fix and a week.
  if (id_count >= MAX_IDS) {
    static int warned = 0;
    if (!warned) {
      warned = 1;
      debugPrintf("[jni] FATAL-ish: FakeID pool exhausted at %d entries. "
                  "Further lookups will alias to '%s.%s' and dispatch WRONG. "
                  "Raise MAX_IDS.\n", MAX_IDS, id_pool[0].cls, id_pool[0].name);
    }
    mutexUnlock(&id_lock);
    return &id_pool[0];
  }

  // One-shot warning while there is still room to act on it.
  if (id_count == (MAX_IDS * 3) / 4) {
    debugPrintf("[jni] FakeID pool %d/%d used\n", id_count, MAX_IDS);
  }

  FakeID *id = &id_pool[id_count++];
  id->tag = TAG_ID;
  strncpy(id->cls, cls, sizeof(id->cls) - 1);
  strncpy(id->name, name, sizeof(id->name) - 1);
  strncpy(id->sig, sig ? sig : "", sizeof(id->sig) - 1);
  mutexUnlock(&id_lock);
  return id;
}

// ---------------------------------------------------------------------------
// DirectoryAccessHandler / FileAccessHandler backends
// (paths per godot's {dir,file}_access_*_jandroid.cpp; ids are 1-based)
// ---------------------------------------------------------------------------

#define ACCESS_RESOURCES 0
#define ACCESS_USERDATA  1
#define ACCESS_FILESYSTEM 2

// map a godot-side path to an on-SD path for the given access type.
// note: with an empty resource dir (the android case) godot's fix_path turns
// "res://X" into "/X" before it reaches us, so for RESOURCES/USERDATA a
// leading slash is still relative to that domain's root, not the SD root.
// defensively strip a leaked data_root prefix too (a stale resource_path
// derived from the wrapper cwd shows up as "switch/<appdir>/X").
static const char *strip_data_root(const char *p) {
  const char *root = config.data_root; // discovered; see app_paths.c
  if (root[0] == '/' && !strncmp(p, root + 1, strlen(root) - 1)) {
    const char *rest = p + strlen(root) - 1;
    if (*rest == '/' || *rest == 0) {
      while (*rest == '/') rest++;
      return rest;
    }
  }
  return p;
}

static const char *resolve_gd_path(int access_type, const char *p, char *buf, size_t sz) {
  if (!p) p = "";
  if (!strncmp(p, "res://", 6)) {
    p += 6;
    while (*p == '/') p++;
    snprintf(buf, sz, "%s/assets/%s", config.data_root, strip_data_root(p));
  } else if (!strncmp(p, "user://", 7)) {
    p += 7;
    while (*p == '/') p++;
    snprintf(buf, sz, "%s/%s", config.save_root, p);
  } else if (access_type == ACCESS_RESOURCES) {
    while (*p == '/') p++;
    snprintf(buf, sz, "%s/assets/%s", config.data_root, strip_data_root(p));
  } else if (access_type == ACCESS_USERDATA) {
    while (*p == '/') p++;
    snprintf(buf, sz, "%s/%s", config.save_root, p);
  } else {
    // FILESYSTEM: absolute paths outside the app's dirs get the same
    // android-style sandbox rebase as the libc layer
    const char *s = sandbox_path(p, buf, sz);
    if (s != buf) snprintf(buf, sz, "%s", s);
  }
  // strip trailing slashes (fatfs stat dislikes them)
  size_t l = strlen(buf);
  while (l > 1 && buf[l - 1] == '/') buf[--l] = 0;
  return buf;
}

#define MAX_GD_DIRS 128

typedef struct {
  DIR *dir;
  char path[512];
  char current[256];
  int current_is_dir;
} GdDir;

static GdDir gd_dirs[MAX_GD_DIRS];
static Mutex gd_dir_lock;

static int gd_dir_open(int access_type, const char *path) {
  char buf[512];
  resolve_gd_path(access_type, path, buf, sizeof(buf));
  DIR *d = opendir(buf);
#if VERBOSE_IO
  debugPrintf("[jni] dirOpen(%d, \"%s\" -> \"%s\") = %p\n", access_type, path, buf, (void *)d);
#endif
  if (!d) {
    debugPrintf("[jni] dirOpen(%d, \"%s\"): opendir(\"%s\") failed\n", access_type, path, buf);
    return -1;
  }
  mutexLock(&gd_dir_lock);
  for (int i = 0; i < MAX_GD_DIRS; i++) {
    if (!gd_dirs[i].dir) {
      gd_dirs[i].dir = d;
      strncpy(gd_dirs[i].path, buf, sizeof(gd_dirs[i].path) - 1);
      gd_dirs[i].current[0] = 0;
      mutexUnlock(&gd_dir_lock);
      return i + 1;
    }
  }
  mutexUnlock(&gd_dir_lock);
  closedir(d);
  debugPrintf("[jni] dirOpen: out of dir slots!\n");
  return -1;
}

static GdDir *gd_dir_get(int id) {
  if (id < 1 || id > MAX_GD_DIRS) return NULL;
  GdDir *g = &gd_dirs[id - 1];
  return g->dir ? g : NULL;
}

// returns the next entry name or NULL; updates current_is_dir
static const char *gd_dir_next(int id) {
  GdDir *g = gd_dir_get(id);
  if (!g) return NULL;
  struct dirent *e = readdir(g->dir);
  if (!e) return NULL;
  strncpy(g->current, e->d_name, sizeof(g->current) - 1);
  char full[800];
  snprintf(full, sizeof(full), "%s/%s", g->path, e->d_name);
  struct stat st;
  g->current_is_dir = (stat(full, &st) == 0 && S_ISDIR(st.st_mode));
  return g->current;
}

static void gd_dir_close(int id) {
  GdDir *g = gd_dir_get(id);
  if (!g) return;
  mutexLock(&gd_dir_lock);
  closedir(g->dir);
  g->dir = NULL;
  mutexUnlock(&gd_dir_lock);
}

#define MAX_GD_FILES 64

typedef struct {
  FILE *f;
  int64_t size;
  int eof;
  int poolable; // .pck handle: park on close instead of fclose
} GdFile;

static GdFile gd_files[MAX_GD_FILES];
static Mutex gd_file_lock;

// FileAccessPack opens the SAME .pck once per resource; each fopen costs a
// fatfs path walk. Park closed pck handles and hand them back on the next
// open of the same path -- reopening becomes a rewind.
#define PCK_POOL_N 8
static FILE *pck_pool[PCK_POOL_N];
static int64_t pck_pool_size = -1;
static char pck_pool_path[288];

static int path_is_pck(const char *p) {
  size_t l = strlen(p);
  return l > 4 && strcmp(p + l - 4, ".pck") == 0;
}

// godot FileAccess::ModeFlags
#define GD_READ 1
#define GD_WRITE 2
#define GD_READ_WRITE 3
#define GD_WRITE_READ 7

static int gd_file_open(const char *path, int mode) {
  const char *m;
  switch (mode) {
    case GD_READ:       m = "rb";  break;
    case GD_WRITE:      m = "wb";  break;
    case GD_READ_WRITE: m = "rb+"; break;
    case GD_WRITE_READ: m = "wb+"; break;
    default: return -1;
  }
  char buf[512];
  resolve_gd_path(ACCESS_FILESYSTEM, path, buf, sizeof(buf));

  FILE *f = NULL;
  int64_t known_size = -1;
  const int poolable = (mode == GD_READ && path_is_pck(buf));
  if (poolable) {
    mutexLock(&gd_file_lock);
    if (strcmp(pck_pool_path, buf) == 0) {
      for (int i = 0; i < PCK_POOL_N; i++) {
        if (pck_pool[i]) {
          f = pck_pool[i];
          pck_pool[i] = NULL;
          known_size = pck_pool_size;
          break;
        }
      }
    }
    mutexUnlock(&gd_file_lock);
    if (f) {
      fseek(f, 0, SEEK_SET);
      clearerr(f);
    }
  }

  if (!f) {
    f = fopen(buf, m);
#if VERBOSE_IO
    debugPrintf("[jni] fileOpen(\"%s\" -> \"%s\", %d) = %p\n", path, buf, mode, (void *)f);
#endif
    if (!f) return -1;
    setvbuf(f, NULL, _IOFBF, 64 * 1024);
  }

  mutexLock(&gd_file_lock);
  for (int i = 0; i < MAX_GD_FILES; i++) {
    if (!gd_files[i].f) {
      gd_files[i].f = f;
      gd_files[i].eof = 0;
      gd_files[i].poolable = poolable;
      if (known_size >= 0) {
        gd_files[i].size = known_size;
      } else {
        fseek(f, 0, SEEK_END);
        gd_files[i].size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (poolable) {
          pck_pool_size = gd_files[i].size;
          strncpy(pck_pool_path, buf, sizeof(pck_pool_path) - 1);
        }
      }
      mutexUnlock(&gd_file_lock);
      return i + 1;
    }
  }
  mutexUnlock(&gd_file_lock);
  fclose(f);
  return -1;
}

static GdFile *gd_file_get(int id) {
  if (id < 1 || id > MAX_GD_FILES) return NULL;
  GdFile *g = &gd_files[id - 1];
  return g->f ? g : NULL;
}

static void gd_file_close(int id) {
  GdFile *g = gd_file_get(id);
  if (!g) return;
  mutexLock(&gd_file_lock);
  if (g->poolable) {
    for (int i = 0; i < PCK_POOL_N; i++) {
      if (!pck_pool[i]) {
        pck_pool[i] = g->f;
        g->f = NULL;
        break;
      }
    }
  }
  if (g->f) fclose(g->f);
  g->f = NULL;
  mutexUnlock(&gd_file_lock);
}

// ---------------------------------------------------------------------------
// method dispatch (by name + signature; instance and static share handlers).
// varargs follow AAPCS64: jint -> int, jlong -> int64_t, jfloat/jdouble ->
// double, objects -> void *.
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// Chained dispatch helpers.
//
// Each fallback below is handed the same va_list in turn. On AArch64 va_list is
// a struct, so a callee gets a copy and the caller's is untouched -- but the C
// standard says that after a function calls va_arg on a va_list it received,
// the CALLER's value is indeterminate. That is a rule about the language, not
// about this ABI, and code that leans on the ABI is how you get a bug that only
// appears after a toolchain bump.
//
// So each attempt gets its own va_copy. The extension functions name-check
// before consuming anything, so in practice a miss consumes nothing either way
// -- this makes that guarantee explicit rather than incidental.
// ---------------------------------------------------------------------------

#define TRY_EXT_INT(fn) do {                       \
    va_list _c; va_copy(_c, va);                   \
    int _r; int _hit = fn(name, &_r, _c);          \
    va_end(_c);                                    \
    if (_hit) return (juint)_r;                    \
  } while (0)

#define TRY_EXT_VOID(fn) do {                      \
    va_list _c; va_copy(_c, va);                   \
    int _hit = fn(name, _c);                       \
    va_end(_c);                                    \
    if (_hit) return;                              \
  } while (0)

#define TRY_EXT_VOID_INT(fn) do {                  \
    va_list _c; va_copy(_c, va);                   \
    int _r; int _hit = fn(name, &_r, _c);          \
    va_end(_c);                                    \
    if (_hit) return;                              \
  } while (0)

// sts2_nx routed org/fmod/* here first; SM127 has no FMOD, so these are empty.
#define TRY_FMOD(retexpr) do {} while (0)
#define TRY_FMOD_VOID() do {} while (0)

static juint call_boolean(void *recv, FakeID *id, va_list va) {
  const char *name = id->name;
  (void)recv;
  TRY_FMOD((juint)fr.z);

  // Godot class
  if (!strcmp(name, "forceQuit")) { jni_quit_requested = 1; return 1; }
  if (!strcmp(name, "requestPermission"))  return 1;
  if (!strcmp(name, "requestPermissions")) return 1;
  if (!strcmp(name, "isDarkModeSupported") || !strcmp(name, "isDarkMode")) return 0;
  if (!strcmp(name, "hasClipboard"))       return 0;
  if (!strcmp(name, "isInImmersiveMode"))  return 0;
  if (!strcmp(name, "checkInternalFeatureSupport")) { (void)va_arg(va, void *); return 0; }
  if (!strcmp(name, "hasHardwareKeyboard")) return 0;
  // Godot 3.x: no offscreen (loader-thread) GL context -- the engine then
  // uploads on the render thread. And the activity is always resumed.
  if (!strcmp(name, "createOffscreenGL"))  return 0;
  if (!strcmp(name, "isActivityResumed"))  return 1;
  if (!strcmp(name, "isSpeaking"))          return 0;
  // DirectoryAccessHandler
  if (!strcmp(name, "dirIsDir")) {
    GdDir *g = gd_dir_get(va_arg(va, int));
    return g ? (juint)g->current_is_dir : 0;
  }
  if (!strcmp(name, "isCurrentHidden")) {
    GdDir *g = gd_dir_get(va_arg(va, int));
    return (g && g->current[0] == '.' && strcmp(g->current, ".") && strcmp(g->current, "..")) ? 1 : 0;
  }
  if (!strcmp(name, "dirExists")) {
    int at = va_arg(va, int);
    const char *p = obj_str(va_arg(va, void *));
    char buf[512];
    struct stat st;
    int ok = (stat(resolve_gd_path(at, p, buf, sizeof(buf)), &st) == 0 && S_ISDIR(st.st_mode));
    debugPrintf("[jni] dirExists(%d, \"%s\") -> \"%s\" = %d\n", at, p, buf, ok);
    return ok ? 1 : 0;
  }
  if (!strcmp(name, "fileExists")) {
    char buf[512];
    struct stat st;
    if (id->sig[1] == 'I') { // DirectoryAccessHandler variant: (ILjava/lang/String;)Z
      int at = va_arg(va, int);
      const char *p = obj_str(va_arg(va, void *));
      const char *full = resolve_gd_path(at, p, buf, sizeof(buf));
      int ok = (stat(full, &st) == 0 && !S_ISDIR(st.st_mode)) ? 1 : 0;
      // Log .so probes only. If GDMono checks for the NativeAOT payload with
      // FileAccess::exists before dlopen'ing it, this names the file it wants
      // -- the same open question the [dl] dlopen logging covers from the
      // other side. Filtered to .so so it costs nothing during asset loading,
      // which is why VERBOSE_IO is not simply switched on.
      if (p && strstr(p, ".so"))
        debugPrintf("[jni] fileExists(\"%s\") -> %s = %d\n", p, full, ok);
      return ok;
    }
    const char *p = obj_str(va_arg(va, void *)); // FileAccessHandler: (Ljava/lang/String;)Z
    return (stat(resolve_gd_path(ACCESS_FILESYSTEM, p, buf, sizeof(buf)), &st) == 0 && !S_ISDIR(st.st_mode)) ? 1 : 0;
  }
  if (!strcmp(name, "makeDir")) {
    int at = va_arg(va, int);
    const char *p = obj_str(va_arg(va, void *));
    char buf[512];
    resolve_gd_path(at, p, buf, sizeof(buf));
    // Logged, because this was silent and the shader cache depends on it.
    // "Can't create shader cache folder" told us the engine gave up but not
    // what it had asked for or what happened.
    if (mkdir(buf, 0777) == 0) {
      debugPrintf("[jni] makeDir(%d, \"%s\") -> \"%s\" created\n", at, p, buf);
      return 1;
    }
    struct stat st;
    const int existed = (stat(buf, &st) == 0 && S_ISDIR(st.st_mode));
    debugPrintf("[jni] makeDir(%d, \"%s\") -> \"%s\" %s\n", at, p, buf,
                existed ? "already exists" : "FAILED");
    return existed ? 1 : 0;
  }
  if (!strcmp(name, "rename")) {
    int at = va_arg(va, int);
    const char *from = obj_str(va_arg(va, void *));
    const char *to = obj_str(va_arg(va, void *));
    char b1[512], b2[512];
    resolve_gd_path(at, from, b1, sizeof(b1));
    resolve_gd_path(at, to, b2, sizeof(b2));

    // sm127_rename_replacing, not bare rename(). POSIX rename() replaces an
    // existing destination; FAT through newlib returns FR_EXIST and fails.
    //
    // THIS is the path that matters. The engine is the Android build, so
    // `user://` goes through DirectoryAccessHandler here rather than
    // DirAccessUnix -- run 15 still failed every save after the libc-level
    // fix, because Godot never reached libc rename() for those paths.
    //
    // The save pattern is write-tmp-then-rename, so without this nothing
    // persists: not settings, not progress, not the backup rotation.
    return sm127_rename_replacing(b1, b2) == 0 ? 1 : 0;
  }
  if (!strcmp(name, "remove")) {
    int at = va_arg(va, int);
    const char *p = obj_str(va_arg(va, void *));
    char buf[512];
    resolve_gd_path(at, p, buf, sizeof(buf));
    struct stat st;
    if (stat(buf, &st) != 0) return 0;
    return ((S_ISDIR(st.st_mode) ? rmdir(buf) : unlink(buf)) == 0) ? 1 : 0;
  }
  // FileAccessHandler
  if (!strcmp(name, "isFileEof")) {
    GdFile *g = gd_file_get(va_arg(va, int));
    return g ? (juint)(g->eof || feof(g->f)) : 1;
  }
  if (!strcmp(name, "fileWrite")) {
    GdFile *g = gd_file_get(va_arg(va, int));
    FakeObject *bb = va_arg(va, void *);
    if (!g || !bb || bb->tag != TAG_OBJECT || bb->kind != K_BYTEBUF) return 0;
    return fwrite(bb->ptr, 1, (size_t)bb->cap, g->f) == (size_t)bb->cap ? 1 : 0;
  }
  // gaps found by the APK cross-reference (docs/JNI_SURFACE.md). Last, so the
  // handlers above -- which are hardware-tested in drmariomania_nx -- win.
  TRY_EXT_INT(godot_ext_int);
  TRY_EXT_INT(godot_io_ext_int);
  TRY_EXT_INT(godot_renderview_ext);
  return 0;
}

static juint call_int(void *recv, FakeID *id, va_list va) {
  const char *name = id->name;
  (void)recv;
  TRY_FMOD((juint)fr.i);

  // Godot 3.x: OS_Android::initialize picks GLES3 only when this reports
  // >= 0x30000 (and the project asks for GLES3). The unhandled default of 0
  // would force GLES2 or no renderer at all. Mesa here is OpenGL ES 3.2.
  if (!strcmp(name, "getGLESVersionCode"))    return config.gles2 ? 0x00020000 : 0x00030002;
  // GodotIO
  if (!strcmp(name, "getScreenDPI"))          return 236; // 6.2" 1280x720 panel
  if (!strcmp(name, "getScreenOrientation"))  return 0;   // landscape
  if (!strcmp(name, "getDisplayRotation"))    return 0;
  if (!strcmp(name, "getAccentColor"))        return 0;
  if (!strcmp(name, "getBaseColor"))          return 0;
  if (!strcmp(name, "createNewGodotInstance")) return 0;
  if (!strcmp(name, "nativeVerifyApk"))       return 1; // ERR_UNAVAILABLE-ish
  // DirectoryAccessHandler
  if (!strcmp(name, "dirOpen")) {
    int at = va_arg(va, int);
    const char *p = obj_str(va_arg(va, void *));
    return (juint)(int64_t)gd_dir_open(at, p);
  }
  if (!strcmp(name, "getDriveCount")) return 0;
  // FileAccessHandler
  if (!strcmp(name, "fileOpen")) {
    const char *p = obj_str(va_arg(va, void *));
    int mode = va_arg(va, int);
    return (juint)(int64_t)gd_file_open(p, mode);
  }
  if (!strcmp(name, "fileRead")) {
    GdFile *g = gd_file_get(va_arg(va, int));
    FakeObject *bb = va_arg(va, void *);
    if (!g || !bb || bb->tag != TAG_OBJECT || bb->kind != K_BYTEBUF) return 0;
    size_t got = fread(bb->ptr, 1, (size_t)bb->cap, g->f);
    if (got < (size_t)bb->cap) g->eof = 1;
    return (juint)got;
  }
  if (!strcmp(name, "fileResize")) {
    GdFile *g = gd_file_get(va_arg(va, int));
    int64_t len = va_arg(va, int64_t);
    if (!g) return 1; // FAILED
    fflush(g->f);
    if (ftruncate(fileno(g->f), (off_t)len) != 0) return 1;
    if (g->size > len) g->size = len;
    return 0; // OK
  }
  TRY_EXT_INT(godot_ext_int);
  TRY_EXT_INT(godot_io_ext_int);
  TRY_EXT_INT(godot_renderview_ext);
  return 0;
}

static juint call_long(void *recv, FakeID *id, va_list va) {
  const char *name = id->name;
  (void)recv;
  TRY_FMOD((juint)fr.j);

  // FileAccessHandler
  if (!strcmp(name, "fileGetSize")) {
    GdFile *g = gd_file_get(va_arg(va, int));
    return g ? (juint)g->size : 0;
  }
  if (!strcmp(name, "fileGetPosition")) {
    GdFile *g = gd_file_get(va_arg(va, int));
    return g ? (juint)ftell(g->f) : 0;
  }
  if (!strcmp(name, "fileSize") || !strcmp(name, "fileLastModified") || !strcmp(name, "fileLastAccessed")) {
    const char *p = obj_str(va_arg(va, void *));
    char buf[512];
    struct stat st;
    if (stat(resolve_gd_path(ACCESS_FILESYSTEM, p, buf, sizeof(buf)), &st) != 0)
      return !strcmp(name, "fileSize") ? (juint)-1 : 0;
    return !strcmp(name, "fileSize") ? (juint)st.st_size : (juint)st.st_mtime;
  }
  // DirectoryAccessHandler
  if (!strcmp(name, "getSpaceLeft")) return 1024ull * 1024 * 1024; // 1 GiB
  return 0;
}

static void *call_object(void *recv, FakeID *id, va_list va) {
  const char *name = id->name;
  FakeObject *o = recv;

  // org/fmod/* first. The only object-returning method in that surface is
  // FMOD.getAssetManager(), which must hand back the SAME AssetManager the
  // engine already holds so both go through one AAssetManager emulation.
  TRY_FMOD(fr.l);

  // ClassLoader.loadClass(String)Class / findClass.
  //
  // Returning NULL here (which is what the unhandled path did, and what my
  // first fix made explicit) makes Godot's jni_find_class fail. The first
  // hardware run showed twelve of them: java/lang/Class, reflect/Constructor,
  // reflect/Method, reflect/Field, and the boxed primitives Boolean..Double.
  // Those are the types Godot's variant marshalling boxes through, so handing
  // back NULL breaks JNI interop rather than politely declining it.
  //
  // The right answer is the same named class object j_FindClass vends -- the
  // pool is keyed by name and the object is never dereferenced as a real
  // jclass, only compared and passed back to GetMethodID.
  if (!strcmp(name, "loadClass") || !strcmp(name, "findClass")) {
    FakeString *arg = va_arg(va, void *);
    const char *cls_name = (arg && arg->tag == TAG_STRING) ? arg->utf : NULL;
    if (!cls_name) return NULL;

    // Java uses dots in loadClass names ("java.lang.Integer") but slashes in
    // FindClass ("java/lang/Integer"). Normalise so both reach one pool entry.
    char norm[96];
    size_t n = 0;
    for (const char *c = cls_name; *c && n < sizeof(norm) - 1; c++, n++)
      norm[n] = (*c == '.') ? '/' : *c;
    norm[n] = '\0';

    return find_class_obj(norm);
  }

  // Godot class
  if (!strcmp(name, "getActivity"))     return &g_activity;
  if (!strcmp(name, "getClipboard"))    return jni_make_string("");
  if (!strcmp(name, "getCACertificates")) return jni_make_string("");
  if (!strcmp(name, "getInputFallbackMapping")) return jni_make_string("");
  if (!strcmp(name, "getGrantedPermissions"))   return new_obj_array(0);
  if (!strcmp(name, "getGDExtensionConfigFiles")) return new_obj_array(0);
  // must be non-null: GodotJavaWrapper::get_godot_view() wraps it and
  // DisplayServerAndroid calls methods on the wrapper without a null check
  // (can_capture_pointer reads this+0x28 -> the black-screen Data Abort)
  if (!strcmp(name, "getRenderView")) {
    debugPrintf("[jni] getRenderView -> %p\n", (void *)&g_renderview);
    return &g_renderview;
  }
  if (!strcmp(name, "getClassLoader"))  return make_generic(K_GENERIC);
  // GodotIO
  if (!strcmp(name, "getLocale"))    return jni_make_string(DEVICE_LOCALE);
  if (!strcmp(name, "getModel"))     return jni_make_string("Nintendo Switch");
  if (!strcmp(name, "getUniqueID"))  return jni_make_string("nintendo-switch");
  // Logged, because whether this is called at all is the open question.
  //
  // The engine's directory calls arrive as "//localization_override" and
  // "/.godot/mono/publish/arm64" -- a leading empty component, which is what
  // user:// looks like when the data dir resolves to nothing. Our handler
  // prepends save_root and rescues those, but ACCESS_USERDATA goes through
  // DirAccessUnix and never reaches us, so user://shader_cache lands at
  // /shader_cache instead of <save_root>/shader_cache.
  //
  // If this line never appears, the engine is not asking and its idea of the
  // data dir comes from somewhere else. If it appears with the right value,
  // the fault is downstream of us.
  if (!strcmp(name, "getDataDir")) {
    debugPrintf("[jni] getDataDir -> \"%s\"\n", config.save_root);
    return jni_make_string(config.save_root);
  }
  if (!strcmp(name, "getCacheDir") || !strcmp(name, "getTempDir")) {
    char buf[300];
    snprintf(buf, sizeof(buf), "%s/cache", config.save_root);
    return jni_make_string(buf);
  }
  if (!strcmp(name, "getSystemDir")) { // (int idx, bool shared)
    (void)va_arg(va, int);
    return jni_make_string(config.save_root);
  }
  // Godot 3.x spellings: the whole window is safe; the surface is our
  // singleton (the wrapper owns the EGL surface, nothing dereferences it).
  if (!strcmp(name, "getSurface")) return &g_surface;
  if (!strcmp(name, "getDisplaySafeArea") || !strcmp(name, "getWindowSafeArea")) {
    int *r = calloc(4, sizeof(int));
    r[0] = 0; r[1] = 0; r[2] = screen_width; r[3] = screen_height;
    return make_pri_array_adopt(r, 4, 4);
  }
  if (!strcmp(name, "getDisplayCutouts")) return new_pri_array(0, 4);
  // DirectoryAccessHandler
  if (!strcmp(name, "dirNext")) {
    const char *e = gd_dir_next(va_arg(va, int));
    return e ? jni_make_string(e) : jni_make_string(""); // "" = end of listing
  }
  if (!strcmp(name, "getDrive")) return jni_make_string("");
  // generic Context-ish getters some plugins/glue probe
  if (!strcmp(name, "getAssets")) return &g_assetmgr;
  if (!strcmp(name, "getAbsolutePath") || !strcmp(name, "getCanonicalPath") || !strcmp(name, "getPath"))
    return jni_make_string(o && o->tag == TAG_OBJECT ? o->str : "");
#if DEBUG_LOG
  debugPrintf("[jni] unhandled object method: %s %s\n", name, id->sig);
#endif

  { void *r;
    if (godot_dictionary_ext(name, &r)) return r; }
  return NULL;
}

static void call_void(void *recv, FakeID *id, va_list va) {
  const char *name = id->name;
  (void)recv;
  // org/fmod/* first: the namespaces are disjoint from org/godotengine/*, and
  // routing on the recorded class avoids the init()/close() collisions between
  // FMOD, AudioDevice and MediaCodec that name-only dispatch would produce.
  TRY_FMOD_VOID();

  // FileAccessHandler
  if (!strcmp(name, "fileSeek")) {
    GdFile *g = gd_file_get(va_arg(va, int));
    int64_t pos = va_arg(va, int64_t);
    if (g) { fseek(g->f, (long)pos, SEEK_SET); g->eof = 0; }
    return;
  }
  if (!strcmp(name, "fileSeekFromEnd")) {
    GdFile *g = gd_file_get(va_arg(va, int));
    int64_t pos = va_arg(va, int64_t);
    if (g) { fseek(g->f, (long)-pos, SEEK_END); g->eof = 0; }
    return;
  }
  if (!strcmp(name, "fileClose")) { gd_file_close(va_arg(va, int)); return; }
  if (!strcmp(name, "fileFlush")) {
    GdFile *g = gd_file_get(va_arg(va, int));
    if (g) fflush(g->f);
    return;
  }
  if (!strcmp(name, "setFileEof")) {
    GdFile *g = gd_file_get(va_arg(va, int));
    if (g) g->eof = va_arg(va, int);
    return;
  }
  // DirectoryAccessHandler
  if (!strcmp(name, "dirClose")) { gd_dir_close(va_arg(va, int)); return; }
#if DEBUG_LOG
  if (!strcmp(name, "alert")) {
    const char *msg = obj_str(va_arg(va, void *));
    const char *title = obj_str(va_arg(va, void *));
    debugPrintf("[jni] alert: %s: %s\n", title, msg);
    return;
  }
#endif
  // everything else (lifecycle notifications, vibrate, keyboard, immersive
  // mode, benchmarks, TTS, multicast locks, ...) is a safe no-op
  TRY_EXT_VOID(godot_ext_void);
  TRY_EXT_VOID_INT(godot_io_ext_int);
}

static float call_float(void *recv, FakeID *id, va_list va) {
  (void)recv; (void)va;
  if (!strcmp(id->name, "getScaledDensity")) return 1.0f;
  if (!strcmp(id->name, "getRefreshRate"))   return 60.0f;
  return 0.0f;
}

static double call_double(void *recv, FakeID *id, va_list va) {
  (void)recv; (void)va;
  if (!strcmp(id->name, "getScreenRefreshRate")) return 60.0;
  return 0.0;
}

// ---------------------------------------------------------------------------
// field access
// ---------------------------------------------------------------------------

static juint get_int_field(void *obj, FakeID *id) {
  (void)obj;
  if (!strcmp(id->name, "SDK_INT"))     return 30; // Android 11: modern enough
  if (!strcmp(id->name, "widthPixels")) return (juint)screen_width;
  if (!strcmp(id->name, "heightPixels"))return (juint)screen_height;
  if (!strcmp(id->name, "densityDpi"))  return 236;
  return 0;
}

static float get_float_field(void *obj, FakeID *id) {
  (void)obj;
  if (!strcmp(id->name, "xdpi") || !strcmp(id->name, "ydpi")) return 236.0f;
  if (!strcmp(id->name, "density") || !strcmp(id->name, "scaledDensity")) return 1.0f;
  return 0.0f;
}

static void *get_object_field(void *obj, FakeID *id) {
  (void)obj;
  if (!strcmp(id->name, "WINDOW_SERVICE")) return jni_make_string("window");
  return NULL;
}

// ---------------------------------------------------------------------------
// JNIEnv function implementations
// ---------------------------------------------------------------------------

static juint j_GetVersion(void *env) { (void)env; return JNI_VERSION_1_6; }
static void *j_FindClass(void *env, const char *name) { (void)env; return find_class_obj(name); }
static void *j_GetObjectClass(void *env, void *obj) { (void)env; (void)obj; return &g_cls_generic; }
static void *j_GetMethodID(void *env, void *cls, const char *name, const char *sig) {
  (void)env;
  FakeObject *c = (FakeObject *)cls;
  const char *cname = (c && c->tag == TAG_CLASS) ? c->str : "";
  return get_id(cname, name, sig);
}

static void *j_NewGlobalRef(void *env, void *obj) {
  (void)env;
  mutexLock(&locals_lock);
  for (int i = locals_top - 1; i >= 0; i--)
    if (locals[i] == obj) { locals[i] = locals[--locals_top]; break; }
  mutexUnlock(&locals_lock);
  return obj;
}
static void j_DeleteGlobalRef(void *env, void *obj) { (void)env; free_ref(obj); }
static void j_DeleteLocalRef(void *env, void *obj) { (void)env; delete_local(obj); }
static void *j_NewLocalRef(void *env, void *obj) { (void)env; return obj; }
static juint j_IsSameObject(void *env, void *a, void *b) { (void)env; return a == b; }
static juint j_EnsureLocalCapacity(void *env, int cap) { (void)env; (void)cap; return 0; }

// Returns JNI_ERR when it cannot push, instead of 0.
//
// This used to drop the frame silently and report success. The matching
// PopLocalFrame then consumed an OUTER frame's mark and freed references that
// outer scope still owned -- the engine carried on using a freed jobject.
//
// That is cumulative and order-dependent rather than tied to any one action,
// which is exactly how the crash behaves: potions are fine in one fight and not
// another, and doing the same things in a different order changes the outcome.
// Nesting depth is a property of how much has happened, not of what was done.
static juint j_PushLocalFrame(void *env, int cap) {
  (void)env; (void)cap;
  mutexLock(&locals_lock);
  const int ok = frame_top < MAX_FRAMES;
  if (ok) {
    frames[frame_top++] = locals_top;
  } else if (frames_dropped++ == 0) {
    debugPrintf("[jni] PushLocalFrame: %d frames deep, cannot nest further. "
                "Returning JNI_ERR rather than dropping it silently -- a "
                "dropped push makes the next pop free an outer frame's refs.\n",
                MAX_FRAMES);
    debugFlush();
  }
  mutexUnlock(&locals_lock);
  return ok ? 0 : (juint)-1;   // JNI_ERR
}
static void *j_PopLocalFrame(void *env, void *result) {
  (void)env;
  mutexLock(&locals_lock);

  // An unmatched pop used to fall back to mark = 0, which frees EVERY tracked
  // local reference in the process -- not just this frame's. One unbalanced pop
  // would take out references belonging to every live scope at once.
  //
  // Freeing nothing leaks; freeing everything corrupts. Leak.
  if (frame_top <= 0) {
    if (unbalanced_pops++ == 0) {
      debugPrintf("[jni] PopLocalFrame with no matching push -- freeing nothing. "
                  "Falling back to mark 0 would have released every live local "
                  "ref in the process.\n");
      debugFlush();
    }
    mutexUnlock(&locals_lock);
    return result;
  }

  const int mark = frames[--frame_top];
  for (int i = mark; i < locals_top; i++)
    if (locals[i] != result) free_ref(locals[i]);
  locals_top = mark;
  if (result && locals_top < MAX_LOCALS) locals[locals_top++] = result;
  mutexUnlock(&locals_lock);
  return result;
}

#define CALL_VARIADIC(fn, ret_t, dispatch) \
  static ret_t fn(void *env, void *recv, FakeID *id, ...) { \
    (void)env; va_list va; va_start(va, id); \
    ret_t r = dispatch(recv, id, va); va_end(va); return r; } \
  static ret_t fn##V(void *env, void *recv, FakeID *id, va_list va) { \
    (void)env; return dispatch(recv, id, va); }

CALL_VARIADIC(j_CallObjectMethod, void *, call_object)
CALL_VARIADIC(j_CallIntMethod, juint, call_int)
CALL_VARIADIC(j_CallBooleanMethod, juint, call_boolean)
CALL_VARIADIC(j_CallLongMethod, juint, call_long)
CALL_VARIADIC(j_CallFloatMethod, float, call_float)
CALL_VARIADIC(j_CallDoubleMethod, double, call_double)

static void j_CallVoidMethod(void *env, void *recv, FakeID *id, ...) {
  (void)env; va_list va; va_start(va, id); call_void(recv, id, va); va_end(va);
}
static void j_CallVoidMethodV(void *env, void *recv, FakeID *id, va_list va) {
  (void)env; call_void(recv, id, va);
}

#define j_CallStaticObjectMethod   j_CallObjectMethod
#define j_CallStaticObjectMethodV  j_CallObjectMethodV
#define j_CallStaticIntMethod      j_CallIntMethod
#define j_CallStaticIntMethodV     j_CallIntMethodV
#define j_CallStaticBooleanMethod  j_CallBooleanMethod
#define j_CallStaticBooleanMethodV j_CallBooleanMethodV
#define j_CallStaticLongMethod     j_CallLongMethod
#define j_CallStaticLongMethodV    j_CallLongMethodV
#define j_CallStaticFloatMethod    j_CallFloatMethod
#define j_CallStaticFloatMethodV   j_CallFloatMethodV
#define j_CallStaticDoubleMethod   j_CallDoubleMethod
#define j_CallStaticDoubleMethodV  j_CallDoubleMethodV
#define j_CallStaticVoidMethod     j_CallVoidMethod
#define j_CallStaticVoidMethodV    j_CallVoidMethodV

// strings
static void *j_NewStringUTF(void *env, const char *utf) { (void)env; return jni_make_string(utf); }
static const char *j_GetStringUTFChars(void *env, void *jstr, uint8_t *is_copy) {
  (void)env; if (is_copy) *is_copy = 0; return obj_str(jstr);
}
static void j_ReleaseStringUTFChars(void *env, void *jstr, const char *utf) { (void)env; (void)jstr; (void)utf; }
static juint j_GetStringUTFLength(void *env, void *jstr) { (void)env; return strlen(obj_str(jstr)); }
static juint j_GetStringLength(void *env, void *jstr) { (void)env; return strlen(obj_str(jstr)); }

// arrays
static juint j_GetArrayLength(void *env, void *arr) {
  (void)env;
  FakePriArray *a = arr;
  if (a && (a->tag == TAG_PRIARR || a->tag == TAG_OBJARR)) return a->len;
  return 0;
}
static void *j_NewByteArray(void *env, int len) { (void)env; return new_pri_array(len, 1); }
static void *j_NewShortArray(void *env, int len) { (void)env; return new_pri_array(len, 2); }
static void *j_NewIntArray(void *env, int len) { (void)env; return new_pri_array(len, 4); }
static void *j_NewLongArray(void *env, int len) { (void)env; return new_pri_array(len, 8); }
static void *j_NewFloatArray(void *env, int len) { (void)env; return new_pri_array(len, 4); }
static void *j_NewDoubleArray(void *env, int len) { (void)env; return new_pri_array(len, 8); }

static void *j_GetPriArrayElements(void *env, void *arr, uint8_t *is_copy) {
  (void)env; if (is_copy) *is_copy = 0;
  FakePriArray *a = arr;
  return (a && a->tag == TAG_PRIARR) ? a->data : NULL;
}
static void j_ReleasePriArrayElements(void *env, void *arr, void *elems, int mode) {
  (void)env; (void)arr; (void)elems; (void)mode;
}
static void j_GetPriArrayRegion(void *env, void *arr, int start, int len, void *buf) {
  (void)env;
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR && start >= 0 && start + len <= a->len)
    memcpy(buf, (char *)a->data + (size_t)start * a->elem_size, (size_t)len * a->elem_size);
}
static void j_SetPriArrayRegion(void *env, void *arr, int start, int len, const void *buf) {
  (void)env;
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR && start >= 0 && start + len <= a->len)
    memcpy((char *)a->data + (size_t)start * a->elem_size, buf, (size_t)len * a->elem_size);
}

// fields
//
// Same class-aware treatment as j_GetMethodID: the FakeID pool is keyed on
// (class, name, signature), so a field ID must carry its class too or it can
// alias a method ID with the same name+signature. Field and method IDs share
// one pool, which is fine as long as the key is complete.
static void *j_GetFieldID(void *env, void *cls, const char *name, const char *sig) {
  (void)env;
  FakeObject *c = (FakeObject *)cls;
  const char *cname = (c && c->tag == TAG_CLASS) ? c->str : "";
  return get_id(cname, name, sig);
}
static juint j_GetIntField(void *env, void *obj, FakeID *id) { (void)env; return get_int_field(obj, id); }
static juint j_GetLongField(void *env, void *obj, FakeID *id) { (void)env; (void)obj; (void)id; return 0; }
static float j_GetFloatField(void *env, void *obj, FakeID *id) { (void)env; return get_float_field(obj, id); }
static void *j_GetObjectField(void *env, void *obj, FakeID *id) { (void)env; return get_object_field(obj, id); }

// direct byte buffers: wrap {addr, cap} so the file handlers can honor the
// requested length (godot's fileRead passes the destination this way)
static void *j_NewDirectByteBuffer(void *env, void *addr, int64_t cap) {
  (void)env;
  FakeObject *o = calloc(1, sizeof(*o));
  o->tag = TAG_OBJECT;
  o->kind = K_BYTEBUF;
  o->ptr = addr;
  o->cap = cap;
  return reg_local(o);
}
static void *j_GetDirectBufferAddress(void *env, void *buf) {
  (void)env;
  FakeObject *o = buf;
  if (o && o->tag == TAG_OBJECT && o->kind == K_BYTEBUF) return o->ptr;
  return buf;
}
static int64_t j_GetDirectBufferCapacity(void *env, void *buf) {
  (void)env;
  FakeObject *o = buf;
  if (o && o->tag == TAG_OBJECT && o->kind == K_BYTEBUF) return o->cap;
  return 0;
}

// misc
static juint j_RegisterNatives(void *env, void *cls, void *methods, int n) {
  (void)env; (void)cls; (void)methods; (void)n; return 0;
}
static juint j_GetJavaVM(void *env, void **vm) { (void)env; *vm = fake_vm; return JNI_OK; }
static juint j_ExceptionCheck(void *env) { (void)env; return 0; }
static void *j_ExceptionOccurred(void *env) { (void)env; return NULL; }
static void *j_NewObjectArray(void *env, int len, void *cls, void *init) {
  (void)env; (void)cls;
  FakeObjArray *a = new_obj_array(len);
  if (init)
    for (int i = 0; i < len; i++) a->items[i] = init;
  return a;
}
static void *j_GetObjectArrayElement(void *env, void *arr, int idx) {
  (void)env;
  FakeObjArray *a = arr;
  if (a && a->tag == TAG_OBJARR && idx >= 0 && idx < a->len) return a->items[idx];
  return NULL;
}
static void j_SetObjectArrayElement(void *env, void *arr, int idx, void *val) {
  (void)env;
  FakeObjArray *a = arr;
  if (a && a->tag == TAG_OBJARR && idx >= 0 && idx < a->len) a->items[idx] = val;
}
static juint j_IsInstanceOf(void *env, void *obj, void *cls) { (void)env; (void)obj; (void)cls; return 1; }
static void j_void1(void *env) { (void)env; }
// j_unimplemented removed: replaced by the per-slot stubs in jni_unimpl.h,
// which say WHICH slot was called instead of returning 0 anonymously.

// ---------------------------------------------------------------------------
// table assembly (indices per the JNI specification)
// ---------------------------------------------------------------------------

static void *env_table[256];
static void **env_table_ptr = env_table;
void *fake_env = &env_table_ptr;

static juint vm_DestroyJavaVM(void *vm) { (void)vm; return JNI_OK; }
static juint vm_AttachCurrentThread(void *vm, void **env, void *args) {
  (void)vm; (void)args;
  tls_setup_guard(); // engine threads attach before calling back into "java"
  if (env) *env = fake_env;
  return JNI_OK;
}
static juint vm_DetachCurrentThread(void *vm) { (void)vm; return JNI_OK; }
static juint vm_GetEnv(void *vm, void **env, int version) {
  (void)vm; (void)version; if (env) *env = fake_env; return JNI_OK;
}
static void *vm_table[8];
static void **vm_table_ptr = vm_table;
void *fake_vm = &vm_table_ptr;

// Depth and drop counters. The one-shot warnings say a limit was hit; this says
// how close things run in a healthy frame, which is what tells you whether a
// limit is comfortable or one busy scene away from being hit.
void jni_report(void) {
  mutexLock(&locals_lock);
  const int lt = locals_top, ft = frame_top;
  const uint64_t ld = locals_dropped, fd = frames_dropped, up = unbalanced_pops;
  mutexUnlock(&locals_lock);

  if (ld || fd || up)
    debugPrintf("[jni] locals %d/%d, frames %d/%d; dropped %llu ref(s), "
                "%llu frame(s), %llu unmatched pop(s)\n",
                lt, MAX_LOCALS, ft, MAX_FRAMES,
                (unsigned long long)ld, (unsigned long long)fd,
                (unsigned long long)up);
}

void jni_init(void) {
  mutexInit(&locals_lock);
  mutexInit(&id_lock);
  mutexInit(&gd_dir_lock);
  mutexInit(&gd_file_lock);
  init_singletons();

  // One stub PER SLOT, so an unimplemented call names itself.
  //
  // This was a single j_unimplemented() returning 0 in silence. The engine
  // then treated 0 as a real jobject / jfieldID / length and carried on
  // building something half-initialised -- which is precisely the object shape
  // the repeating crash walks. See jni_unimpl.h.
  for (int i = 0; i < 256; i++) env_table[i] = ju_slots[i];

  env_table[4]   = (void *)j_GetVersion;
  env_table[6]   = (void *)j_FindClass;
  env_table[15]  = (void *)j_ExceptionOccurred;
  env_table[16]  = (void *)j_void1; // ExceptionDescribe
  env_table[17]  = (void *)j_void1; // ExceptionClear
  env_table[19]  = (void *)j_PushLocalFrame;
  env_table[20]  = (void *)j_PopLocalFrame;
  env_table[21]  = (void *)j_NewGlobalRef;
  env_table[22]  = (void *)j_DeleteGlobalRef;
  env_table[23]  = (void *)j_DeleteLocalRef;
  env_table[24]  = (void *)j_IsSameObject;
  env_table[25]  = (void *)j_NewLocalRef;
  env_table[26]  = (void *)j_EnsureLocalCapacity;
  env_table[31]  = (void *)j_GetObjectClass;
  env_table[32]  = (void *)j_IsInstanceOf;
  env_table[33]  = (void *)j_GetMethodID;
  env_table[34]  = (void *)j_CallObjectMethod;
  env_table[35]  = (void *)j_CallObjectMethodV;
  env_table[37]  = (void *)j_CallBooleanMethod;
  env_table[38]  = (void *)j_CallBooleanMethodV;
  env_table[49]  = (void *)j_CallIntMethod;
  env_table[50]  = (void *)j_CallIntMethodV;
  env_table[52]  = (void *)j_CallLongMethod;
  env_table[53]  = (void *)j_CallLongMethodV;
  env_table[55]  = (void *)j_CallFloatMethod;
  env_table[56]  = (void *)j_CallFloatMethodV;
  env_table[58]  = (void *)j_CallDoubleMethod;
  env_table[59]  = (void *)j_CallDoubleMethodV;
  env_table[61]  = (void *)j_CallVoidMethod;
  env_table[62]  = (void *)j_CallVoidMethodV;
  env_table[94]  = (void *)j_GetFieldID;
  env_table[95]  = (void *)j_GetObjectField;
  env_table[100] = (void *)j_GetIntField;
  env_table[101] = (void *)j_GetLongField;
  env_table[102] = (void *)j_GetFloatField;
  env_table[113] = (void *)j_GetMethodID;            // GetStaticMethodID
  env_table[114] = (void *)j_CallStaticObjectMethod;
  env_table[115] = (void *)j_CallStaticObjectMethodV;
  env_table[117] = (void *)j_CallStaticBooleanMethod;
  env_table[118] = (void *)j_CallStaticBooleanMethodV;
  env_table[129] = (void *)j_CallStaticIntMethod;
  env_table[130] = (void *)j_CallStaticIntMethodV;
  env_table[132] = (void *)j_CallStaticLongMethod;
  env_table[133] = (void *)j_CallStaticLongMethodV;
  env_table[135] = (void *)j_CallStaticFloatMethod;
  env_table[136] = (void *)j_CallStaticFloatMethodV;
  env_table[138] = (void *)j_CallStaticDoubleMethod;
  env_table[139] = (void *)j_CallStaticDoubleMethodV;
  env_table[141] = (void *)j_CallStaticVoidMethod;
  env_table[142] = (void *)j_CallStaticVoidMethodV;
  env_table[144] = (void *)j_GetFieldID;             // GetStaticFieldID
  env_table[145] = (void *)j_GetObjectField;         // GetStaticObjectField
  env_table[146] = (void *)j_GetIntField;            // GetStaticBooleanField (int-width ok)
  env_table[150] = (void *)j_GetIntField;            // GetStaticIntField (SDK_INT)
  env_table[164] = (void *)j_GetStringLength;
  env_table[167] = (void *)j_NewStringUTF;
  env_table[168] = (void *)j_GetStringUTFLength;
  env_table[169] = (void *)j_GetStringUTFChars;
  env_table[170] = (void *)j_ReleaseStringUTFChars;
  env_table[171] = (void *)j_GetArrayLength;
  env_table[172] = (void *)j_NewObjectArray;
  env_table[173] = (void *)j_GetObjectArrayElement;
  env_table[174] = (void *)j_SetObjectArrayElement;
  env_table[176] = (void *)j_NewByteArray;
  env_table[178] = (void *)j_NewShortArray;
  env_table[179] = (void *)j_NewIntArray;
  env_table[180] = (void *)j_NewLongArray;
  env_table[181] = (void *)j_NewFloatArray;
  env_table[182] = (void *)j_NewDoubleArray;
  for (int i = 183; i <= 190; i++) env_table[i] = (void *)j_GetPriArrayElements;
  for (int i = 191; i <= 198; i++) env_table[i] = (void *)j_ReleasePriArrayElements;
  for (int i = 199; i <= 206; i++) env_table[i] = (void *)j_GetPriArrayRegion;
  for (int i = 207; i <= 214; i++) env_table[i] = (void *)j_SetPriArrayRegion;
  env_table[215] = (void *)j_RegisterNatives;
  env_table[219] = (void *)j_GetJavaVM;
  env_table[222] = (void *)j_GetPriArrayElements;     // GetPrimitiveArrayCritical
  env_table[223] = (void *)j_ReleasePriArrayElements; // ReleasePrimitiveArrayCritical
  env_table[226] = (void *)j_NewGlobalRef;            // NewWeakGlobalRef
  env_table[227] = (void *)j_DeleteGlobalRef;         // DeleteWeakGlobalRef
  env_table[228] = (void *)j_ExceptionCheck;
  env_table[229] = (void *)j_NewDirectByteBuffer;
  env_table[230] = (void *)j_GetDirectBufferAddress;
  env_table[231] = (void *)j_GetDirectBufferCapacity;

  // Slots 0-2 are reserved by the JNI spec and were left NULL. A static array
  // zero-fills, so a call into one of them jumps through a null function
  // pointer -- an instruction abort with pc == lr == 0, which is what run 35
  // looked like. Filling them costs nothing and turns a jump-to-zero into a
  // log line.
  for (int i = 0; i < 8; i++) vm_table[i] = ju_slots[i];

  vm_table[3] = (void *)vm_DestroyJavaVM;
  vm_table[4] = (void *)vm_AttachCurrentThread;
  vm_table[5] = (void *)vm_DetachCurrentThread;
  vm_table[6] = (void *)vm_GetEnv;
  vm_table[7] = (void *)vm_AttachCurrentThread; // AttachCurrentThreadAsDaemon
}
