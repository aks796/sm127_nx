/* godot_shim.c -- bionic/NDK shims added for libgodot_android.so (Godot 4.6):
 * bionic dirent conversion, rwlocks, AAssetManager over the assets/ dir,
 * and assorted linux-isms newlib lacks. Camera/media NDK stubs live in
 * imports.c as plain ret0/retm1 entries. MIT license; see LICENSE. */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <ctype.h>
#include <math.h>
#include <wchar.h>
#include <wctype.h>
#include <time.h>
#include <sys/stat.h>
#include <switch.h>

#include "config.h"
#include "imports.h"      // sm127_plausible_lock_ptr, sm127_note_bad_lock_slot
#include "godot_shim.h"
#include "libc_shim.h"
#include "util.h"

// ---------------------------------------------------------------------------
// bionic dirent: { u64 d_ino; s64 d_off; u16 d_reclen; u8 d_type; char d_name[256]; }
// newlib's struct dirent differs, so wrap the directory stream and convert.
// ---------------------------------------------------------------------------

struct bionic_dirent {
  uint64_t d_ino;
  int64_t  d_off;
  uint16_t d_reclen;
  uint8_t  d_type;
  char     d_name[256];
};

#define BIONIC_DT_UNKNOWN 0
#define BIONIC_DT_DIR     4
#define BIONIC_DT_REG     8

typedef struct {
  uint32_t magic; // 'BDIR'
  DIR *dir;
  char path[512];
  struct bionic_dirent ent;
} FakeDir;

#define FAKEDIR_MAGIC 0x42444952

void *opendir_fake(const char *path) {
  if (!path) return NULL;
  char sb[640];
  path = sandbox_path(path, sb, sizeof(sb));
  DIR *d = opendir(path);
  if (!d) return NULL;
  FakeDir *fd = calloc(1, sizeof(*fd));
  if (!fd) { closedir(d); return NULL; }
  fd->magic = FAKEDIR_MAGIC;
  fd->dir = d;
  strncpy(fd->path, path, sizeof(fd->path) - 1);
  return fd;
}

void *fdopendir_fake(int fd) { (void)fd; return NULL; }

void *readdir_fake(void *dirp) {
  FakeDir *fd = dirp;
  if (!fd || fd->magic != FAKEDIR_MAGIC) return NULL;
  struct dirent *e = readdir(fd->dir);
  if (!e) return NULL;
  memset(&fd->ent, 0, sizeof(fd->ent));
  fd->ent.d_ino = 1;
  fd->ent.d_reclen = sizeof(fd->ent);
  // memcpy + explicit terminator rather than strncpy: gcc warns that a
  // 255-byte strncpy from a 255-byte source may not terminate, which is true
  // and is exactly why the terminator is written separately.
  {
    size_t n = strlen(e->d_name);
    if (n > sizeof(fd->ent.d_name) - 1) n = sizeof(fd->ent.d_name) - 1;
    memcpy(fd->ent.d_name, e->d_name, n);
    fd->ent.d_name[n] = '\0';
  }
  // newlib on Switch has no d_type; stat to tell dirs from files (Godot's
  // DirAccessUnix falls back to stat when DT_UNKNOWN, but be explicit).
  char full[768];
  snprintf(full, sizeof(full), "%s/%s", fd->path, e->d_name);
  struct stat st;
  if (stat(full, &st) == 0)
    fd->ent.d_type = S_ISDIR(st.st_mode) ? BIONIC_DT_DIR : BIONIC_DT_REG;
  else
    fd->ent.d_type = BIONIC_DT_UNKNOWN;
  return &fd->ent;
}

int closedir_fake(void *dirp) {
  FakeDir *fd = dirp;
  if (!fd || fd->magic != FAKEDIR_MAGIC) return -1;
  closedir(fd->dir);
  fd->magic = 0;
  free(fd);
  return 0;
}

// ---------------------------------------------------------------------------
// pthread rwlock via libnx RwLock, pointer-indirected like the mutex fakes
// (bionic zero-initializes the storage inline; first use allocates).
// ---------------------------------------------------------------------------

typedef struct { RwLock l; } FakeRwLock;

static FakeRwLock *ensure_rwlock(void **lk) {
  // The same guard the mutex and cond paths have, which this never had: it
  // only checked for NULL, so ANY non-null value in the slot -- including a
  // 32-bit 1 written by something else -- was dereferenced as a FakeRwLock*.
  //
  // Nothing in the engine imports pthread_rwlock, so this path may never run
  // here; that is not a reason to leave the one unguarded copy of a pattern the
  // other two needed a guard for.
  if (*lk && !sm127_plausible_lock_ptr((uint64_t)(uintptr_t)*lk)) {
    sm127_note_bad_lock_slot_at("rwlock", (uint64_t)(uintptr_t)*lk, lk);
    *lk = NULL;
  }

  if (!*lk) {
    FakeRwLock *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    rwlockInit(&r->l);
    // benign race at worst leaks one small object; engine inits these early
    *lk = r;
  }
  return (FakeRwLock *)*lk;
}

int pthread_rwlock_rdlock_fake(void **lk) {
  FakeRwLock *r = ensure_rwlock(lk);
  if (!r) return -1;
  rwlockReadLock(&r->l);
  return 0;
}
int pthread_rwlock_wrlock_fake(void **lk) {
  FakeRwLock *r = ensure_rwlock(lk);
  if (!r) return -1;
  rwlockWriteLock(&r->l);
  return 0;
}
int pthread_rwlock_unlock_fake(void **lk) {
  FakeRwLock *r = (FakeRwLock *)*lk;
  if (!r) return -1;
  // libnx needs the matching unlock; the write path holds the writer lock
  if (rwlockIsWriteLockHeldByCurrentThread(&r->l))
    rwlockWriteUnlock(&r->l);
  else
    rwlockReadUnlock(&r->l);
  return 0;
}

// ---------------------------------------------------------------------------
// misc bionic/linux
// ---------------------------------------------------------------------------

int gettid_fake2(void) {
  u64 id = 1;
  if (R_SUCCEEDED(svcGetThreadId(&id, CUR_THREAD_HANDLE)) && id)
    return (int)(id & 0x7fffffff);
  return 1;
}

int pthread_gettid_np_fake(void *thread) { (void)thread; return gettid_fake2(); }

// AT_* values (Linux/bionic aarch64 auxv). Previously returned 0
// unconditionally for every type -- harmless for the GDScript engine, but
// StS2.so's NativeAOT runtime queries AT_HWCAP for baseline AArch64
// features (FP+ASIMD are architecturally mandatory, always present) during
// early bootstrap; reporting zero capabilities is an impossible state a
// runtime built for aarch64 may not tolerate silently. Real HWCAP values
// (matching what reference/nativeaot-celeste64/nativeaot_shims.c already
// established for the same real CPU, Switch's Cortex-A57) fix that; not
// confirmed yet whether this alone is the hardware abort's actual cause.
#define AT_HWCAP_  16
#define AT_HWCAP2_ 26
#define AT_PAGESZ_ 6
unsigned long getauxval_fake(unsigned long type) {
  switch (type) {
    case AT_HWCAP_:  return (1<<0) | (1<<1) | (1<<3) | (1<<4) | (1<<5) | (1<<6) | (1<<7); // FP|ASIMD|AES|PMULL|SHA1|SHA2|CRC32
    case AT_HWCAP2_: return 0;
    case AT_PAGESZ_: return 0x1000;
    default: return 0;
  }
}

int __system_property_get_fake(const char *name, char *value) {
  (void)name;
  if (value) value[0] = 0;
  return 0;
}

struct bionic_rlimit { uint64_t rlim_cur, rlim_max; };
#define BIONIC_RLIMIT_AS 9 // bionic/Linux resource-index constant, not newlib's own RLIMIT_AS
int getrlimit_fake(int res, void *rlim) {
  struct bionic_rlimit *r = rlim;
  if (!r) return 0;
  if (res == BIONIC_RLIMIT_AS) {
    // NativeAOT's GCToOSInterface::GetVirtualMemoryLimit() calls
    // getrlimit(RLIMIT_AS, ...) to size the GC's region_range (PORT_NOTES.md
    // "WKS::GCHeap::Initialize() -> 0x8007000e"): returning the 8MB stack
    // cap below here made region_range collapse to ~4MB, well under the
    // ~19-76MB a single region needs, so GC init silently failed with
    // E_OUTOFMEMORY before ever attempting a real allocation. RLIM_INFINITY
    // (~0ull) matches a real Linux default (address space is unlimited
    // unless explicitly capped) and makes the caller's own "rlim_cur==-1"
    // fallback (a fixed 128TB constant) kick in, exactly as intended.
    r->rlim_cur = r->rlim_max = ~0ull;
  } else {
    r->rlim_cur = r->rlim_max = 8ull * 1024 * 1024; // RLIMIT_STACK etc.: plausible stack cap
  }
  return 0;
}

// no symlinks on fatfs: canonicalization is a plain copy
char *realpath_fake(const char *path, char *resolved) {
  if (!path) { errno = EINVAL; return NULL; }
  char *out = resolved ? resolved : malloc(4096);
  if (!out) return NULL;
  strncpy(out, path, 4095);
  out[4095] = 0;
  return out;
}

int mkstemp_fake(char *tmpl) {
  if (!tmpl) { errno = EINVAL; return -1; }
  size_t l = strlen(tmpl);
  if (l < 6) { errno = EINVAL; return -1; }
  static int counter = 0;
  for (int tries = 0; tries < 100; tries++) {
    // %06d of a value already reduced mod 1000000 is exactly 6 digits, but the
    // compiler cannot prove the bound and warns about the 7-byte destination.
    // Formatting into a local of the size gcc wants and copying the 6 digits
    // states the invariant instead of suppressing the warning.
    {
      char six[8];
      snprintf(six, sizeof(six), "%06d", (counter++) % 1000000);
      memcpy(tmpl + l - 6, six, 6);
      tmpl[l] = '\0';
    }
    int fd = open(tmpl, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) return fd;
  }
  errno = EEXIST;
  return -1;
}

// sandboxed variants of the direct filesystem mutators
int mkdir_fake(const char *path, int mode) {
  char sb[640];
  return mkdir(sandbox_path(path, sb, sizeof(sb)), (mode_t)mode);
}
int unlink_fake(const char *path) {
  char sb[640];
  return unlink(sandbox_path(path, sb, sizeof(sb)));
}
int rmdir_fake(const char *path) {
  char sb[640];
  return rmdir(sandbox_path(path, sb, sizeof(sb)));
}
// rename.
//
// POSIX rename() REPLACES an existing destination atomically. FAT via newlib
// does not: f_rename returns FR_EXIST and the call fails.
//
// Godot's DirAccessUnix::rename is a bare ::rename() and relies on the POSIX
// behaviour, which is also the standard write-tmp-then-rename save pattern.
// Hardware run 14:
//
//   Failed to save settings: SaveException: Failed to rename file.
//     source=user://default/1/settings.save.tmp
//     destination=user://default/1/settings.save
//     source_exists=True destination_exists=True
//
// Every save over an existing file failed, which on a card game means no
// settings, no progress, nothing persisted between runs.
//
// Unlinking first is not atomic -- a power cut between the two calls loses the
// old file. That window is unavoidable on FAT without a journal, and it is far
// narrower than the alternative of never saving at all. The unlink is only
// attempted when the destination exists, so a first-time save keeps the
// single-call behaviour.
// The FAT replace-on-rename workaround, shared.
//
// Exported because there are TWO rename paths and fixing only one does
// nothing. This engine is the ANDROID build, so `user://` file operations go
// through the JNI DirectoryAccessHandler (jni_fake.c) rather than
// DirAccessUnix -- run 15 still failed every save with the libc-level fix in
// place, because Godot never reached libc rename() for those paths.
//
// Both call sites now route here.
int sm127_rename_replacing(const char *src, const char *dst) {
  int rc = rename(src, dst);
  if (rc == 0) return 0;

  struct stat st;
  if (stat(dst, &st) != 0) return rc;   // destination absent: a real failure

  if (remove(dst) != 0) {
    debugPrintf("[rename] could not remove existing %s\n", dst);
    return rc;
  }

  rc = rename(src, dst);
  if (rc != 0)
    debugPrintf("[rename] %s -> %s failed even after removing the destination\n",
                src, dst);
  return rc;
}

int rename_fake(const char *from, const char *to) {
  char s1[640], s2[640];
  return sm127_rename_replacing(sandbox_path(from, s1, sizeof(s1)),
                               sandbox_path(to, s2, sizeof(s2)));
}
int remove_fake(const char *path) {
  char sb[640];
  return remove(sandbox_path(path, sb, sizeof(sb)));
}

#define BIONIC_AT_FDCWD (-100)

// Forwards to open_fake, which does two things this used to skip.
//
// It called newlib's open() directly with the caller's flags, and that was
// wrong twice over:
//
//   * FLAGS. Bionic and newlib O_* bits collide -- bionic O_APPEND (0x400) is
//     newlib O_TRUNC, and bionic O_TRUNC (0x200) is newlib O_CREAT. Appending
//     truncated. open_fake translates; this did not.
//   * PATH. It passed `path` straight through, with no sandbox_path() rebase,
//     so an absolute path went to the SD root instead of <save_root> -- the
//     same bug that kept the shader cache empty for weeks.
//
// tools/sandbox_check.py did not catch the second because it treats a `_fake`
// suffix as evidence of sandboxing. A wrapper can be named like a shim and
// still forward raw; the checker now has to look at what the body calls.
int openat_fake(int dirfd, const char *path, int flags, ...) {
  mode_t mode = 0;
  if (flags & BIONIC_O_CREAT) {
    va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap);
  }
  if (dirfd != BIONIC_AT_FDCWD && path && path[0] != '/') { errno = ENOSYS; return -1; }
  return open_fake(path, flags, (int)mode);
}
int unlinkat_fake(int dirfd, const char *path, int flags) {
  if (dirfd != BIONIC_AT_FDCWD && path && path[0] != '/') { errno = ENOSYS; return -1; }
  if (flags) return rmdir_fake(path);
  return unlink_fake(path);
}
int fchmodat_fake(int dirfd, const char *path, int mode, int flags) {
  (void)dirfd; (void)path; (void)mode; (void)flags; return 0;
}
int utimensat_fake(int dirfd, const char *path, const void *times, int flags) {
  (void)dirfd; (void)path; (void)times; (void)flags; return 0;
}

long pathconf_fake(const char *path, int name) { (void)path; (void)name; return 4096; }

int sched_getaffinity_fake(int pid, size_t setsize, void *mask) {
  (void)pid;
  if (mask && setsize >= 1) { memset(mask, 0, setsize); ((uint8_t *)mask)[0] = 0x7; } // 3 cores
  return 0;
}
int sched_setaffinity_fake(int pid, size_t setsize, const void *mask) {
  (void)pid; (void)setsize; (void)mask; return 0;
}

// thread_local destructors: threads live for the process lifetime here, so
// registering the destructors is safely skippable (leaks only at thread exit).
int __cxa_thread_atexit_impl_fake(void (*dtor)(void *), void *obj, void *dso) {
  (void)dtor; (void)obj; (void)dso; return 0;
}

void __FD_SET_chk_fake(int fd, void *set, size_t setsize) {
  if (set && fd >= 0 && (size_t)(fd / 8) < setsize)
    ((uint8_t *)set)[fd / 8] |= 1u << (fd % 8);
}

struct bionic_statvfs {
  uint64_t f_bsize, f_frsize, f_blocks, f_bfree, f_bavail;
  uint64_t f_files, f_ffree, f_favail;
  uint64_t f_fsid;
  uint64_t f_flag, f_namemax;
  uint64_t __spare[6];
};
int statvfs_fake(const char *path, void *buf) {
  (void)path;
  struct bionic_statvfs *s = buf;
  memset(s, 0, sizeof(*s));
  s->f_bsize = s->f_frsize = 0x1000;
  s->f_blocks = (4ull * 1024 * 1024 * 1024) / 0x1000;
  s->f_bfree = s->f_bavail = (2ull * 1024 * 1024 * 1024) / 0x1000;
  s->f_namemax = 255;
  return 0;
}

int truncate_fake(const char *path, int64_t len) {
  // open_fake, not open: the path needs the sandbox_path() rebase like every
  // other path call, and this went straight to newlib with whatever the engine
  // passed. An absolute path landed at the SD root.
  int fd = open_fake(path, O_WRONLY);
  if (fd < 0) return -1;
  int rc = ftruncate(fd, (off_t)len);
  close(fd);
  return rc;
}

int __android_log_vprint_fake(int prio, const char *tag, const char *fmt, va_list va) {
  (void)prio;
#if DEBUG_LOG
  char buf[0x800];
  vsnprintf(buf, sizeof(buf), fmt, va);
  debugPrintf("[%s] %s\n", tag ? tag : "", buf);
#else
  (void)tag; (void)fmt; (void)va;
#endif
  return 0;
}

int android_log_write_fake(int prio, const char *tag, const char *msg) {
  (void)prio;
#if DEBUG_LOG
  debugPrintf("[%s] %s\n", tag ? tag : "", msg ? msg : "");
#else
  (void)tag; (void)msg;
#endif
  return 0;
}

void perror_fake(const char *s) {
#if DEBUG_LOG
  debugPrintf("perror: %s: %s\n", s ? s : "", strerror(errno));
#else
  (void)s;
#endif
}

int isatty_fake(int fd) { (void)fd; return 0; }

// On real Android the app process cwd is "/", and Godot's ProjectSettings
// uses the cwd during project discovery to derive resource_path: any real
// directory reported here leaks into every res:// path the engine builds
// (the empty-character-select bug). Mimic Android: cwd is always "/", and the
// sandbox_path() rebase in libc_shim keeps stray absolute writes ("/saves")
// inside the app's save dir instead of the SD root.
char *getcwd_fake(char *buf, size_t size) {
  if (!buf) return strdup("/");
  if (size < 2) { errno = ERANGE; return NULL; }
  strcpy(buf, "/");
  return buf;
}

// ---------------------------------------------------------------------------
// locale _l variants: single-locale system, forward to the C versions
// ---------------------------------------------------------------------------

int strcoll_l_fake(const char *a, const char *b, void *loc) { (void)loc; return strcmp(a, b); }
size_t strftime_l_fake(char *s, size_t max, const char *fmt, const void *tm, void *loc) {
  (void)loc; return strftime(s, max, fmt, (const struct tm *)tm);
}
size_t strxfrm_l_fake(char *dst, const char *src, size_t n, void *loc) { (void)loc; return strxfrm(dst, src, n); }
int wcscoll_l_fake(const wchar_t *a, const wchar_t *b, void *loc) { (void)loc; return wcscmp(a, b); }
size_t wcsxfrm_l_fake(wchar_t *dst, const wchar_t *src, size_t n, void *loc) { (void)loc; return wcsxfrm(dst, src, n); }
int towlower_l_fake(int c, void *loc) { (void)loc; return towlower(c); }
int towupper_l_fake(int c, void *loc) { (void)loc; return towupper(c); }
int isdigit_l_fake(int c, void *loc) { (void)loc; return isdigit(c); }
int isxdigit_l_fake(int c, void *loc) { (void)loc; return isxdigit(c); }
int islower_l_fake(int c, void *loc) { (void)loc; return islower(c); }
int isupper_l_fake(int c, void *loc) { (void)loc; return isupper(c); }
int tolower_l_fake(int c, void *loc) { (void)loc; return tolower(c); }
int toupper_l_fake(int c, void *loc) { (void)loc; return toupper(c); }
long double log10l_fake(long double x) { return (long double)log10((double)x); }

int iswalpha_l_fake(int c, void *loc) { (void)loc; return iswalpha(c); }
int iswblank_l_fake(int c, void *loc) { (void)loc; return iswblank(c); }
int iswcntrl_l_fake(int c, void *loc) { (void)loc; return iswcntrl(c); }
int iswdigit_l_fake(int c, void *loc) { (void)loc; return iswdigit(c); }
int iswlower_l_fake(int c, void *loc) { (void)loc; return iswlower(c); }
int iswprint_l_fake(int c, void *loc) { (void)loc; return iswprint(c); }
int iswpunct_l_fake(int c, void *loc) { (void)loc; return iswpunct(c); }
int iswspace_l_fake(int c, void *loc) { (void)loc; return iswspace(c); }
int iswupper_l_fake(int c, void *loc) { (void)loc; return iswupper(c); }
int iswxdigit_l_fake(int c, void *loc) { (void)loc; return iswxdigit(c); }

// ---------------------------------------------------------------------------
// AAssetManager over <data_root>/assets/: FileAccessAndroid opens every res://
// file through this. Paths arrive relative ("project.binary", "Instances/...").
// ---------------------------------------------------------------------------

typedef struct {
  uint32_t magic;       // 'ASET'
  FILE *f;              // loose file, or NULL for the resident pack
  const uint8_t *mem;   // resident pack bytes (not owned)
  int64_t len, pos;
} FakeAsset;

#define FAKEASSET_MAGIC 0x41534554

static void *g_fake_assetmgr = (void *)0xA55E7;

// The main pack (res://sm127.pck, mounted with --main-pack) is read into
// RAM once, on its first open, and every later "open" of it is a view onto
// that buffer.
//
// FileAccessPack opens the pack afresh for every resource it serves, then
// seeks to the resource. Served from an SD-card FILE, each of those was a
// seek plus a 64 KB stdio refill even for a 200-byte file -- the second
// hardware run's boot took three times as long as with loose files.
// Resident, a scene load does no SD I/O at all.
//
// It is an optimisation, NOT a requirement: the FILE path below serves the
// same bytes. That matters here because this was written for a ~37 MB pack
// against a ~3 GB heap, and SM127's pack is ~162 MB -- four times as much.
// Launched through title override there is still ample room, but from the
// album (applet mode, roughly 448 MB for everything) holding it would take a
// third of the budget away from the engine, and the level load that then runs
// out of memory is far more expensive than the SD reads this saves. So the
// size is weighed against what is actually free; `pack_ram` in config.txt
// forces the decision either way.
static Mutex    s_pack_lock;            // zero-initialised == unlocked
static uint8_t *s_pack_mem;
static int64_t  s_pack_len;
static char     s_pack_path[768];
// FileAccessPack reopens the pack for EVERY resource it serves, so a decision
// not to go resident has to be remembered. Without this the attempt -- and its
// log line -- repeated on every open: one hardware run wrote the same sentence
// several thousand times and buried the engine's own output.
static int      s_pack_declined;

static int is_pck_path(const char *p) {
  size_t l = strlen(p);
  return l > 4 && strcmp(p + l - 4, ".pck") == 0;
}

// Called with s_pack_lock held.
static int load_pack(const char *path) {
  if (s_pack_declined) return 0;
  const u64 t0 = armGetSystemTick();
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  setvbuf(f, NULL, _IONBF, 0);         // large reads go straight to the card
  fseek(f, 0, SEEK_END);
  const int64_t len = ftell(f);
  fseek(f, 0, SEEK_SET);

  if (config.pack_ram == 0) {
    fclose(f);
    s_pack_declined = 1;
    debugPrintf("[assets] pack %s streamed from the card (pack_ram 0)\n", path);
    return 0;
  }

  // Ask for the memory and believe the answer.
  //
  // This briefly used svcGetInfo(TotalMemorySize) - svcGetInfo(UsedMemorySize)
  // to decide whether the pack would fit. That number is not what it sounds
  // like: libnx reserves the whole heap at startup, so "used" already counts
  // every byte newlib will ever hand out and the difference is ~0 whatever the
  // real situation. On hardware it read "162 MB against 3 MB free" on a 3 GB
  // title-override launch and streamed the pack from the card on every open.
  //
  // malloc already answers the only question that matters, exactly and without
  // a heuristic: if the allocation succeeds there was room, and if it fails the
  // FILE path below serves the same bytes.
  uint8_t *buf = len > 0 ? malloc((size_t)len) : NULL;
  if (len > 0 && !buf) {
    fclose(f);
    s_pack_declined = 1;
    debugPrintf("[assets] pack %s: no room for %lld MB, streaming from the card "
                "instead\n", path, (long long)(len >> 20));
    return 0;
  }
  int64_t got = 0;
  while (buf && got < len) {
    const size_t want = (size_t)((len - got) < (4 << 20) ? (len - got) : (4 << 20));
    const size_t n = fread(buf + got, 1, want, f);
    if (!n) break;
    got += (int64_t)n;
  }
  fclose(f);
  if (!buf || got != len) {
    free(buf);
    s_pack_declined = 1;
    debugPrintf("[assets] could not read pack %s into memory (%lld of %lld bytes)\n",
                path, (long long)got, (long long)len);
    return 0;
  }
  s_pack_mem = buf;
  s_pack_len = len;
  strncpy(s_pack_path, path, sizeof(s_pack_path) - 1);
  debugPrintf("[assets] pack %s resident in RAM (%lld KB, read in %llu ms)\n", path,
              (long long)(len >> 10),
              (unsigned long long)(armTicksToNs(armGetSystemTick() - t0) / 1000000ull));
  return 1;
}

void *AAssetManager_fromJava_fake(void *env, void *assetManager) {
  (void)env; (void)assetManager;
  return g_fake_assetmgr;
}

void *AAssetManager_open_fake(void *mgr, const char *filename, int mode) {
  (void)mgr; (void)mode;
  if (!filename) return NULL;
  while (*filename == '/') filename++;
  char path[768];
  FILE *f = NULL;

  // The port's helper scripts (res://switch_port/) ride in the NRO's romfs, so
  // they always match the wrapper and an NRO update needs nothing else on the
  // card. The SD card's assets/switch_port/ is the fallback.
  if (!strncmp(filename, PORT_RES_DIR "/", sizeof(PORT_RES_DIR))) {
    snprintf(path, sizeof(path), "romfs:/%s", filename + sizeof(PORT_RES_DIR));
    f = fopen(path, "rb");
  }

  if (!f) {
    snprintf(path, sizeof(path), "%s/assets/%s", config.data_root, filename);

    if (is_pck_path(path)) {
      mutexLock(&s_pack_lock);
      const int resident = s_pack_mem ? !strcmp(s_pack_path, path) : load_pack(path);
      mutexUnlock(&s_pack_lock);
      if (resident) {
        FakeAsset *a = calloc(1, sizeof(*a));
        if (!a) return NULL;
        a->magic = FAKEASSET_MAGIC;
        a->mem = s_pack_mem;
        a->len = s_pack_len;
        return a;
      }
    }

    f = fopen(path, "rb");
  }
#if VERBOSE_IO
  debugPrintf("AAssetManager_open(\"%s\") -> %p\n", filename, (void *)f);
#endif
  if (!f) return NULL;
  setvbuf(f, NULL, _IOFBF, 64 * 1024);
  FakeAsset *a = calloc(1, sizeof(*a));
  if (!a) { fclose(f); return NULL; }
  a->magic = FAKEASSET_MAGIC;
  a->f = f;
  fseek(f, 0, SEEK_END);
  a->len = ftell(f);
  fseek(f, 0, SEEK_SET);
  return a;
}

int AAsset_read_fake(void *asset, void *buf, size_t count) {
  FakeAsset *a = asset;
  if (!a || a->magic != FAKEASSET_MAGIC || !buf) return -1;
  if (a->mem) {
    int64_t n = a->len - a->pos;
    if (n > (int64_t)count) n = (int64_t)count;
    if (n <= 0) return 0;
    memcpy(buf, a->mem + a->pos, (size_t)n);
    a->pos += n;
    return (int)n;
  }
  return (int)fread(buf, 1, count, a->f);
}

int64_t AAsset_seek_fake(void *asset, int64_t offset, int whence) {
  FakeAsset *a = asset;
  if (!a || a->magic != FAKEASSET_MAGIC) return -1;
  if (a->mem) {
    int64_t p = whence == SEEK_SET ? offset : whence == SEEK_CUR ? a->pos + offset : a->len + offset;
    if (p < 0 || p > a->len) return -1;
    a->pos = p;
    return p;
  }
  if (fseek(a->f, (long)offset, whence) != 0) return -1;
  return ftell(a->f);
}

int64_t AAsset_getLength_fake(void *asset) {
  FakeAsset *a = asset;
  return (a && a->magic == FAKEASSET_MAGIC) ? a->len : 0;
}
int64_t AAsset_getLength64_fake(void *asset) { return AAsset_getLength_fake(asset); }

void AAsset_close_fake(void *asset) {
  FakeAsset *a = asset;
  if (a && a->magic == FAKEASSET_MAGIC) {
    if (a->f) fclose(a->f);   // the resident pack is never freed
    a->magic = 0;
    free(a);
  }
}

// ---------------------------------------------------------------------------
// data symbols
// ---------------------------------------------------------------------------

unsigned char in6addr_any_fake[16];

extern uint8_t fake_sF[3][0x100];
FILE *stdin_fake  = (FILE *)fake_sF[0];
FILE *stdout_fake = (FILE *)fake_sF[1];
FILE *stderr_fake = (FILE *)fake_sF[2];
