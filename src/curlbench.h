/* curlbench -- a libcurl microbenchmark harness.
 *
 * Public-API only: every case here drives libcurl through <curl/curl.h>, so
 * the harness links against an ordinary static libcurl and needs no private
 * headers, no build-tree layout and no patches of its own.
 *
 * SPDX-License-Identifier: curl
 */
#ifndef CURLBENCH_H
#define CURLBENCH_H

#include <stddef.h>
#include <stdint.h>

/* One benchmark case.
 *
 * run() must execute exactly `iters` iterations of the workload and return a
 * value derived from the work done. The harness feeds that into a volatile
 * sink so the optimizer cannot delete the workload.
 */
struct bench {
  const char *name;    /* "group/case", used for selection and reporting */
  const char *targets; /* patch ids this case is meant to move, or "-" */
  const char *desc;    /* one line, shown by --list */
  int (*setup)(void **pctx);
  void (*teardown)(void *ctx);
  uint64_t (*run)(void *ctx, uint64_t iters);
  int (*verify)(void *ctx); /* 0 == correct; run under --verify */
};

/* Case tables, each terminated by a zeroed entry. */
extern const struct bench cb_cases_str[];
extern const struct bench cb_cases_multi[];

/* Deterministic 32-bit xorshift, so every architecture runs byte-identical
   workloads regardless of libc rand() differences. */
uint32_t cb_rand(uint32_t *state);

#endif /* CURLBENCH_H */
