/* curlbench -- harness: selection, calibration, timing, statistics, output.
 *
 * SPDX-License-Identifier: curl
 */
#define _POSIX_C_SOURCE 200809L

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/utsname.h>

#include <curl/curl.h>

#include "curlbench.h"

#define CB_VERSION "1"

/* defaults */
#define DEF_REPS      11
#define DEF_MIN_MS    200
#define DEF_WARMUP_MS 20
#define MAX_ITERS     ((uint64_t)1 << 40)

static volatile uint64_t cb_sink;

uint32_t cb_rand(uint32_t *state)
{
  uint32_t x = *state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *state = x;
  return x;
}

/* ---------------------------------------------------------------- clocks */

static double clock_s(clockid_t id)
{
  struct timespec ts;
  if(clock_gettime(id, &ts))
    return -1.0;
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static double wall_s(void)
{
  return clock_s(CLOCK_MONOTONIC);
}

static double cpu_s(void)
{
  return clock_s(CLOCK_PROCESS_CPUTIME_ID);
}

/* ------------------------------------------------------------ statistics */

static int cmp_double(const void *a, const void *b)
{
  double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}

static double median_of(double *v, size_t n)
{
  qsort(v, n, sizeof(*v), cmp_double);
  if(!n)
    return 0.0;
  if(n & 1)
    return v[n / 2];
  return (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

/* median absolute deviation, a robust spread that a single slow rep (a
   migration, an interrupt storm) cannot inflate the way stddev can */
static double mad_of(const double *v, size_t n, double med)
{
  double *d;
  double r;
  size_t i;
  if(n < 2)
    return 0.0;
  d = malloc(n * sizeof(*d));
  if(!d)
    return 0.0;
  for(i = 0; i < n; i++)
    d[i] = (v[i] > med) ? (v[i] - med) : (med - v[i]);
  r = median_of(d, n);
  free(d);
  return r;
}

/* --------------------------------------------------------------- results */

struct result {
  const struct bench *b;
  uint64_t iters;
  unsigned reps;
  double wall_ns_med;
  double wall_ns_min;
  double wall_ns_mad;
  double cpu_ns_med;
  uint64_t checksum;
  int verified; /* -1 no verify fn, 0 ok, 1 failed */
};

/* ------------------------------------------------------------------ core */

static int run_case(const struct bench *b, unsigned reps, double min_ms,
                    uint64_t forced_iters, int do_verify,
                    struct result *out)
{
  void *ctx = NULL;
  uint64_t iters;
  double *wall, *cpu;
  unsigned r;
  uint64_t sum = 0;

  if(b->setup && b->setup(&ctx)) {
    fprintf(stderr, "%s: setup failed\n", b->name);
    return 1;
  }

  memset(out, 0, sizeof(*out));
  out->b = b;
  out->verified = -1;
  if(do_verify && b->verify) {
    out->verified = b->verify(ctx) ? 1 : 0;
    if(out->verified) {
      fprintf(stderr, "%s: VERIFY FAILED\n", b->name);
      if(b->teardown)
        b->teardown(ctx);
      return 1;
    }
  }

  /* warm up caches, branch predictors and any lazy allocation the workload
     does on first use, so calibration does not measure first-call costs */
  sum += b->run(ctx, 1);
  {
    double t0 = wall_s();
    while((wall_s() - t0) * 1000.0 < DEF_WARMUP_MS)
      sum += b->run(ctx, 1);
  }

  /* calibrate: grow the iteration count until one rep spans min_ms, so the
     measured interval is far larger than the clock's resolution */
  if(forced_iters)
    iters = forced_iters;
  else {
    double t;
    iters = 1;
    for(;;) {
      double t0 = wall_s();
      sum += b->run(ctx, iters);
      t = (wall_s() - t0) * 1000.0;
      if(t >= min_ms || iters >= MAX_ITERS)
        break;
      if(t < min_ms / 64.0)
        iters *= 16; /* far off: jump */
      else
        iters *= 2;
    }
  }

  wall = calloc(reps, sizeof(*wall));
  cpu = calloc(reps, sizeof(*cpu));
  if(!wall || !cpu) {
    free(wall);
    free(cpu);
    if(b->teardown)
      b->teardown(ctx);
    return 1;
  }

  for(r = 0; r < reps; r++) {
    double w0, c0, w1, c1;
    c0 = cpu_s();
    w0 = wall_s();
    sum += b->run(ctx, iters);
    w1 = wall_s();
    c1 = cpu_s();
    wall[r] = ((w1 - w0) * 1e9) / (double)iters;
    cpu[r] = ((c1 - c0) * 1e9) / (double)iters;
  }

  cb_sink = sum;
  out->iters = iters;
  out->reps = reps;
  out->checksum = sum;
  out->wall_ns_med = median_of(wall, reps); /* sorts wall */
  out->wall_ns_min = wall[0];
  out->wall_ns_mad = mad_of(wall, reps, out->wall_ns_med);
  out->cpu_ns_med = median_of(cpu, reps);

  free(wall);
  free(cpu);
  if(b->teardown)
    b->teardown(ctx);
  return 0;
}

/* -------------------------------------------------------------- reporting */

static void print_header(void)
{
  printf("%-28s %-17s %12s %12s %10s %12s\n",
         "case", "patches", "ns/op", "cpu ns/op", "mad%", "ops/s");
  printf("%-28s %-17s %12s %12s %10s %12s\n",
         "----", "-------", "-----", "---------", "----", "-----");
}

static void print_result(const struct result *r)
{
  double madpct = r->wall_ns_med > 0.0 ?
    (100.0 * r->wall_ns_mad / r->wall_ns_med) : 0.0;
  printf("%-28s %-17s %12.1f %12.1f %9.2f%% %12.0f\n",
         r->b->name, r->b->targets, r->wall_ns_med, r->cpu_ns_med,
         madpct, r->wall_ns_med > 0.0 ? 1e9 / r->wall_ns_med : 0.0);
}

static void json_escape(const char *s)
{
  for(; *s; s++) {
    if(*s == '"' || *s == '\\')
      printf("\\%c", *s);
    else
      putchar(*s);
  }
}

static void print_json(const struct result *rs, size_t n, const char *label)
{
  struct utsname u;
  size_t i;
  memset(&u, 0, sizeof(u));
  uname(&u);
  printf("{\n");
  printf("  \"tool\": \"curlbench\",\n");
  printf("  \"format\": %s,\n", CB_VERSION);
  printf("  \"label\": \"");
  json_escape(label ? label : "");
  printf("\",\n");
  printf("  \"libcurl\": \"");
  json_escape(curl_version());
  printf("\",\n");
  printf("  \"machine\": \"%s\",\n", u.machine);
  printf("  \"sysname\": \"%s\",\n", u.sysname);
  printf("  \"release\": \"%s\",\n", u.release);
  printf("  \"results\": [\n");
  for(i = 0; i < n; i++) {
    printf("    {\"name\": \"%s\", \"targets\": \"%s\", "
           "\"iters\": %" PRIu64 ", \"reps\": %u, "
           "\"ns_med\": %.4f, \"ns_min\": %.4f, \"ns_mad\": %.4f, "
           "\"cpu_ns_med\": %.4f, \"checksum\": %" PRIu64 "}%s\n",
           rs[i].b->name, rs[i].b->targets, rs[i].iters, rs[i].reps,
           rs[i].wall_ns_med, rs[i].wall_ns_min, rs[i].wall_ns_mad,
           rs[i].cpu_ns_med, rs[i].checksum,
           (i + 1 < n) ? "," : "");
  }
  printf("  ]\n}\n");
}

/* ------------------------------------------------------------------- main */

static const struct bench *const *all_tables(void)
{
  static const struct bench *tables[] = {
    cb_cases_str, cb_cases_printf, cb_cases_multi, NULL
  };
  return tables;
}

static int matches(const struct bench *b, const char *filter,
                   const char *targets)
{
  if(filter && !strstr(b->name, filter))
    return 0;
  if(targets && !strstr(b->targets, targets))
    return 0;
  return 1;
}

static void usage(void)
{
  printf(
    "usage: curlbench [options]\n"
    "  --list             list cases and exit\n"
    "  --filter SUB       only cases whose name contains SUB\n"
    "  --targets ID       only cases whose patch list contains ID\n"
    "  --reps N           timed repetitions per case (default %d)\n"
    "  --min-ms N         target milliseconds per repetition (default %d)\n"
    "  --iters N          force the iteration count (skips calibration;\n"
    "                     use this under perf record for reproducibility)\n"
    "  --verify           run each case's correctness check first\n"
    "  --json             machine-readable output for compare.py\n"
    "  --label TEXT       tag the run (variant name) in JSON output\n"
    "  --version          print libcurl version and exit\n",
    DEF_REPS, DEF_MIN_MS);
}

int main(int argc, char **argv)
{
  const char *filter = NULL, *targets = NULL, *label = NULL;
  unsigned reps = DEF_REPS;
  double min_ms = DEF_MIN_MS;
  uint64_t forced = 0;
  int do_list = 0, do_json = 0, do_verify = 0, failed = 0;
  struct result *rs = NULL;
  size_t nres = 0, cap = 0;
  const struct bench *const *t;
  int i;

  for(i = 1; i < argc; i++) {
    const char *a = argv[i];
    if(!strcmp(a, "--list"))
      do_list = 1;
    else if(!strcmp(a, "--json"))
      do_json = 1;
    else if(!strcmp(a, "--verify"))
      do_verify = 1;
    else if(!strcmp(a, "--version")) {
      printf("%s\n", curl_version());
      return 0;
    }
    else if(!strcmp(a, "--help") || !strcmp(a, "-h")) {
      usage();
      return 0;
    }
    else if(!strcmp(a, "--filter") && i + 1 < argc)
      filter = argv[++i];
    else if(!strcmp(a, "--targets") && i + 1 < argc)
      targets = argv[++i];
    else if(!strcmp(a, "--label") && i + 1 < argc)
      label = argv[++i];
    else if(!strcmp(a, "--reps") && i + 1 < argc)
      reps = (unsigned)strtoul(argv[++i], NULL, 10);
    else if(!strcmp(a, "--min-ms") && i + 1 < argc)
      min_ms = strtod(argv[++i], NULL);
    else if(!strcmp(a, "--iters") && i + 1 < argc)
      forced = strtoull(argv[++i], NULL, 10);
    else {
      fprintf(stderr, "unknown or incomplete option: %s\n", a);
      usage();
      return 2;
    }
  }
  if(!reps)
    reps = 1;

  if(do_list) {
    for(t = all_tables(); *t; t++) {
      const struct bench *b;
      for(b = *t; b->name; b++) {
        if(matches(b, filter, targets))
          printf("%-28s %-17s %s\n", b->name, b->targets, b->desc);
      }
    }
    return 0;
  }

  if(cpu_s() < 0.0)
    fprintf(stderr, "warning: CLOCK_PROCESS_CPUTIME_ID unavailable, "
            "cpu ns/op will read 0\n");

  if(curl_global_init(CURL_GLOBAL_NOTHING) != CURLE_OK) {
    fprintf(stderr, "curl_global_init failed\n");
    return 1;
  }

  if(!do_json) {
    printf("# %s\n", curl_version());
    printf("# reps=%u min-ms=%.0f%s\n", reps, min_ms,
           forced ? " (iteration count forced)" : "");
    print_header();
  }

  for(t = all_tables(); *t; t++) {
    const struct bench *b;
    for(b = *t; b->name; b++) {
      struct result r;
      if(!matches(b, filter, targets))
        continue;
      if(run_case(b, reps, min_ms, forced, do_verify, &r)) {
        failed = 1;
        continue;
      }
      if(nres == cap) {
        size_t ncap = cap ? cap * 2 : 32;
        struct result *nrs = realloc(rs, ncap * sizeof(*nrs));
        if(!nrs) {
          fprintf(stderr, "out of memory\n");
          curl_global_cleanup();
          return 1;
        }
        rs = nrs;
        cap = ncap;
      }
      rs[nres++] = r;
      if(!do_json) {
        print_result(&r);
        fflush(stdout);
      }
    }
  }

  if(do_json)
    print_json(rs, nres, label);

  free(rs);
  curl_global_cleanup();
  return failed ? 1 : 0;
}
