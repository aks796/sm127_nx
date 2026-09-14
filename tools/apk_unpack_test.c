/* apk_unpack_test.c -- runs source/apk_unpack.c on a computer.
 *
 *   cc -O2 -Isource tools/apk_unpack_test.c source/apk_unpack.c -lz -o apk_unpack_test
 *   ./apk_unpack_test path/to/game.apk out_dir
 *
 * out_dir gets what the console's first launch writes next to the NRO.
 *
 * MIT license; see LICENSE. */

#include <stdio.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "apk_unpack.h"

static void progress(void *user, uint64_t done, uint64_t total) {
  static int last = -1;
  (void)user;
  const int pct = total ? (int)(done * 100 / total) : 100;
  if (pct / 10 != last) {
    last = pct / 10;
    printf("  %3d%%  %llu of %llu MB\n", pct, (unsigned long long)(done >> 20),
           (unsigned long long)(total >> 20));
  }
}

static int keep_going(void *user) { (void)user; return 1; }
static int rename_replacing(const char *from, const char *to) { return rename(from, to); }

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s <game.apk> <out dir>\n", argv[0]);
    return 2;
  }
  mkdir(argv[2], 0777);
  ApkUnpackIo io = { NULL, progress, keep_going, rename_replacing };
  ApkUnpackResult res;
  struct timeval t0, t1;
  gettimeofday(&t0, NULL);
  const int rc = apk_unpack(argv[1], argv[2], "sm127.pck", &io, &res);
  gettimeofday(&t1, NULL);
  if (rc != APK_UNPACK_OK) {
    fprintf(stderr, "failed (%d): %s\n", rc, res.error);
    return 1;
  }
  printf("%u files in the pack, %llu MB written in %.1f s\n", res.files,
         (unsigned long long)(res.bytes >> 20),
         (t1.tv_sec - t0.tv_sec) + (t1.tv_usec - t0.tv_usec) / 1e6);
  return 0;
}
