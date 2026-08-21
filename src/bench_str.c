/* curlbench: case-insensitive comparison cases.
 *
 * Targets the raw case-conversion cost paid by curl_strequal()/
 * curl_strnequal(), which libcurl uses for header, scheme and host matching.
 *
 * SPDX-License-Identifier: curl
 */
#define _POSIX_C_SOURCE 200809L

#include <string.h>

#include <curl/curl.h>

#include "curlbench.h"

/* A realistic response header set: what libcurl actually walks when it
   decides whether an incoming header is one it cares about. */
static const char *const seen_headers[] = {
  "Content-Type", "Content-Length", "Transfer-Encoding", "Connection",
  "Date", "Server", "ETag", "Cache-Control", "Last-Modified", "Location",
  "Set-Cookie", "Vary", "Accept-Ranges", "Content-Encoding", "Age",
  "X-Powered-By", "Strict-Transport-Security", "Content-Range",
  "WWW-Authenticate", "Retry-After", "Content-Disposition", "Link",
  "icy-name", "icy-genre", "icy-br", "icy-metaint",
  NULL
};

/* The names libcurl compares those against. Same-case on purpose: the
   overwhelmingly common outcome is a byte-identical match or an early
   mismatch, which is the case the conversion cost dominates. */
static const char *const wanted_headers[] = {
  "Content-Length", "Transfer-Encoding", "Connection", "Location",
  "Content-Encoding", "Content-Range", "Retry-After", "icy-metaint",
  NULL
};

static uint64_t run_equal_hit(void *ctx, uint64_t iters)
{
  uint64_t acc = 0;
  (void)ctx;
  while(iters--)
    acc += (uint64_t)curl_strequal("Transfer-Encoding", "Transfer-Encoding");
  return acc;
}

static uint64_t run_equal_folded(void *ctx, uint64_t iters)
{
  uint64_t acc = 0;
  (void)ctx;
  while(iters--)
    acc += (uint64_t)curl_strequal("transfer-encoding", "Transfer-Encoding");
  return acc;
}

static uint64_t run_equal_miss_late(void *ctx, uint64_t iters)
{
  uint64_t acc = 0;
  (void)ctx;
  /* differs only in the final byte: the whole string is converted */
  while(iters--)
    acc += (uint64_t)curl_strequal("Content-Encoding", "Content-EncodinG!");
  return acc;
}

static uint64_t run_equal_miss_early(void *ctx, uint64_t iters)
{
  uint64_t acc = 0;
  (void)ctx;
  while(iters--)
    acc += (uint64_t)curl_strequal("Xontent-Length", "Content-Length");
  return acc;
}

static uint64_t run_nequal_prefix(void *ctx, uint64_t iters)
{
  uint64_t acc = 0;
  (void)ctx;
  while(iters--)
    acc += (uint64_t)curl_strnequal("Transfer-Encoding: chunked",
                                    "Transfer-Encoding", 17);
  return acc;
}

/* The integrated shape: every seen header against every wanted header, which
   is what header dispatch costs per response. */
static uint64_t run_header_scan(void *ctx, uint64_t iters)
{
  uint64_t acc = 0;
  (void)ctx;
  while(iters--) {
    const char *const *s;
    for(s = seen_headers; *s; s++) {
      const char *const *w;
      for(w = wanted_headers; *w; w++)
        acc += (uint64_t)curl_strequal(*s, *w);
    }
  }
  return acc;
}

static int verify_str(void *ctx)
{
  (void)ctx;
  if(!curl_strequal("Transfer-Encoding", "transfer-ENCODING"))
    return 1;
  if(curl_strequal("Content-Length", "Content-Lengthx"))
    return 1;
  if(curl_strequal("Content-Length", "Content-Lengt"))
    return 1;
  if(!curl_strnequal("Transfer-Encoding: chunked", "transfer-encoding", 17))
    return 1;
  if(curl_strnequal("abc", "abd", 3))
    return 1;
  if(!curl_strnequal("abc", "abd", 2))
    return 1;
  /* bytes >= 0x80 must not fold: the conversion is plain ASCII only */
  if(curl_strequal("\xc3\xa9", "\xc3\x89"))
    return 1;
  return 0;
}

const struct bench cb_cases_str[] = {
  { "str/equal_hit", "strcase-inline",
    "curl_strequal on byte-identical header names",
    NULL, NULL, run_equal_hit, verify_str },
  { "str/equal_folded", "strcase-inline",
    "curl_strequal where every byte needs case folding",
    NULL, NULL, run_equal_folded, verify_str },
  { "str/equal_miss_late", "strcase-inline",
    "curl_strequal differing only in the last byte",
    NULL, NULL, run_equal_miss_late, verify_str },
  { "str/equal_miss_early", "strcase-inline",
    "curl_strequal differing in the first byte",
    NULL, NULL, run_equal_miss_early, verify_str },
  { "str/nequal_prefix", "strcase-inline",
    "curl_strnequal prefix match against a full header line",
    NULL, NULL, run_nequal_prefix, verify_str },
  { "str/header_scan", "strcase-inline",
    "26 response headers x 8 wanted names, one response worth of dispatch",
    NULL, NULL, run_header_scan, verify_str },
  { NULL, NULL, NULL, NULL, NULL, NULL, NULL }
};
