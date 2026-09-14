/* apk_unpack.h -- the game's Android APK to the files the wrapper loads.
 * See apk_unpack.c.
 *
 * MIT license; see LICENSE. */

#ifndef __APK_UNPACK_H__
#define __APK_UNPACK_H__

#include <stddef.h>
#include <stdint.h>

typedef struct {
  void *user;
  // Called as data is written; done and total count uncompressed bytes.
  void (*progress)(void *user, uint64_t done, uint64_t total);
  // Checked between files. 0 stops the install and removes what it wrote.
  int  (*keep_going)(void *user);
  // rename() that replaces an existing destination (FAT's does not).
  int  (*rename_replacing)(const char *from, const char *to);
} ApkUnpackIo;

typedef struct {
  unsigned files;    // files in the pack
  uint64_t bytes;    // uncompressed bytes written, libraries included
  char error[512];   // why, when apk_unpack returns APK_UNPACK_FAILED
} ApkUnpackResult;

enum { APK_UNPACK_OK = 0, APK_UNPACK_FAILED = -1, APK_UNPACK_STOPPED = -2 };

// Writes <root>/libgodot_android.so, <root>/libc++_shared.so and
// <root>/assets/<pck_name> from the APK at apk_path.
int apk_unpack(const char *apk_path, const char *root, const char *pck_name,
               const ApkUnpackIo *io, ApkUnpackResult *res);

#endif
