#!/usr/bin/env bash
# lain0 全量回归：编译 examples 下每个 .lain，报通过数与通过清单。
# 用法：  bash scripts/regress.sh [--round-trip]
set -u

# 向上找到含 bootstrap/lain/examples 的目录作为仓库根
dir="$(cd "$(dirname "$0")" && pwd)"
while [ "$dir" != "/" ] && [ ! -d "$dir/bootstrap/lain/examples" ]; do dir="$(dirname "$dir")"; done
cd "$dir" || exit 1

EXE=./build/lain0/lain0.exe
if [ ! -x "$EXE" ]; then echo "找不到 $EXE —— 先编译 lain0"; exit 1; fi

FLAGS="${1:-}"
pass=0; fail=0; ok=""; bad=""
for f in bootstrap/lain/examples/*.lain; do
  [ -f "$f" ] || continue
  out=$("$EXE" $FLAGS "$f" 2>&1); rc=$?
  name=$(basename "$f" .lain)
  if [ $rc -eq 0 ] && ! printf '%s' "$out" | grep -q "error"; then
    pass=$((pass+1)); ok="$ok $name"
  else
    fail=$((fail+1)); bad="$bad $name"
  fi
done
echo "PASS=$pass FAIL=$fail   flags='${FLAGS}'"
echo "通过:$ok"
if [ -n "$bad" ]; then echo "未过:$bad"; fi
