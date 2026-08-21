#!/bin/sh
# Run a set of bench binaries A/B/A/B... and emit one JSON per (variant, pass).
#
# usage: run.sh --bin DIR --out DIR [--passes N] [--reps N] [--min-ms N]
#               [--filter SUB] [--targets ID] [--cpu N]
#
# Why interleave: a device warms up, a governor ramps, another process wakes.
# Running every pass of variant A before variant B lets that drift land
# entirely on one side and read as a result. Alternating passes spreads it
# across both. Pass 1 of every variant is discarded by compare.py unless told
# otherwise, since first-touch page faults land there.
#
# The variance notes that matter on a device:
#   - pin the frequency governor to performance before measuring, or the same
#     code measures differently depending on what ran before it
#   - pin to one CPU (--cpu) so migration between cores of different speeds
#     does not show up as a patch effect
#   - stop anything else that is running; a single competing thread swamps a
#     1% effect

set -e

BIN=""
OUT=""
PASSES=3
REPS=11
MIN_MS=200
FILTER=""
TARGETS=""
CPU=""

while [ $# -gt 0 ]; do
  case "$1" in
    --bin) BIN="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --passes) PASSES="$2"; shift 2 ;;
    --reps) REPS="$2"; shift 2 ;;
    --min-ms) MIN_MS="$2"; shift 2 ;;
    --filter) FILTER="$2"; shift 2 ;;
    --targets) TARGETS="$2"; shift 2 ;;
    --cpu) CPU="$2"; shift 2 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

if [ -z "$BIN" ] || [ -z "$OUT" ]; then
  echo "usage: run.sh --bin DIR --out DIR [--passes N]" >&2
  exit 2
fi

mkdir -p "$OUT"

ARGS="--json --reps $REPS --min-ms $MIN_MS"
if [ -n "$FILTER" ]; then
  ARGS="$ARGS --filter $FILTER"
fi
if [ -n "$TARGETS" ]; then
  ARGS="$ARGS --targets $TARGETS"
fi

PIN=""
if [ -n "$CPU" ]; then
  if command -v taskset >/dev/null 2>&1; then
    # A hex mask, not -c: busybox taskset (what an embedded target ships)
    # rejects -c, while both busybox and util-linux accept a bare mask.
    MASK=1
    i=0
    while [ "$i" -lt "$CPU" ]; do
      MASK=$((MASK * 2))
      i=$((i + 1))
    done
    PIN="taskset $(printf '0x%x' "$MASK")"
  else
    echo "note: taskset not present, running unpinned" >&2
  fi
fi

# Report the environment once: without this a number in a file is unreadable
# six weeks later.
{
  echo "date: $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
  echo "uname: $(uname -a)"
  echo "passes: $PASSES reps: $REPS min_ms: $MIN_MS cpu: ${CPU:-any}"
  if [ -r /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ]; then
    echo "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"
  else
    echo "governor: (no cpufreq)"
  fi
  if [ -r /proc/loadavg ]; then
    echo "loadavg: $(cat /proc/loadavg)"
  fi
} > "$OUT/environment.txt"
cat "$OUT/environment.txt"

# One correctness pass per binary before any timing: a patch that changed
# behavior must not be reported as a speedup.
for b in "$BIN"/curlbench-*; do
  [ -x "$b" ] || continue
  v=$(basename "$b" | sed 's/^curlbench-//')
  echo "== verify $v"
  if ! $PIN "$b" --verify --reps 1 --min-ms 1 >"$OUT/verify-$v.txt" 2>&1; then
    echo "!! $v FAILED VERIFICATION -- see $OUT/verify-$v.txt" >&2
    exit 1
  fi
done

p=1
while [ "$p" -le "$PASSES" ]; do
  for b in "$BIN"/curlbench-*; do
    [ -x "$b" ] || continue
    v=$(basename "$b" | sed 's/^curlbench-//')
    echo "== pass $p variant $v"
    $PIN "$b" $ARGS --label "$v" > "$OUT/$v.pass$p.json"
  done
  p=$((p + 1))
done

# Marker written only after every pass has finished. A waiter must look for
# this, never for the JSON files: a shell creates a redirect target before the
# program writes a byte into it, so file existence says nothing about whether
# the run completed.
date -u '+%Y-%m-%dT%H:%M:%SZ' > "$OUT/COMPLETE"

echo ""
echo "== results in $OUT"
ls -1 "$OUT"
