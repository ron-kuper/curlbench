#!/bin/sh
# Build one bench binary per variant: master plus each patch applied alone,
# plus (optionally) all patches together.
#
# usage: build-variants.sh --curl-src GIT_CLONE --out DIR
#                          [--patch FILE]... [--all]
#                          [--host TRIPLE] [--cc CC] [--rev REV]
#
# Each variant gets its own git worktree of the clone, so the clone itself is
# never modified and variants cannot contaminate each other. A patch that does
# not apply is reported and skipped rather than silently dropped.

set -e

HERE=$(cd "$(dirname "$0")" && pwd)
BENCH_ROOT=$(cd "$HERE/.." && pwd)

CURL_SRC=""
OUT=""
HOST=""
CC_OVERRIDE=""
CFLAGS_OVERRIDE=""
TARGET=""
REV="origin/master"
DO_ALL=0
PATCHES=""

while [ $# -gt 0 ]; do
  case "$1" in
    --curl-src) CURL_SRC="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --patch) PATCHES="$PATCHES $2"; shift 2 ;;
    --all) DO_ALL=1; shift ;;
    --target) TARGET="$2"; shift 2 ;;
    --toolchain) TOOLCHAIN="$2"; shift 2 ;;
    --host) HOST="$2"; shift 2 ;;
    --cc) CC_OVERRIDE="$2"; shift 2 ;;
    --cflags) CFLAGS_OVERRIDE="$2"; shift 2 ;;
    --rev) REV="$2"; shift 2 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

# --toolchain names an already-installed cross toolchain by its triple, and is
# the right choice whenever one exists for the device: it was built for that
# part, so it already targets the right kernel and libc and already defaults to
# the right -mcpu. That is why no tune flags are added here -- overriding a
# device toolchain's own defaults is how a measurement stops describing the
# device. Pass --cflags to override anyway, or --cc for a toolchain that is not
# on PATH.
if [ -n "$TOOLCHAIN" ]; then
  [ -z "$HOST" ] && HOST="$TOOLCHAIN"
  [ -z "$CC_OVERRIDE" ] && CC_OVERRIDE="$TOOLCHAIN-gcc"
  [ -z "$CFLAGS_OVERRIDE" ] && CFLAGS_OVERRIDE="-O2 -g"
fi

# A target profile supplies the host triple, the tune flags and a default
# compiler. Explicit --host/--cc/--cflags and --toolchain all win over it, so a
# profile can be used with a different toolchain than the one it suggests.
if [ -n "$TARGET" ]; then
  PROFILE="$BENCH_ROOT/targets/$TARGET.env"
  if [ ! -r "$PROFILE" ]; then
    echo "no such target profile: $PROFILE" >&2
    echo "available:" >&2
    ls -1 "$BENCH_ROOT/targets" 2>/dev/null | sed 's/\.env$//' >&2
    exit 2
  fi
  . "$PROFILE"
  echo "== target $TARGET: $CB_NOTE"
  [ -z "$HOST" ] && HOST="$CB_HOST"
  [ -z "$CC_OVERRIDE" ] && CC_OVERRIDE="$CB_CC"
  [ -z "$CFLAGS_OVERRIDE" ] && CFLAGS_OVERRIDE="$CB_CFLAGS"
fi

# Fail here rather than after building a variant's worth of libcurl.
if [ -n "$CC_OVERRIDE" ] && ! command -v "$CC_OVERRIDE" >/dev/null 2>&1; then
  if [ ! -x "$CC_OVERRIDE" ]; then
    echo "compiler not found: $CC_OVERRIDE" >&2
    echo "install a cross toolchain or pass --cc /path/to/gcc" >&2
    exit 2
  fi
fi

if [ -z "$CURL_SRC" ] || [ -z "$OUT" ]; then
  echo "usage: build-variants.sh --curl-src DIR --out DIR [--patch FILE]..." >&2
  exit 2
fi

mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
CURL_SRC=$(cd "$CURL_SRC" && pwd)

HOSTARG=""
if [ -n "$HOST" ]; then
  HOSTARG="--host $HOST"
fi

BASE_SHA=$(git -C "$CURL_SRC" rev-parse "$REV")
echo "== baseline $REV = $BASE_SHA"
echo "$BASE_SHA" > "$OUT/baseline.sha"

# Record which compiler produced these numbers, and what it targets by default.
# Without this a result cannot be attributed months later, and a toolchain's
# built-in -mcpu is invisible on the command line.
{
  echo "baseline: $REV = $BASE_SHA"
  echo "host: ${HOST:-(native)}"
  echo "cc: ${CC_OVERRIDE:-cc}"
  echo "cflags: ${CFLAGS_OVERRIDE:-(Makefile default)}"
  "${CC_OVERRIDE:-cc}" --version 2>/dev/null | head -1
  "${CC_OVERRIDE:-cc}" -Q --help=target 2>/dev/null |
    grep -E '^[[:space:]]+-m(cpu|arch|tune)=' | head -3
} > "$OUT/toolchain.txt"
cat "$OUT/toolchain.txt"

build_one() {
  name="$1"
  shift
  wt="$OUT/src/$name"
  prefix="$OUT/curl/$name"

  echo ""
  echo "########## variant: $name"
  rm -rf "$wt" "$prefix"
  mkdir -p "$OUT/src" "$OUT/curl"
  git -C "$CURL_SRC" worktree add --detach --quiet "$wt" "$BASE_SHA"

  for p in "$@"; do
    echo "== apply $(basename "$p")"
    if ! git -C "$wt" apply --whitespace=nowarn "$p"; then
      echo "!! $(basename "$p") does not apply to $BASE_SHA -- skipping variant"
      git -C "$CURL_SRC" worktree remove --force "$wt" >/dev/null 2>&1 || true
      return 1
    fi
  done

  mkdir -p "$prefix"
  if [ -n "$CC_OVERRIDE" ]; then
    CC="$CC_OVERRIDE" export CC
  fi
  if [ -n "$CFLAGS_OVERRIDE" ]; then
    sh "$HERE/build-curl.sh" --src "$wt" --prefix "$prefix" $HOSTARG \
      --cflags "$CFLAGS_OVERRIDE"
  else
    sh "$HERE/build-curl.sh" --src "$wt" --prefix "$prefix" $HOSTARG
  fi

  echo "== bench binary"
  mkdir -p "$OUT/bin"
  # CC and CFLAGS go through the environment rather than the make command
  # line: the Makefile declares them with ?= so the environment wins, and a
  # multi-word CFLAGS survives without any quoting games in this script.
  if [ -n "$CFLAGS_OVERRIDE" ]; then
    CFLAGS="$CFLAGS_OVERRIDE -std=c99 -Wall -Wextra -Wno-unused-parameter"
    export CFLAGS
  fi
  make -C "$BENCH_ROOT" CURL_PREFIX="$prefix" O="$OUT/obj/$name"
  cp "$OUT/obj/$name/curlbench" "$OUT/bin/curlbench-$name"
  echo "== $OUT/bin/curlbench-$name"
  return 0
}

FAILED=""

build_one base || FAILED="$FAILED base"

# One --patch argument is one variant. A comma-separated list is ONE variant
# with those patches stacked in order, which is how a patch that builds on an
# earlier one gets measured: the variant is named after the LAST patch, so
# comparing it against the variant named after its prerequisite gives that
# patch's own contribution rather than the stack's total.
for spec in $PATCHES; do
  files=""
  name=""
  oldifs="$IFS"
  IFS=,
  for p in $spec; do
    IFS="$oldifs"
    abs=$(cd "$(dirname "$p")" && pwd)/$(basename "$p")
    files="$files $abs"
    name=$(basename "$abs" .patch)
    IFS=,
  done
  IFS="$oldifs"
  build_one "$name" $files || FAILED="$FAILED $name"
done

if [ "$DO_ALL" = "1" ] && [ -n "$PATCHES" ]; then
  ABS=""
  for p in $PATCHES; do
    ABS="$ABS $(cd "$(dirname "$p")" && pwd)/$(basename "$p")"
  done
  build_one all $ABS || FAILED="$FAILED all"
fi

echo ""
echo "== binaries in $OUT/bin"
ls -1 "$OUT/bin" 2>/dev/null || true
if [ -n "$FAILED" ]; then
  echo "== variants that failed:$FAILED"
  exit 1
fi
