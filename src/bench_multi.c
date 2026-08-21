/* curlbench: handle lifecycle cases.
 *
 * curl_multi_init() sizes the transfer table and its bitsets up front, and
 * curl_multi_cleanup() walks the table to clear it. Applications that run a
 * handful of concurrent transfers pay both costs on every multi handle.
 *
 * SPDX-License-Identifier: curl
 */
#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>

#include <curl/curl.h>

#include "curlbench.h"

#define FANOUT 8

static uint64_t run_multi_lifecycle(void *ctx, uint64_t iters)
{
  uint64_t acc = 0;
  (void)ctx;
  while(iters--) {
    CURLM *m = curl_multi_init();
    if(m) {
      acc++;
      curl_multi_cleanup(m);
    }
  }
  return acc;
}

static uint64_t run_easy_lifecycle(void *ctx, uint64_t iters)
{
  uint64_t acc = 0;
  (void)ctx;
  while(iters--) {
    CURL *e = curl_easy_init();
    if(e) {
      acc++;
      curl_easy_cleanup(e);
    }
  }
  return acc;
}

/* multi handle plus a fan-out's worth of easy handles added and removed: the
   full per-round handle cost of a parallel request batch, with no I/O. */
static uint64_t run_multi_fanout_setup(void *ctx, uint64_t iters)
{
  uint64_t acc = 0;
  (void)ctx;
  while(iters--) {
    CURL *e[FANOUT];
    CURLM *m = curl_multi_init();
    int i;
    if(!m)
      continue;
    for(i = 0; i < FANOUT; i++) {
      e[i] = curl_easy_init();
      if(e[i]) {
        curl_easy_setopt(e[i], CURLOPT_URL, "http://127.0.0.1:1/");
        if(curl_multi_add_handle(m, e[i]) == CURLM_OK)
          acc++;
      }
    }
    for(i = 0; i < FANOUT; i++) {
      if(e[i]) {
        curl_multi_remove_handle(m, e[i]);
        curl_easy_cleanup(e[i]);
      }
    }
    curl_multi_cleanup(m);
  }
  return acc;
}

/* One multi handle reused across rounds: isolates add/remove from init. */
struct mctx {
  CURLM *m;
  CURL *e[FANOUT];
};

static int setup_reuse(void **pctx)
{
  struct mctx *c = calloc(1, sizeof(*c));
  int i;
  if(!c)
    return 1;
  c->m = curl_multi_init();
  if(!c->m) {
    free(c);
    return 1;
  }
  for(i = 0; i < FANOUT; i++) {
    c->e[i] = curl_easy_init();
    if(!c->e[i]) {
      while(--i >= 0)
        curl_easy_cleanup(c->e[i]);
      curl_multi_cleanup(c->m);
      free(c);
      return 1;
    }
    curl_easy_setopt(c->e[i], CURLOPT_URL, "http://127.0.0.1:1/");
  }
  *pctx = c;
  return 0;
}

static void teardown_reuse(void *ctx)
{
  struct mctx *c = ctx;
  int i;
  if(!c)
    return;
  for(i = 0; i < FANOUT; i++)
    curl_easy_cleanup(c->e[i]);
  curl_multi_cleanup(c->m);
  free(c);
}

static uint64_t run_add_remove(void *ctx, uint64_t iters)
{
  struct mctx *c = ctx;
  uint64_t acc = 0;
  while(iters--) {
    int i;
    for(i = 0; i < FANOUT; i++)
      acc += (curl_multi_add_handle(c->m, c->e[i]) == CURLM_OK);
    for(i = 0; i < FANOUT; i++)
      curl_multi_remove_handle(c->m, c->e[i]);
  }
  return acc;
}

static int verify_multi(void *ctx)
{
  CURLM *m;
  CURL *e[FANOUT];
  int i, rc = 0;
  (void)ctx;

  m = curl_multi_init();
  if(!m)
    return 1;
  /* grow past the initial table size to make sure on-demand growth works */
  for(i = 0; i < FANOUT; i++) {
    e[i] = curl_easy_init();
    if(!e[i] || curl_easy_setopt(e[i], CURLOPT_URL, "http://127.0.0.1:1/") ||
       curl_multi_add_handle(m, e[i]) != CURLM_OK)
      rc = 1;
  }
  for(i = 0; i < FANOUT; i++) {
    if(e[i] && curl_multi_remove_handle(m, e[i]) != CURLM_OK)
      rc = 1;
    curl_easy_cleanup(e[i]);
  }
  if(curl_multi_cleanup(m) != CURLM_OK)
    rc = 1;
  return rc;
}

const struct bench cb_cases_multi[] = {
  { "multi/lifecycle", "multi-table-seed",
    "curl_multi_init + curl_multi_cleanup",
    NULL, NULL, run_multi_lifecycle, verify_multi },
  { "multi/easy_lifecycle", "-",
    "curl_easy_init + curl_easy_cleanup",
    NULL, NULL, run_easy_lifecycle, verify_multi },
  { "multi/fanout_setup", "multi-table-seed",
    "multi + 8 easy handles created, added, removed, destroyed",
    NULL, NULL, run_multi_fanout_setup, verify_multi },
  { "multi/add_remove", "-",
    "8 add_handle + 8 remove_handle on a reused multi handle",
    setup_reuse, teardown_reuse, run_add_remove, verify_multi },
  { NULL, NULL, NULL, NULL, NULL, NULL, NULL }
};
