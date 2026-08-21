#!/bin/sh
# Recompile the bench binaries for every variant of an existing matrix, without
# rebuilding libcurl. Use after changing the harness itself: the libcurl builds
# are the expensive part and they are unaffected.
#
# usage: rebuild-bench.sh --matrix DIR --stage DIR [--cc CC] [--cflags "..."]
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
BENCH_ROOT=$(cd "$HERE/.." && pwd)
MX=""
STAGE=""
CCX=""
CFX=""
STRIP=""

while [ $# -gt 0 ]; do
  case "$1" in
    --matrix) MX="$2"; shift 2 ;;
    --stage) STAGE="$2"; shift 2 ;;
    --cc) CCX="$2"; shift 2 ;;
    --cflags) CFX="$2"; shift 2 ;;
    --strip) STRIP="$2"; shift 2 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

if [ -z "$MX" ] || [ -z "$STAGE" ]; then
  echo "usage: rebuild-bench.sh --matrix DIR --stage DIR" >&2
  exit 2
fi

if [ -n "$CCX" ]; then
  CC="$CCX"; export CC
fi
if [ -n "$CFX" ]; then
  CFLAGS="$CFX -std=c99 -Wall -Wextra -Wno-unused-parameter"; export CFLAGS
fi

for prefix in "$MX"/curl/*; do
  [ -d "$prefix" ] || continue
  v=$(basename "$prefix")
  rm -rf "$MX/obj/$v"
  make -C "$BENCH_ROOT" CURL_PREFIX="$prefix" O="$MX/obj/$v" >/dev/null 2>&1
  cp "$MX/obj/$v/curlbench" "$MX/bin/curlbench-$v"
  echo "  rebuilt $v"
done

mkdir -p "$STAGE"
rm -f "$STAGE"/curlbench-*
cp "$MX"/bin/curlbench-* "$STAGE"/
cp "$STAGE/curlbench-base" "$STAGE/curlbench-base2"
if [ -n "$STRIP" ] && command -v "$STRIP" >/dev/null 2>&1; then
  "$STRIP" "$STAGE"/curlbench-* 2>/dev/null || true
else
  strip "$STAGE"/curlbench-* 2>/dev/null || true
fi
chmod 755 "$STAGE"/curlbench-*
echo "=== restaged $STAGE"
ls -1 "$STAGE" | wc -l
