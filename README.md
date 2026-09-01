# curlbench

A benchmark harness for evaluating candidate libcurl performance patches, one
patch at a time, on the hardware the patch is meant to help.

It exists because "this looks faster" is not a reviewable claim. Each case here
is a public-API workload chosen to isolate one internal cost, so a patch can be
credited with the thing it actually changes and nothing else.

## What it is not

It is not a curl feature test, and it is not a replacement for curl's own test
suite. It measures; correctness is checked only to the extent needed to make a
measurement trustworthy (`--verify`, run before any timing).

## Design constraints

**Public API only.** Every case drives libcurl through its installed headers,
`curl/curl.h` and `curl/mprintf.h`. No private headers, no build-tree layout, no
compiling the bench inside the curl tree. This keeps the harness working across curl versions and
makes it something a maintainer can run without adopting anything.

**Self-contained binaries.** libcurl is linked statically and, by default, so
is libc. The result is one file to copy onto a target device: no runtime
dependencies, no TLS library, no test server to install, no network peer.

**One variable at a time.** Every variant is built from the same base commit,
with the same configure flags and the same compiler flags. The only difference
between two binaries is the patch under test.

## Layout

    src/                       the harness and the cases
    scripts/build-curl.sh      build one static libcurl with fixed configure flags
    scripts/build-variants.sh  base + one binary per patch (+ --all)
    scripts/rebuild-bench.sh   recompile the bench for existing variants
    scripts/run.sh             interleaved A/B/A/B runs, with a verify gate
    scripts/compare.py         robust A/B comparison of the JSON output
    scripts/summarize.py       many variants at once, against the noise controls
    controls/                  dead-code patches that measure recompile noise
    targets/                   per-architecture compiler and tune profiles

## Quick start (native)

    ./scripts/build-curl.sh --src /path/to/curl-git --prefix /tmp/cb/base
    make CURL_PREFIX=/tmp/cb/base
    ./build/curlbench --list
    ./build/curlbench --verify

## Comparing a patch against master

    ./scripts/build-variants.sh \
        --curl-src /path/to/curl-git \
        --out /tmp/cb \
        --patch /path/to/strcase-inline-the-raw-case-conversions.patch \
        --patch /path/to/multi-seed-the-transfer-table-at-64.patch \
        --patch controls/null-layout.patch

    ./scripts/run.sh --bin /tmp/cb/bin --out /tmp/cb/results --passes 5 --cpu 2
    ./scripts/compare.py /tmp/cb/results --drop-first-pass

`build-variants.sh` gives each variant its own git worktree of the base commit,
so the clone is never modified and variants cannot contaminate each other. A
patch that does not apply is reported and its variant skipped, never silently
dropped.

## Cross-compiling

Pick a target profile; it supplies the host triple, the tune flags and a
default compiler, any of which can be overridden.

    ./scripts/build-variants.sh --target arm64 --curl-src ... --out ... --patch ...
    ./scripts/build-variants.sh --target ppc   --cc /path/to/gcc ...

Profiles in `targets/`:

| profile | tuning | notes |
|---|---|---|
| `amd64` | generic `-O2` | out-of-order and wide; hides per-byte costs |
| `arm64` | `-mcpu=cortex-a55` | ARMv8.2-A in-order; pays per-call latency in full |
| `ppc`   | `-mcpu=e300c3` | 32-bit big-endian, single core, no L2 |

### Using an already-installed toolchain

If a cross toolchain built for the actual target is already installed, prefer it
over a profile and name it by its triple:

    ./scripts/build-variants.sh --toolchain powerpc-myboard-linux-gnu \
        --curl-src ... --out ... --patch ...

`--toolchain X` sets both the configure host and `CC` to `X-gcc`, and adds **no**
tune flags — a toolchain built for a part already targets the right kernel and
libc and already defaults to the right `-mcpu`, and overriding those defaults is
how a measurement stops describing the device. Add `--cflags` to override
anyway, or `--cc /path/to/X-gcc` if it is not on `PATH`.

This is the escape hatch for targets a current distribution toolchain cannot
serve. The clearest case is an old kernel: distribution glibc stamps a minimum
kernel into every binary (`readelf -n` shows `NT_GNU_ABI_TAG`), and if the target
runs something older the binary exits before `main()`. A vendor or crosstool-NG
toolchain built for that kernel does not have the problem.

Every run writes `toolchain.txt` next to the binaries recording the compiler,
its version and its built-in `-mcpu`, so a number stays attributable.

### Distribution toolchains

Otherwise the profiles use distribution cross packages, so a result is
reproducible from a package name:

    apt install gcc-11-aarch64-linux-gnu gcc-11-powerpc-linux-gnu

Each pulls in the cross libc development files, including the static libc the
default `STATIC=1` link needs. Use the *versioned* package names: on Ubuntu
22.04 the unversioned metapackages are uninstallable (pinned at 11.2.0 against
an archive that moved to 11.4.0). The profiles fall back to the `-gcc-11`
binary name when the metapackage's unversioned symlink is absent, so
`--target arm64` still works with no extra flags.

Read `targets/ppc.env` before building for it. A current cross glibc requires
kernel 3.2 or newer, so a static binary will not start on an older target —
check `uname -r` there first. Big-endian plus a 32-bit `size_t` is also a real
correctness surface: run `--verify` on the target before reading any timing.

## Measurement methodology

Per case, the harness warms up, then calibrates the iteration count until one
repetition spans `--min-ms` (default 200 ms), so the measured interval dwarfs
clock resolution. It then takes `--reps` timed repetitions (default 11) and
reports the **median** ns/op with a **median absolute deviation**, plus CPU time
from `CLOCK_PROCESS_CPUTIME_ID` alongside monotonic wall time.

Median and MAD rather than mean and standard deviation: a stalled repetition is
a one-sided outlier, and a mean lets one of them move the answer.

Each case returns a checksum that the harness feeds to a volatile sink, so no
workload can be optimized away.

`compare.py` calls a delta only when it exceeds a noise band built from both
sides' spread (and at least 1%). Anything inside the band prints `-`, which
means *this run cannot tell*, not *no effect*.

### Getting numbers worth quoting

- **Pin the frequency governor** to `performance` before measuring. On a DVFS
  part the same code measures differently depending on what ran before it.
- **Pin to one CPU** (`run.sh --cpu N`). Migration between cores of different
  speeds otherwise reads as a patch effect.
- **Interleave variants** — `run.sh` alternates passes rather than running all
  of A then all of B, so warm-up and drift land on both sides instead of one.
  Discard pass 1 (`compare.py --drop-first-pass`); first-touch faults live there.
- **Check what else is running.** A single competing thread swamps a 1% effect.
  `run.sh` records governor, load average and `uname` next to the results,
  because a number in a file is unreadable six weeks later without them.
- **Measure the noise before believing a result.** See the next section: run
  the same binary twice, and run a dead-code patch, and take the larger of the
  two as the bar your result has to clear.

### Two kinds of noise

Before believing any number, know how much the timings wobble. Anything smaller
than the wobble is not a result. Two different wobbles matter here, and they are
far apart.

**Repeat noise** is what the *same* binary measures twice. Copy
`curlbench-base` to `curlbench-base2` so the identical file runs under two
labels; `summarize.py` recognises `base2` and reports it. Any difference is
measurement noise alone.

**Recompile noise** is the one people forget. Rebuilding with a change that
does nothing still moves every machine instruction to a different address, and
CPUs care where code sits: instructions are fetched in fixed-size blocks, so a
loop that fits in one block beats the identical loop straddling two, and the
branch predictor indexes its history by address bits, so moving code makes
unrelated branches start or stop colliding in that table. Same logic,
recompiled, different speed.

`controls/null-layout.patch` isolates exactly that. It appends dead
`__attribute__((used))` code to `lib/strcase.c` and `lib/multi.c`. The code
never executes, so it cannot change behaviour — it only shifts everything else
to new addresses. Whatever speedup it appears to produce is pure address
shuffling, and that is the bar a real result has to clear.

    ./scripts/build-variants.sh --curl-src ... --out /tmp/cb \
        --patch controls/null-layout.patch --patch /path/to/real.patch
    cp /tmp/cb/bin/curlbench-base /tmp/cb/bin/curlbench-base2
    ./scripts/run.sh --bin /tmp/cb/bin --out /tmp/cb/results --passes 5 --cpu 2
    ./scripts/summarize.py arm64=/tmp/cb/results --drop-first \
        --layout=arm64=/tmp/cb/results

The control's variant name must begin with `null` or `summarize.py` scores it
as if it were a real patch instead of a control.

Measured on the three targets, worst case per platform:

| target | repeat noise | recompile noise |
|---|---|---|
| Cortex-A55, in-order | 0.12% | **0.04%** |
| e300c3, 250 MHz, no L2 | 0.23% | up to 11% |
| x86-64 desktop | up to 4.9% | up to **64.9%** |

**The in-order parts are the trustworthy ones.** That is not a slogan; it is a
30x difference in repeat noise and a three-orders-of-magnitude difference in
recompile noise on the same cases.

**The desktop is pathological, and worth understanding before trusting one.**
Its worst case is `str/equal_miss_early`, a ten-instruction workload that takes
3.6 ns. Dead code makes it appear 64.9% faster — measured four times, on idle
and busy machines, at 64.86%, 64.87%, 64.87% and 64.87%. It is perfectly
reproducible and perfectly useless. At ten instructions, which fetch block the
loop lands in swamps everything the code does. The same case takes 16.8 ns on
the A55 and 163 ns on the e300c3, where it behaves normally.

The desktop's repeat noise is also intrinsic, not contamination: 4.86% on an
idle machine over seven passes, against 4.81% on a busy one. Boost clocks,
thermal management and 32 cores make a 26 ns measurement squishy no matter how
quiet the box is.

**Where the desktop is still useful:** comparing two *patched* builds against
each other. The instability lives in the unpatched baseline — `str/equal_hit`
measures anywhere from 25.7 to 27.6 ns run to run, so any percentage computed
against it inherits that wander. Two patched builds are both steady, and that
comparison reproduced to two decimal places across four runs (−54.34%,
−54.35%). Use the desktop to choose between candidate patches; use the devices
to say how much faster a patch actually is.

One more trap, seen twice in real runs: a patch touching only `lib/strcase.c`,
`lib/strcase.h` and `lib/cookie.c` produced a "faster" verdict on
`multi/lifecycle`, which it cannot reach — −4.38% in one run and −3.28% in
another. A false positive here is stable enough to survive a single run, so
measure anything interesting twice.

Read the **per-case** figure, never a median across cases. On the desktop the
median recompile noise moved three points between two runs of the same
binaries, so it is not a threshold worth quoting; per-case it ranges from 0.05%
to 64.9%.

## Measured so far

Five interleaved passes per variant, first discarded, median of per-pass
medians, governor pinned to `performance` where the part has one, pinned to one
CPU on multi-core parts. **The device columns carry the argument** — their
repeat noise is 0.12% and 0.23% against the desktop's 4.9%. The desktop figure
is shown for completeness and should be read as the least reliable of the
three.

`strcase: inline the raw case conversions` (macros over the existing tables):

| case | Cortex-A55 | e300c3 | x86-64 |
|---|---|---|---|
| `str/equal_hit` | −49.1% | −51.3% | −83.6% |
| `str/equal_folded` | −49.2% | −51.4% | −83.5% |
| `str/equal_miss_late` | −49.1% | −51.3% | −83.4% |
| `str/nequal_prefix` | −42.8% | −54.6% | −71.6% |
| `str/header_scan` | −43.4% | −40.8% | −69.6% |
| recompile noise, these cases | 0.04% | up to 11% | up to 64.9% |

On the A55 that is three orders of magnitude above the noise, and on the e300c3
about five times it. The desktop number is larger than either device figure only
because its *unpatched* baseline wanders 25.7–27.6 ns between runs, and a
percentage computed against a moving denominator moves with it.

This is the second formulation of that patch. The first replaced the lookup
tables with range tests and arithmetic; measured head to head, keeping the tables
and inlining the lookup won by 26% on the A55, 41% on the e300c3 and 54% on
x86-64. Both formulations remove the same out-of-line call, so the difference is
purely arithmetic versus one L1 load — and on an in-order core with weak branch
prediction, the load wins comfortably. Choosing between two candidate patches is
exactly what a harness like this is for.

`multi: seed the transfer table at 64`:

| case | Cortex-A55 | e300c3 | x86-64 |
|---|---|---|---|
| `multi/lifecycle` | −3.2% | −5.6% | −10.8% / −10.4% |
| recompile noise, this case | 0.10% | 0.13% | 2.2 / 3.4% |

A much smaller change, but the device figures sit 30–40x above their own
recompile noise, which is what makes them worth quoting at all. The desktop pair
also reproduced across two runs. This one rests as much on the mechanism — a
4 KB row table zeroed per `curl_multi_init()` when the table already grows
geometrically on demand — as on the measurement.

`mprintf: emit output in blocks rather than byte-by-byte` (staging output and
emitting literal runs, `%s` bodies, digit runs and padding as runs):

| case | Cortex-A55 | e300c3 | x86-64 |
|---|---|---|---|
| `printf/soap_body` | −79.0% | −75.5% | −83.5% |
| `printf/header_pair` | −76.1% | −65.5% | −79.5% |
| `printf/request_line` | −66.6% | −59.7% | −73.0% |
| `printf/numbers` | −57.2% | −44.3% | −64.3% |
| `printf/complex` | −48.0% | −33.9% | −57.0% |
| `printf/snprintf_soap` | −45.0% | −35.2% | −59.2% |
| `printf/snprintf_header` | −41.5% | −33.7% | −51.4% |
| `printf/snprintf_upstream` | −10.6% | **+1.2%** | −24.6% |
| `printf/snprintf_small` | −8.5% | **+2.3%** | −18.6% |
| repeat noise, these cases | 0.40% | 0.31% | 1.1% |
| recompile noise, these cases | 0.84% | 0.24%* | 5.4% |

The first five cases drive `curl_maprintf()`, the rest `curl_msnprintf()` into a
caller-supplied buffer. *The e300c3 recompile figure is the per-case one for the
four `snprintf` rows; on `numbers`, `complex` and `request_line` the same control
reaches 6.5% there, so read those three against a much looser bar. Rebuilding
every variant from scratch and repeating the whole run reproduced each figure to
within 0.4 points.

**The two positive numbers are a real regression, not noise.** Of five interleaved
passes with the first discarded, every patched pass is slower than every baseline
pass — for `printf/snprintf_small`, 7441/7444/7439/7459 ns against
7270/7288/7303/7262 — and `controls/null-mprintf.patch` reads −0.24% on that same
case, which rules out code layout. On the slowest part, short conversion-heavy
`msnprintf` formats lose slightly.

The cause is that a run callback ends in a `memcpy`, and these two cases have runs
of only a handful of bytes: one separator, four digits, a short host name. Where
`memcpy` is an out-of-line call with size and alignment dispatch, that call costs
more than moving the bytes, so staging gives back a little of what it saves.

That diagnosis was tested rather than assumed, and the obvious cure was rejected on
the measurement. Copying runs shorter than 16 bytes with an inline loop and keeping
`memcpy` above that does fix the e300c3: `printf/snprintf_upstream` goes from a
regression to −0.04%, and every other case there improves, up to −8.2% on
`printf/request_line`. On x86-64 the same change costs **+23.0% on
`printf/soap_body` and +29.5% on `printf/snprintf_soap`**, and on the A55 +1.7% and
+2.7%. The threshold does not protect those two because their runs genuinely are
2 to 21 bytes, and there `memcpy` wins at any size. One constant cannot serve all
three parts, so the regression is documented rather than traded away.

Three things in this table are worth more than the percentages:

- **It is not only the growable-buffer sink.** A caller-supplied buffer pays almost
  nothing per byte, and `printf/snprintf_header` and `printf/snprintf_soap` still
  move 33-45% on the devices. What sets the size of the win is the shape of the
  format, not which sink receives the bytes.
- **`printf/snprintf_upstream` is curl's own `tests/perf/snprintf.c` format**,
  copied verbatim so a number from that test and a number from here are the same
  measurement. Eight conversions with 4-9 bytes of literal between them is the
  least favourable shape here for anything that batches output, and it ranges from
  −24.6% to +1.2% across three parts while the long-run cases stay within 8 points
  of each other. A single-platform figure for that case says very little, which is
  most of the reason to have it.
- **The pairs are what license the first claim.** `printf/snprintf_header` and
  `printf/snprintf_soap` hold the format, the arguments and the output bytes
  identical to `printf/header_pair` and `printf/soap_body` and vary only the sink,
  so a pair's difference is the output path and nothing else.

Two cautions that came out of these runs rather than out of theory:

- `str/equal_miss_early` is unmeasurable on the desktop. Dead code alone makes
  it look 64.9% faster, reproducibly. It is fine on both devices, where the same
  case takes 16.8 ns and 163 ns instead of 3.6 ns.
- A patch touching only `lib/strcase.*` and `lib/cookie.c` produced a "faster"
  verdict on `multi/lifecycle` on the desktop, −4.38% in one run and −3.28% in
  another. It cannot reach that code. Measure anything interesting twice.

## Profiling a single case

`--iters` forces the iteration count, which is what makes a profile
reproducible:

    perf record -g ./build/curlbench --filter str/header_scan --iters 2000000 --reps 1
    perf report --stdio

## Case selection

`--list` prints every case with the patches it targets.
`--targets strcase-inline` selects the cases relevant to one patch;
`--filter multi/` selects a group.

The patch ids are only labels — a case is a workload, not a verdict. When a
case moves, confirm the mechanism in a profile before believing the label.

## Configure flags, and why they are fixed

`build-curl.sh` pins one feature set for every variant: static only, no TLS, no
compression, no alternative resolvers, HTTP only. Two choices are deliberate
and worth knowing about:

- **`--disable-unity`.** Unity mode compiles many source files as one
  translation unit, which lets the compiler inline across them. That would mask
  exactly the cross-file call overhead some patches exist to remove.
- **No LTO**, for the same reason.

If a patch's win comes from removing a call that LTO would also have removed,
that has to be stated rather than discovered later. Build with LTO on as a
second data point when a patch is in that category.
