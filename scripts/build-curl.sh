#!/bin/sh
# Build one static libcurl into a private prefix, with a feature set chosen to
# be identical for every variant so the only difference between two runs is
# the patch under test.
#
# usage: build-curl.sh --src DIR --prefix DIR [--host TRIPLE] [--jobs N]
#                      [--cflags "..."]
#
# Deliberate configure choices:
#   --disable-shared      one .a, so the bench binary can be fully static
#   --without-ssl etc.    none of the patches touch TLS or compression, and
#                         dropping them keeps the binary self-contained
#   --disable-unity       unity mode would inline across translation units and
#                         mask exactly the call overhead some patches remove
#   --enable-optimize     -O2, matching a release build
#   no LTO                same reason as unity: it changes what is measurable
#
# The result is not meant to match any particular product build: it is a controlled
# baseline that anyone (including upstream) can reproduce from a clean clone.

set -e

SRC=""
PREFIX=""
HOST=""
JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
EXTRA_CFLAGS="-O2 -g"

while [ $# -gt 0 ]; do
  case "$1" in
    --src) SRC="$2"; shift 2 ;;
    --prefix) PREFIX="$2"; shift 2 ;;
    --host) HOST="$2"; shift 2 ;;
    --jobs) JOBS="$2"; shift 2 ;;
    --cflags) EXTRA_CFLAGS="$2"; shift 2 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

if [ -z "$SRC" ] || [ -z "$PREFIX" ]; then
  echo "usage: build-curl.sh --src DIR --prefix DIR [--host TRIPLE]" >&2
  exit 2
fi

if [ ! -f "$SRC/configure" ]; then
  echo "== autoreconf in $SRC"
  ( cd "$SRC" && autoreconf -fi >/dev/null )
fi

BUILD="$PREFIX/build"
mkdir -p "$BUILD"

HOSTARG=""
if [ -n "$HOST" ]; then
  HOSTARG="--host=$HOST"
fi

echo "== configure ($SRC -> $PREFIX)"
cd "$BUILD"
CFLAGS="$EXTRA_CFLAGS" "$SRC/configure" \
  --prefix="$PREFIX" \
  $HOSTARG \
  --disable-shared \
  --enable-static \
  --enable-optimize \
  --disable-debug \
  --disable-curldebug \
  --disable-unity \
  --without-ssl \
  --without-zlib \
  --without-brotli \
  --without-zstd \
  --without-libpsl \
  --without-libidn2 \
  --without-nghttp2 \
  --without-libssh2 \
  --without-librtmp \
  --disable-ares \
  --enable-http \
  --disable-ftp \
  --disable-file \
  --disable-ldap \
  --disable-ldaps \
  --disable-rtsp \
  --disable-dict \
  --disable-telnet \
  --disable-tftp \
  --disable-pop3 \
  --disable-imap \
  --disable-smtp \
  --disable-gopher \
  --disable-smb \
  --disable-mqtt \
  --disable-manual \
  --disable-libcurl-option \
  --disable-netrc \
  >"$PREFIX/configure.log" 2>&1 || {
    echo "configure failed, tail of $PREFIX/configure.log:" >&2
    tail -30 "$PREFIX/configure.log" >&2
    exit 1
  }

echo "== make -j$JOBS"
make -j"$JOBS" >"$PREFIX/build.log" 2>&1 || {
  echo "build failed, tail of $PREFIX/build.log:" >&2
  tail -40 "$PREFIX/build.log" >&2
  exit 1
}
make install >>"$PREFIX/build.log" 2>&1

echo "== installed: $PREFIX"
"$PREFIX/bin/curl-config" --version 2>/dev/null || true
