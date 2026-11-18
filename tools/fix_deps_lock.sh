#!/usr/bin/env bash
# firmware/dependencies.lock の絶対パスを、このチェックアウトの場所に合わせる。
#
# ESP-IDF のコンポーネントマネージャは vendor/ の override_path を解決した
# 絶対パスを dependencies.lock に記録する。このリポジトリの lock は作者の
# マシンのパス (/home/sasnews/Desktop/ai-space/SAS-Watch/...) を保持している
# ため、別の場所で clone すると素の `idf.py build` が configure で
# 「path ... does not exist」で止まる。CI (.github/workflows/firmware.yml) は
# sed で同じ正規化をしている。ローカルでもこのスクリプトを 1 回実行すれば直る。
#
#   tools/fix_deps_lock.sh          # 正規化する (変更があれば書き込む)
#   tools/fix_deps_lock.sh --check  # 変更が必要なら exit 1 (CI/検査向け)
#
# 生成物ではない tracked ファイル (dependencies.lock) を書き換えるので、
# 実行後に差分が出たら、そのままコミットしてはいけない (作者パスを保つ)。
# ローカルビルドのためだけに使い、ビルド後は `git checkout -- firmware/dependencies.lock`
# で戻すのが安全。
set -euo pipefail

usage() {
    sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
    echo
    echo "使い方: $0 [--check]"
}

check_only=0
case "${1:-}" in
    "" ) ;;
    --check) check_only=1 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
esac

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
lock="$root/firmware/dependencies.lock"

if [ ! -f "$lock" ]; then
    echo "not found: $lock" >&2
    exit 1
fi

python3 - "$lock" "$root" "$check_only" <<'PY'
import re
import sys

path, root, check_only = sys.argv[1], sys.argv[2], sys.argv[3] == "1"
text = open(path, encoding="utf-8").read()

# 例: /home/sasnews/Desktop/ai-space/SAS-Watch  →  <root>
# リポジトリ名 (SAS-Watch) より前を丸ごと置き換える。二重実行しても同じ結果。
pattern = re.compile(r"/home/[^/\s]+/(?:[^\s]*?/)?SAS-Watch")
new = pattern.sub(root, text)

if new == text:
    print(f"ok: {path} (変更なし)")
    sys.exit(0)

if check_only:
    print(f"stale: {path} (絶対パスがこのチェックアウトと一致しない)", file=sys.stderr)
    sys.exit(1)

# 念のため atomic に書く (失敗時に壊れた lock を残さない)。
tmp = path + ".tmp"
with open(tmp, "w", encoding="utf-8") as f:
    f.write(new)
import os
os.replace(tmp, path)
print(f"fixed: {path}")
print("注意: これはローカルビルド用。コミット前に git checkout -- firmware/dependencies.lock で戻すこと。")
PY
