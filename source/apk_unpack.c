/* apk_unpack.c -- the game's Android APK to the files the wrapper loads.
 *
 * Reads the APK (a zip) and writes, under the app folder:
 *
 *   libgodot_android.so, libc++_shared.so   lib/arm64-v8a/, as they are
 *   assets/sm127.pck                         assets/, packed into one Godot 3
 *                                            pack (format 1)
 *
 * The same files scripts/extract_apk.sh and tools/make_pck.py make on a
 * computer, made on the console. Every file is checked against the CRC-32 the
 * archive recorded for it, and each output is written as <name>.part and only
 * renamed into place once all of them are complete, so a failed or stopped
 * install leaves the previous one as it was.
 *
 * The pack leaves out assets/dexopt/ (Android runtime profiles) and
 * assets/override.cfg, which the wrapper writes itself.
 *
 * Pack format, as Godot 3.6's PackedSourcePCK::try_open_pack reads it:
 *
 *   u32 magic 'GDPC'   u32 format 1   u32 major, minor, patch
 *   u32 reserved[16]   u32 file_count
 *   per file: u32 path_len, path ("res://...", NUL-padded to 4), u64 offset,
 *             u64 size, u8 md5[16]
 *   file data at the absolute offsets, each aligned to 16 bytes
 *
 * No libnx in this file: tools/apk_unpack_test.c runs it on a computer.
 *
 * MIT license; see LICENSE. */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <zlib.h>

#include "apk_unpack.h"

#define ZIP_EOCD_SIG  0x06054b50
#define ZIP_CDIR_SIG  0x02014b50
#define ZIP_LOCAL_SIG 0x04034b50

#define PCK_MAGIC  0x43504447   // "GDPC"
#define PCK_HEADER 88
#define PCK_ALIGN  16

#define APK_BUF (4 << 20)       // read-ahead over the APK
#define CHUNK   (1 << 20)       // inflate output, and the pack's stdio buffer

static const char *const LIB_ENTRY[2] = {
  "lib/arm64-v8a/libgodot_android.so",
  "lib/arm64-v8a/libc++_shared.so",
};

// --- MD5 (RFC 1321), for the pack's per-file digests -------------------------

typedef struct {
  uint32_t h[4];
  uint64_t len;
  uint8_t buf[64];
  size_t n;
} Md5;

static const uint32_t MD5_K[64] = {
  0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
  0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
  0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
  0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
  0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
  0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
  0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
  0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

static const uint8_t MD5_R[64] = {
  7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
  5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
  6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

static void md5_block(Md5 *m, const uint8_t *p) {
  uint32_t w[16];
  for (int i = 0; i < 16; i++)
    w[i] = p[i * 4] | p[i * 4 + 1] << 8 | p[i * 4 + 2] << 16 | (uint32_t)p[i * 4 + 3] << 24;
  uint32_t a = m->h[0], b = m->h[1], c = m->h[2], d = m->h[3];
  for (int i = 0; i < 64; i++) {
    uint32_t f;
    int g;
    if (i < 16)      { f = (b & c) | (~b & d); g = i; }
    else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
    else if (i < 48) { f = b ^ c ^ d;          g = (3 * i + 5) % 16; }
    else             { f = c ^ (b | ~d);       g = (7 * i) % 16; }
    const uint32_t t = d;
    d = c;
    c = b;
    const uint32_t x = a + f + MD5_K[i] + w[g];
    b = b + ((x << MD5_R[i]) | (x >> (32 - MD5_R[i])));
    a = t;
  }
  m->h[0] += a; m->h[1] += b; m->h[2] += c; m->h[3] += d;
}

static void md5_init(Md5 *m) {
  m->h[0] = 0x67452301; m->h[1] = 0xefcdab89; m->h[2] = 0x98badcfe; m->h[3] = 0x10325476;
  m->len = 0;
  m->n = 0;
}

static void md5_update(Md5 *m, const uint8_t *p, size_t n) {
  m->len += n;
  if (m->n) {
    size_t take = 64 - m->n;
    if (take > n) take = n;
    memcpy(m->buf + m->n, p, take);
    m->n += take; p += take; n -= take;
    if (m->n < 64) return;
    md5_block(m, m->buf);
    m->n = 0;
  }
  for (; n >= 64; p += 64, n -= 64) md5_block(m, p);
  if (n) { memcpy(m->buf, p, n); m->n = n; }
}

static void md5_final(Md5 *m, uint8_t out[16]) {
  const uint64_t bits = m->len * 8;
  m->buf[m->n++] = 0x80;
  if (m->n > 56) {
    memset(m->buf + m->n, 0, 64 - m->n);
    md5_block(m, m->buf);
    m->n = 0;
  }
  memset(m->buf + m->n, 0, 56 - m->n);
  for (int i = 0; i < 8; i++) m->buf[56 + i] = (uint8_t)(bits >> (8 * i));
  md5_block(m, m->buf);
  for (int i = 0; i < 4; i++)
    for (int k = 0; k < 4; k++) out[i * 4 + k] = (uint8_t)(m->h[i] >> (8 * k));
}

// --- the APK -----------------------------------------------------------------

typedef struct {
  FILE *f;
  uint8_t *buf;
  uint64_t buf_ofs;   // file offset of buf[0]
  size_t buf_len;
  uint64_t size;
} Apk;

typedef struct {
  const char *name;   // NUL-terminated copy of the entry's name
  uint32_t crc, csize, usize, local_ofs;
  uint16_t method;
  uint32_t path_len;  // "res://" + the name under assets/, padded to 4
  uint64_t pck_ofs;
  uint8_t md5[16];
} Entry;

typedef struct {
  const ApkUnpackIo *io;
  ApkUnpackResult *res;
  uint64_t done, total;
  uint8_t *tmp;
} Job;

typedef struct {
  FILE *f;
  int failed;
} Out;

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }

static int fail(Job *j, const char *fmt, ...) {
  va_list va;
  va_start(va, fmt);
  vsnprintf(j->res->error, sizeof(j->res->error), fmt, va);
  va_end(va);
  return APK_UNPACK_FAILED;
}

// A view of the APK from ofs: points into the read-ahead buffer and returns how
// many bytes it holds (0 past the end or on a read error). Entries are stored
// in the order the directory lists them, so this is nearly always a forward
// read with no seek.
static size_t apk_view(Apk *a, uint64_t ofs, const uint8_t **p) {
  if (ofs < a->buf_ofs || ofs >= a->buf_ofs + a->buf_len) {
    if (ofs >= a->size || fseek(a->f, (long)ofs, SEEK_SET) != 0) return 0;
    a->buf_ofs = ofs;
    a->buf_len = fread(a->buf, 1, APK_BUF, a->f);
    if (a->buf_len == 0) return 0;
  }
  *p = a->buf + (ofs - a->buf_ofs);
  return a->buf_len - (size_t)(ofs - a->buf_ofs);
}

static int apk_read(Apk *a, uint64_t ofs, uint8_t *dst, size_t n) {
  while (n) {
    const uint8_t *p;
    size_t avail = apk_view(a, ofs, &p);
    if (!avail) return -1;
    if (avail > n) avail = n;
    memcpy(dst, p, avail);
    dst += avail; ofs += avail; n -= avail;
  }
  return 0;
}

static void out_write(Out *o, const uint8_t *p, size_t n) {
  if (n && !o->failed && fwrite(p, 1, n, o->f) != n) o->failed = 1;
}

static void produced(Job *j, Out *o, Md5 *md5, uLong *crc, const uint8_t *p, size_t n) {
  *crc = crc32(*crc, p, (uInt)n);
  if (md5) md5_update(md5, p, n);
  out_write(o, p, n);
  j->done += n;
  j->io->progress(j->io->user, j->done, j->total);
}

// One entry's data, uncompressed, to o; checked against the directory's size
// and CRC-32.
static int copy_entry(Job *j, Apk *a, const Entry *e, Out *o, Md5 *md5) {
  uint8_t lh[30];
  if (apk_read(a, e->local_ofs, lh, sizeof(lh)) != 0 || le32(lh) != ZIP_LOCAL_SIG)
    return fail(j, "%s: the APK is damaged (bad local header).", e->name);
  uint64_t in = (uint64_t)e->local_ofs + sizeof(lh) + le16(lh + 26) + le16(lh + 28);
  uLong crc = crc32(0L, Z_NULL, 0);
  const uint64_t before = j->done;

  if (e->method == 0) {
    if (e->csize != e->usize)
      return fail(j, "%s: the APK is damaged (stored sizes differ).", e->name);
    for (uint64_t left = e->usize; left; ) {
      const uint8_t *p;
      size_t n = apk_view(a, in, &p);
      if (!n) return fail(j, "%s: the APK ends early.", e->name);
      if (n > left) n = (size_t)left;
      if (n > CHUNK) n = CHUNK;
      produced(j, o, md5, &crc, p, n);
      in += n;
      left -= n;
    }
  } else {
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return fail(j, "zlib could not start.");
    uint64_t left = e->csize;
    for (;;) {
      if (zs.avail_in == 0 && left) {
        const uint8_t *p;
        size_t n = apk_view(a, in, &p);
        if (!n) {
          inflateEnd(&zs);
          return fail(j, "%s: the APK ends early.", e->name);
        }
        if (n > left) n = (size_t)left;
        zs.next_in = (Bytef *)p;   // zlib only reads through it
        zs.avail_in = (uInt)n;
        in += n;
        left -= n;
      }
      zs.next_out = j->tmp;
      zs.avail_out = CHUNK;
      const int ret = inflate(&zs, Z_NO_FLUSH);
      if (zs.avail_out < CHUNK) produced(j, o, md5, &crc, j->tmp, CHUNK - zs.avail_out);
      if (ret == Z_STREAM_END) break;
      if ((ret == Z_BUF_ERROR && zs.avail_in == 0 && left == 0) ||
          (ret != Z_OK && ret != Z_BUF_ERROR)) {
        inflateEnd(&zs);
        return fail(j, "%s: the APK is damaged (zlib %d).", e->name, ret);
      }
    }
    inflateEnd(&zs);
  }

  if (j->done - before != e->usize)
    return fail(j, "%s: the APK is damaged (%llu bytes, %u expected).", e->name,
                (unsigned long long)(j->done - before), e->usize);
  if ((uint32_t)crc != e->crc)
    return fail(j, "%s: the APK is damaged (checksum mismatch).", e->name);
  if (o->failed)
    return fail(j, "Could not write %s. The SD card may be full.", e->name);
  return APK_UNPACK_OK;
}

static int is_zip64(uint32_t csize, uint32_t usize, uint32_t ofs) {
  return csize == 0xFFFFFFFF || usize == 0xFFFFFFFF || ofs == 0xFFFFFFFF;
}

int apk_unpack(const char *apk_path, const char *root, const char *pck_name,
               const ApkUnpackIo *io, ApkUnpackResult *res) {
  memset(res, 0, sizeof(*res));
  Job j = { io, res, 0, 0, NULL };
  Apk a;
  memset(&a, 0, sizeof(a));
  uint8_t *cd = NULL, *table = NULL, *tail = NULL;
  char *names = NULL, *vbuf = NULL;
  Entry *assets = NULL;
  FILE *pf = NULL;
  char part[3][768];
  int nparts = 0;
  int rc = APK_UNPACK_FAILED;
  #define FAIL(...) do { rc = fail(&j, __VA_ARGS__); goto out; } while (0)
  #define STOP() do { rc = APK_UNPACK_STOPPED; snprintf(res->error, sizeof(res->error), "Stopped."); goto out; } while (0)

  a.f = fopen(apk_path, "rb");
  if (!a.f) FAIL("Could not open the APK.");
  setvbuf(a.f, NULL, _IONBF, 0);   // apk_view does the buffering
  fseek(a.f, 0, SEEK_END);
  a.size = (uint64_t)ftell(a.f);
  a.buf = malloc(APK_BUF);
  j.tmp = malloc(CHUNK);
  vbuf = malloc(CHUNK);
  if (!a.buf || !j.tmp || !vbuf) FAIL("Out of memory.");

  // End of central directory: the last 22 bytes, before a comment of up to
  // 65535 bytes.
  const size_t tail_n = a.size < 22 + 65535 ? (size_t)a.size : 22 + 65535;
  if (tail_n < 22) FAIL("This is not an APK (too small).");
  tail = malloc(tail_n);
  if (!tail || apk_read(&a, a.size - tail_n, tail, tail_n) != 0) FAIL("Could not read the APK.");
  const uint8_t *eocd = NULL;
  for (size_t i = tail_n - 22 + 1; i-- > 0; )
    if (le32(tail + i) == ZIP_EOCD_SIG) { eocd = tail + i; break; }
  if (!eocd) FAIL("This is not an APK (no zip directory).");
  const unsigned entries = le16(eocd + 10);
  const uint32_t cd_size = le32(eocd + 12), cd_ofs = le32(eocd + 16);
  if (entries == 0xFFFF || is_zip64(cd_size, 0, cd_ofs))
    FAIL("The APK uses zip64, which this installer does not read.");
  if ((uint64_t)cd_ofs + cd_size > a.size) FAIL("The APK is damaged (directory out of range).");

  cd = malloc(cd_size ? cd_size : 1);
  names = malloc(cd_size ? cd_size : 1);   // each name is shorter than its 46-byte header
  assets = calloc(entries ? entries : 1, sizeof(Entry));
  if (!cd || !names || !assets) FAIL("Out of memory.");
  if (apk_read(&a, cd_ofs, cd, cd_size) != 0) FAIL("Could not read the APK's directory.");

  Entry libs[2];
  int have_lib[2] = { 0, 0 };
  int have_project = 0;
  unsigned nassets = 0;
  size_t pos = 0, pool = 0;
  for (unsigned i = 0; i < entries; i++) {
    if (pos + 46 > cd_size || le32(cd + pos) != ZIP_CDIR_SIG)
      FAIL("The APK is damaged (bad directory entry).");
    const uint8_t *h = cd + pos;
    const uint16_t nlen = le16(h + 28);
    if (pos + 46 + nlen > cd_size) FAIL("The APK is damaged (bad directory entry).");
    char *name = names + pool;
    memcpy(name, h + 46, nlen);
    name[nlen] = 0;
    pool += (size_t)nlen + 1;
    pos += 46 + (size_t)nlen + le16(h + 30) + le16(h + 32);

    const size_t len = nlen;
    if (len >= 10 && !strcmp(name + len - 10, ".sparsepck"))
      FAIL("This APK stores its data as a Godot 4 sparse pack. It is not the Super Mario 127 build this port runs.");

    int lib = -1, asset = 0;
    for (int k = 0; k < 2; k++)
      if (!strcmp(name, LIB_ENTRY[k])) lib = k;
    if (lib < 0 && !strncmp(name, "assets/", 7) && name[len - 1] != '/' &&
        strncmp(name + 7, "dexopt/", 7) != 0 && strcmp(name + 7, "override.cfg") != 0) {
      asset = 1;
      if (!strcmp(name + 7, "project.binary")) have_project = 1;
    }
    if (lib < 0 && !asset) continue;

    Entry e;
    memset(&e, 0, sizeof(e));
    e.name = name;
    e.method = le16(h + 10);
    e.crc = le32(h + 16);
    e.csize = le32(h + 20);
    e.usize = le32(h + 24);
    e.local_ofs = le32(h + 42);
    if (le16(h + 8) & 1) FAIL("%s is encrypted in the APK.", name);
    if (e.method != 0 && e.method != 8) FAIL("%s uses zip method %u, which this installer does not read.", name, e.method);
    if (is_zip64(e.csize, e.usize, e.local_ofs)) FAIL("The APK uses zip64, which this installer does not read.");

    if (lib >= 0) {
      libs[lib] = e;
      have_lib[lib] = 1;
    } else {
      e.path_len = (uint32_t)((6 + len + 3) & ~(size_t)3);
      assets[nassets++] = e;
    }
  }
  free(tail);
  tail = NULL;

  for (int k = 0; k < 2; k++)
    if (!have_lib[k]) FAIL("The APK has no %s. It needs to be an ARM64 Android build.", LIB_ENTRY[k]);
  if (!have_project)
    FAIL("The APK has no assets/project.binary. It is not a Godot game with its data inside the APK.");

  // Lay out the pack: the directory, then every file aligned to 16.
  uint64_t dir_size = 0;
  for (unsigned i = 0; i < nassets; i++) dir_size += 4 + assets[i].path_len + 8 + 8 + 16;
  uint64_t ofs = PCK_HEADER + dir_size;
  for (unsigned i = 0; i < nassets; i++) {
    ofs = (ofs + PCK_ALIGN - 1) & ~(uint64_t)(PCK_ALIGN - 1);
    assets[i].pck_ofs = ofs;
    ofs += assets[i].usize;
  }
  const uint64_t pck_size = ofs;
  j.total = (uint64_t)libs[0].usize + libs[1].usize;
  for (unsigned i = 0; i < nassets; i++) j.total += assets[i].usize;

  const uint64_t need = pck_size + libs[0].usize + libs[1].usize + (16 << 20);
  struct statvfs sv;
  if (statvfs(root, &sv) == 0) {
    const uint64_t avail = (uint64_t)sv.f_bavail * sv.f_frsize;
    if (avail < need)
      FAIL("The SD card needs %llu MB free to install the game and has %llu MB.",
           (unsigned long long)(need >> 20), (unsigned long long)(avail >> 20));
  }

  char path[768];
  snprintf(path, sizeof(path), "%s/assets", root);
  mkdir(path, 0777);

  for (int k = 0; k < 2; k++) {
    if (!io->keep_going(io->user)) STOP();
    const char *base = strrchr(LIB_ENTRY[k], '/') + 1;
    snprintf(part[nparts], sizeof(part[0]), "%s/%s.part", root, base);
    FILE *f = fopen(part[nparts], "wb");
    if (!f) FAIL("Could not create %s.", part[nparts]);
    nparts++;
    Out o = { f, 0 };
    const int r = copy_entry(&j, &a, &libs[k], &o, NULL);
    if (fclose(f) != 0) o.failed = 1;
    if (r != APK_UNPACK_OK) { rc = r; goto out; }
    if (o.failed) FAIL("Could not write %s. The SD card may be full.", base);
  }

  snprintf(part[nparts], sizeof(part[0]), "%s/assets/%s.part", root, pck_name);
  pf = fopen(part[nparts], "wb");
  if (!pf) FAIL("Could not create %s.", part[nparts]);
  nparts++;
  setvbuf(pf, vbuf, _IOFBF, CHUNK);
  Out o = { pf, 0 };

  uint8_t header[PCK_HEADER];
  memset(header, 0, sizeof(header));
  put32(header, PCK_MAGIC);
  put32(header + 4, 1);
  put32(header + 8, 3);    // Godot 3.6.0, the engine this port runs
  put32(header + 12, 6);
  put32(header + 16, 0);
  put32(header + 84, nassets);
  out_write(&o, header, sizeof(header));

  // The directory is written once as zeros to hold its place, and again at the
  // end, when every file's MD5 is known.
  table = calloc(1, (size_t)dir_size);
  if (!table) FAIL("Out of memory.");
  out_write(&o, table, (size_t)dir_size);

  static const uint8_t zeros[PCK_ALIGN];
  uint64_t at = PCK_HEADER + dir_size;
  for (unsigned i = 0; i < nassets; i++) {
    if (!io->keep_going(io->user)) STOP();
    Entry *e = &assets[i];
    out_write(&o, zeros, (size_t)(e->pck_ofs - at));
    Md5 m;
    md5_init(&m);
    const int r = copy_entry(&j, &a, e, &o, &m);
    if (r != APK_UNPACK_OK) { rc = r; goto out; }
    md5_final(&m, e->md5);
    at = e->pck_ofs + e->usize;
    res->files++;
  }

  size_t t = 0;
  for (unsigned i = 0; i < nassets; i++) {
    const Entry *e = &assets[i];
    put32(table + t, e->path_len);
    t += 4;
    memcpy(table + t, "res://", 6);
    memcpy(table + t + 6, e->name + 7, strlen(e->name + 7));
    t += e->path_len;
    put64(table + t, e->pck_ofs);
    put64(table + t + 8, e->usize);
    memcpy(table + t + 16, e->md5, 16);
    t += 32;
  }
  if (fflush(pf) != 0 || fseek(pf, PCK_HEADER, SEEK_SET) != 0) o.failed = 1;
  out_write(&o, table, (size_t)dir_size);
  if (fclose(pf) != 0) o.failed = 1;
  pf = NULL;
  if (o.failed) FAIL("Could not write %s. The SD card may be full.", pck_name);

  struct stat st;
  if (stat(part[nparts - 1], &st) != 0 || (uint64_t)st.st_size != pck_size)
    FAIL("%s came out the wrong size. The SD card may be full or failing.", pck_name);

  // Everything is written and checked. Move it into place.
  for (int k = 0; k < nparts; k++) {
    char done[sizeof(part[0])];
    memcpy(done, part[k], sizeof(done));
    done[strlen(done) - 5] = 0;   // drop ".part"
    if (io->rename_replacing(part[k], done) != 0) FAIL("Could not replace %s.", done);
    part[k][0] = 0;
  }
  nparts = 0;
  res->bytes = j.done;
  rc = APK_UNPACK_OK;

out:
  #undef FAIL
  #undef STOP
  if (pf) fclose(pf);
  for (int k = 0; k < nparts; k++)
    if (part[k][0]) remove(part[k]);
  if (a.f) fclose(a.f);
  free(a.buf);
  free(j.tmp);
  free(vbuf);
  free(tail);
  free(cd);
  free(names);
  free(assets);
  free(table);
  return rc;
}
