/* curlbench: mprintf cases.
 *
 * libcurl installs its own printf family (curl/mprintf.h) and uses it to
 * assemble request lines, headers and log messages. curl_maprintf() drives
 * the dynbuf-backed sink; curl_msnprintf() drives the fixed-buffer sink.
 *
 * SPDX-License-Identifier: curl
 */
#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>
#include <curl/mprintf.h>

#include "curlbench.h"

/* A SOAP action body of the shape a UPnP control point sends: long enough to
   cross any staging buffer a patch might introduce, all plain %s. */
static const char *const soap_fmt =
  "<?xml version=\"1.0\"?>"
  "<s:Envelope xmlns:s=\"%s\" s:encodingStyle=\"%s\">"
  "<s:Body><u:%s xmlns:u=\"%s\">"
  "<InstanceID>%s</InstanceID><Channel>%s</Channel>"
  "<DesiredVolume>%s</DesiredVolume>"
  "</u:%s></s:Body></s:Envelope>";

/* The arguments printf/soap_body and printf/snprintf_soap format. Shared so
   the two sinks are provably formatting the same thing. */
#define SOAP_ARGS \
  "http://schemas.xmlsoap.org/soap/envelope/", \
  "http://schemas.xmlsoap.org/soap/encoding/", \
  "SetVolume", \
  "urn:schemas-upnp-org:service:RenderingControl:1", \
  "0", "Master", "42", "SetVolume"

/* soap_fmt with SOAP_ARGS renders 344 bytes. Round up, and leave the margin
   large enough that the fixed-buffer case never enters the truncation path:
   verify_printf() checks the rendered length against this. */
#define SOAP_BUFSZ 512

/* The header line printf/header_pair and printf/snprintf_header format. */
#define HEADER_NAME "SOAPACTION"
#define HEADER_VALUE \
  "\"urn:schemas-upnp-org:service:RenderingControl:1#SetVolume\""

static uint64_t run_request_line(void *ctx, uint64_t iters)
{
  uint64_t acc = 0;
  (void)ctx;
  while(iters--) {
    char *s = curl_maprintf("%s %s HTTP/1.1\r\n", "POST",
                            "/MediaRenderer/AVTransport/Control");
    if(s) {
      acc += (uint64_t)(unsigned char)s[0] + strlen(s);
      curl_free(s);
    }
  }
  return acc;
}

static uint64_t run_header_pair(void *ctx, uint64_t iters)
{
  uint64_t acc = 0;
  (void)ctx;
  while(iters--) {
    char *s = curl_maprintf("%s: %s\r\n", HEADER_NAME, HEADER_VALUE);
    if(s) {
      acc += strlen(s);
      curl_free(s);
    }
  }
  return acc;
}

static uint64_t run_numbers(void *ctx, uint64_t iters)
{
  uint64_t acc = 0;
  uint32_t st = 0x12345678u;
  (void)ctx;
  while(iters--) {
    int n = (int)(cb_rand(&st) & 0xffff);
    char *s = curl_maprintf("Content-Length: %d\r\nRange: bytes=%d-%d\r\n",
                            n, n, n + 4095);
    if(s) {
      acc += strlen(s);
      curl_free(s);
    }
  }
  return acc;
}

static uint64_t run_soap_body(void *ctx, uint64_t iters)
{
  uint64_t acc = 0;
  (void)ctx;
  while(iters--) {
    char *s = curl_maprintf(soap_fmt, SOAP_ARGS);
    if(s) {
      acc += strlen(s);
      curl_free(s);
    }
  }
  return acc;
}

static uint64_t run_snprintf_small(void *ctx, uint64_t iters)
{
  uint64_t acc = 0;
  char buf[128];
  (void)ctx;
  while(iters--) {
    int n = curl_msnprintf(buf, sizeof(buf), "%s:%d", "192.168.1.55", 1400);
    acc += (uint64_t)n + (unsigned char)buf[0];
  }
  return acc;
}

/* The format, arguments and buffer size of curl's own tests/perf/snprintf.c,
   copied verbatim so a number quoted from that test is directly comparable to
   one from here instead of having to be argued about. Eight conversions
   carrying width and length modifiers, 98 bytes of output, and the literal
   runs between the conversions are only 4 to 9 bytes -- the least favourable
   printf shape in this file for anything that batches output, which is what
   makes it the useful cross-check on a claim made from the other cases. */
static uint64_t run_snprintf_upstream(void *ctx, uint64_t iters)
{
  uint64_t acc = 0;
  char buf[256];
  (void)ctx;
  while(iters--) {
    int n = curl_msnprintf(buf, sizeof(buf),
                           "Add %-4d stuff %u to %3d the %3u output %s "
                           "%*s"
                           "for %lld testing %lu\n",
                           123, (unsigned int)123,
                           123, (unsigned int)123, "helllo",
                           14, "01234567890123456",
                           (long long)987871231231,
                           (unsigned long)6732673);
    acc += (uint64_t)n + (unsigned char)buf[0];
  }
  return acc;
}

/* printf/header_pair's format and arguments against the fixed-buffer sink
   instead of the dynbuf one. Paired with it, this isolates the sink: same
   parse, same conversions, same bytes out, so the delta between the two cases
   is the cost of the output path and nothing else. */
static uint64_t run_snprintf_header(void *ctx, uint64_t iters)
{
  uint64_t acc = 0;
  char buf[128];
  (void)ctx;
  while(iters--) {
    int n = curl_msnprintf(buf, sizeof(buf), "%s: %s\r\n",
                           HEADER_NAME, HEADER_VALUE);
    acc += (uint64_t)n + (unsigned char)buf[0];
  }
  return acc;
}

/* printf/soap_body against the fixed-buffer sink. Paired with it, this
   isolates dynbuf growth: 344 bytes into a buffer that is already large
   enough does no reallocation, where the dynbuf case starts at
   MIN_FIRST_ALLOC and doubles its way up. */
static uint64_t run_snprintf_soap(void *ctx, uint64_t iters)
{
  uint64_t acc = 0;
  char buf[SOAP_BUFSZ];
  (void)ctx;
  while(iters--) {
    int n = curl_msnprintf(buf, sizeof(buf), soap_fmt, SOAP_ARGS);
    acc += (uint64_t)n + (unsigned char)buf[0];
  }
  return acc;
}

/* Width, precision, hex and pointer conversions in one format: the general
   parser's own workload, kept here so that a change aimed at the common
   shapes has to show it did not slow the uncommon ones down. */
static uint64_t run_complex(void *ctx, uint64_t iters)
{
  uint64_t acc = 0;
  (void)ctx;
  while(iters--) {
    char *s = curl_maprintf("%-20s|%08lx|%.3s|%5d|%%|%p",
                            "conn", (unsigned long)0xdeadbeefUL,
                            "truncate-me", 42, (void *)&acc);
    if(s) {
      acc += strlen(s);
      curl_free(s);
    }
  }
  return acc;
}

static int expect(const char *got, const char *want)
{
  return !got || strcmp(got, want);
}

static int verify_printf(void *ctx)
{
  char buf[64];
  char big[SOAP_BUFSZ];
  char *s;
  int n;
  int rc = 0;
  (void)ctx;

  s = curl_maprintf("%s %s HTTP/1.1\r\n", "POST", "/ctl");
  rc |= expect(s, "POST /ctl HTTP/1.1\r\n");
  curl_free(s);

  s = curl_maprintf("Content-Length: %d\r\nRange: bytes=%d-%d\r\n",
                    7, 7, 4102);
  rc |= expect(s, "Content-Length: 7\r\nRange: bytes=7-4102\r\n");
  curl_free(s);

  s = curl_maprintf("%d %u %d", -1, 4294967295u, 0);
  rc |= expect(s, "-1 4294967295 0");
  curl_free(s);

  /* through a volatile pointer so the compiler does not diagnose the NULL
     argument it is the whole point of this check to pass */
  {
    static const char *volatile nullstr = NULL;
    s = curl_maprintf("[%s]", nullstr);
  }
  /* libcurl renders a NULL %s as "(nil)" */
  rc |= expect(s, "[(nil)]");
  curl_free(s);

  s = curl_maprintf("%c%c%%", 'o', 'k');
  rc |= expect(s, "ok%");
  curl_free(s);

  s = curl_maprintf("%-6s|%.3s|%5d|%08lx", "ab", "abcdef", 42,
                    (unsigned long)0xbeefUL);
  rc |= expect(s, "ab    |abc|   42|0000beef");
  curl_free(s);

  if(curl_msnprintf(buf, sizeof(buf), "%s:%d", "host", 80) != 7)
    rc |= 1;
  rc |= expect(buf, "host:80");

  /* truncation must still NUL-terminate */
  if(curl_msnprintf(buf, 4, "%s", "abcdefgh") < 0)
    rc |= 1;
  rc |= expect(buf, "abc");

  /* a long all-%s format, which is where a staging buffer would show */
  s = curl_maprintf(soap_fmt, "a", "b", "SetVolume", "c", "0", "Master",
                    "42", "SetVolume");
  if(!s || !strstr(s, "<DesiredVolume>42</DesiredVolume>") ||
     !strstr(s, "</u:SetVolume>"))
    rc |= 1;
  curl_free(s);

  /* the format printf/snprintf_upstream measures, spelled out: a change that
     mishandles %*s or %lld shows up here rather than as a mystery in the
     numbers */
  if(curl_msnprintf(big, sizeof(big),
                    "Add %-4d stuff %u to %3d the %3u output %s "
                    "%*s"
                    "for %lld testing %lu\n",
                    123, (unsigned int)123,
                    123, (unsigned int)123, "helllo",
                    14, "01234567890123456",
                    (long long)987871231231,
                    (unsigned long)6732673) != 98)
    rc |= 1;
  rc |= expect(big, "Add 123  stuff 123 to 123 the 123 output helllo "
                    "01234567890123456for 987871231231 testing 6732673\n");

  /* the two sink-pair cases must format byte-identical output, or the delta
     between each pair measures a difference in the work and not in the sink */
  s = curl_maprintf("%s: %s\r\n", HEADER_NAME, HEADER_VALUE);
  if(curl_msnprintf(big, sizeof(big), "%s: %s\r\n",
                    HEADER_NAME, HEADER_VALUE) < 0)
    rc |= 1;
  rc |= expect(s, big);
  curl_free(s);

  s = curl_maprintf(soap_fmt, SOAP_ARGS);
  n = curl_msnprintf(big, sizeof(big), soap_fmt, SOAP_ARGS);
  rc |= expect(s, big);
  curl_free(s);

  /* printf/snprintf_soap must not be measuring the truncation path */
  if(n < 0 || (size_t)n >= SOAP_BUFSZ)
    rc |= 1;

  return rc;
}

const struct bench cb_cases_printf[] = {
  { "printf/request_line", "mprintf-runs",
    "curl_maprintf of a request line (two %s, short output)",
    NULL, NULL, run_request_line, verify_printf },
  { "printf/header_pair", "mprintf-runs",
    "curl_maprintf of one header line",
    NULL, NULL, run_header_pair, verify_printf },
  { "printf/numbers", "mprintf-runs",
    "curl_maprintf with integer conversions",
    NULL, NULL, run_numbers, verify_printf },
  { "printf/soap_body", "mprintf-runs",
    "curl_maprintf of a ~300 byte all-%s SOAP body",
    NULL, NULL, run_soap_body, verify_printf },
  { "printf/snprintf_small", "mprintf-runs",
    "curl_msnprintf into a stack buffer",
    NULL, NULL, run_snprintf_small, verify_printf },
  { "printf/snprintf_upstream", "mprintf-runs",
    "curl's own tests/perf/snprintf.c format, verbatim",
    NULL, NULL, run_snprintf_upstream, verify_printf },
  { "printf/snprintf_header", "mprintf-runs",
    "printf/header_pair into a stack buffer: pairs with it to isolate the sink",
    NULL, NULL, run_snprintf_header, verify_printf },
  { "printf/snprintf_soap", "mprintf-runs",
    "printf/soap_body into a stack buffer: pairs with it to isolate realloc",
    NULL, NULL, run_snprintf_soap, verify_printf },
  { "printf/complex", "mprintf-runs",
    "width/precision/hex/pointer format: must not regress",
    NULL, NULL, run_complex, verify_printf },
  { NULL, NULL, NULL, NULL, NULL, NULL, NULL }
};
