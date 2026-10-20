#!/bin/sh
# assets パーティション (littlefs, 16MiB @ 0x820000) のイメージを生成する。
# 使い方: tools/mklfs_assets/make_assets_image.sh
#   入力: android/app/src/main/assets/themes/*.zip (+ tools/fonts/bin/*.bin)
#   出力: firmware/build/assets_littlefs.bin
#   書き込み: python3 -m esptool write-flash 0x820000 firmware/build/assets_littlefs.bin
set -eu
cd "$(dirname "$0")/../.."
REPO="$PWD"
LFS_DIR="firmware/managed_components/joltwallet__littlefs/src/littlefs"
OUT="firmware/build/assets_littlefs.bin"

cc -O2 -I "$LFS_DIR" -o /tmp/mklfs_assets \
   tools/mklfs_assets/mklfs_assets.c "$LFS_DIR/lfs.c" "$LFS_DIR/lfs_util.c"

# 第4引数: tools/fonts/bin があれば fonts/ に .bin を入れる (T24)。
if [ -d tools/fonts/bin ]; then
    /tmp/mklfs_assets "$OUT" 16777216 android/app/src/main/assets/themes tools/fonts/bin
else
    /tmp/mklfs_assets "$OUT" 16777216 android/app/src/main/assets/themes
fi
