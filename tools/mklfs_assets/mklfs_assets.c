// mklfs_assets — assets パーティション (littlefs) のイメージをホストで生成する。
// firmware の esp_littlefs マウント設定と同じ cfg (read/prog=128, block=4096,
// cache=512, lookahead=128, block_cycles=512) でフォーマットし、
// テーマ zip を /themes/<id>.zip に格納する。
//
//   usage: mklfs_assets <out.img> <size_bytes> <src_dir> [fonts_dir]
//   src_dir 内の *.zip がそのまま themes/ に入る。
//   fonts_dir があれば、その中の *.bin が fonts/ に入る (T24 の日本語フォント)。

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "lfs.h"

// ブロックデバイス = 巨大配列 (消去=0xFF、prog は AND 書き込みで NOR を模す)
typedef struct {
  uint8_t* img;
  uint32_t size;
} bd_t;

static int bd_read(const struct lfs_config* c, lfs_block_t b, lfs_off_t o,
                   void* buf, lfs_size_t n) {
  bd_t* bd = c->context;
  memcpy(buf, bd->img + b * c->block_size + o, n);
  return 0;
}

static int bd_prog(const struct lfs_config* c, lfs_block_t b, lfs_off_t o,
                   const void* buf, lfs_size_t n) {
  bd_t* bd = c->context;
  uint8_t* d = bd->img + b * c->block_size + o;
  const uint8_t* s = buf;
  for (lfs_size_t i = 0; i < n; ++i) d[i] &= s[i];  // NOR: 1→0 のみ
  return 0;
}

static int bd_erase(const struct lfs_config* c, lfs_block_t b) {
  bd_t* bd = c->context;
  memset(bd->img + b * c->block_size, 0xFF, c->block_size);
  return 0;
}

static int bd_sync(const struct lfs_config* c) {
  (void)c;
  return 0;
}

static int ends_with(const char* s, const char* suf) {
  const size_t a = strlen(s), b = strlen(suf);
  return a > b && strcmp(s + a - b, suf) == 0;
}

int main(int argc, char** argv) {
  if (argc != 4 && argc != 5) {
    fprintf(stderr, "usage: %s <out.img> <size_bytes> <src_dir> [fonts_dir]\n",
            argv[0]);
    return 1;
  }
  const char* out_path = argv[1];
  const uint32_t img_size = (uint32_t)strtoul(argv[2], NULL, 0);
  const char* src_dir = argv[3];
  const char* fonts_dir = (argc > 4) ? argv[4] : NULL;

  bd_t bd;
  bd.size = img_size;
  bd.img = malloc(img_size);
  if (!bd.img) {
    fprintf(stderr, "alloc %u failed\n", img_size);
    return 1;
  }
  memset(bd.img, 0xFF, img_size);

  struct lfs_config cfg = {
      .context = &bd,
      .read = bd_read,
      .prog = bd_prog,
      .erase = bd_erase,
      .sync = bd_sync,
      // esp_littlefs と同一 (firmware/sdkconfig CONFIG_LITTLEFS_*)
      .read_size = 128,
      .prog_size = 128,
      .block_size = 4096,
      .block_count = img_size / 4096,
      .cache_size = 512,
      .lookahead_size = 128,
      .block_cycles = 512,
  };

  lfs_t lfs;
  if (lfs_format(&lfs, &cfg) || lfs_mount(&lfs, &cfg)) {
    fprintf(stderr, "lfs format/mount failed\n");
    return 1;
  }
  lfs_mkdir(&lfs, "themes");

  DIR* d = opendir(src_dir);
  if (!d) {
    fprintf(stderr, "opendir %s failed\n", src_dir);
    return 1;
  }
  int copied = 0;
  struct dirent* e;
  char src[512], dst[512];
  while ((e = readdir(d))) {
    if (!ends_with(e->d_name, ".zip")) continue;
    snprintf(src, sizeof(src), "%s/%s", src_dir, e->d_name);
    snprintf(dst, sizeof(dst), "themes/%s", e->d_name);
    FILE* f = fopen(src, "rb");
    if (!f) {
      fprintf(stderr, "open %s failed\n", src);
      return 1;
    }
    lfs_file_t lf;
    if (lfs_file_open(&lfs, &lf, dst, LFS_O_WRONLY | LFS_O_CREAT)) {
      fprintf(stderr, "lfs open %s failed\n", dst);
      return 1;
    }
    uint8_t buf[8192];
    size_t n, total = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
      if (lfs_file_write(&lfs, &lf, buf, n) != (lfs_ssize_t)n) {
        fprintf(stderr, "lfs write %s failed\n", dst);
        return 1;
      }
      total += n;
    }
    lfs_file_close(&lfs, &lf);
    fclose(f);
    printf("  %s (%zu bytes)\n", dst, total);
    copied++;
  }
  closedir(d);
  if (!copied) {
    fprintf(stderr, "no .zip in %s\n", src_dir);
    return 1;
  }

  // T24: fonts_dir の *.bin を fonts/ に入れる (内蔵の日本語フォント)。
  if (fonts_dir) {
    lfs_mkdir(&lfs, "fonts");
    DIR* fd = opendir(fonts_dir);
    if (fd) {
      struct dirent* fe;
      while ((fe = readdir(fd))) {
        if (!ends_with(fe->d_name, ".bin")) continue;
        snprintf(src, sizeof(src), "%s/%s", fonts_dir, fe->d_name);
        snprintf(dst, sizeof(dst), "fonts/%s", fe->d_name);
        FILE* f = fopen(src, "rb");
        if (!f) {
          fprintf(stderr, "open %s failed\n", src);
          return 1;
        }
        lfs_file_t lf;
        if (lfs_file_open(&lfs, &lf, dst, LFS_O_WRONLY | LFS_O_CREAT)) {
          fprintf(stderr, "lfs open %s failed\n", dst);
          return 1;
        }
        uint8_t buf[8192];
        size_t n, total = 0;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
          if (lfs_file_write(&lfs, &lf, buf, n) != (lfs_ssize_t)n) {
            fprintf(stderr, "lfs write %s failed\n", dst);
            return 1;
          }
          total += n;
        }
        lfs_file_close(&lfs, &lf);
        fclose(f);
        printf("  %s (%zu bytes)\n", dst, total);
      }
      closedir(fd);
    }
  }
  lfs_unmount(&lfs);

  FILE* out = fopen(out_path, "wb");
  if (!out || fwrite(bd.img, 1, img_size, out) != img_size) {
    fprintf(stderr, "write %s failed\n", out_path);
    return 1;
  }
  fclose(out);
  printf("wrote %s (%u bytes, %d themes)\n", out_path, img_size, copied);
  return 0;
}
