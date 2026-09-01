# curlbench -- build against a specific libcurl install prefix.
#
#   make CURL_PREFIX=/path/to/install            native
#   make CURL_PREFIX=... CC=aarch64-...-gcc      cross
#   make CURL_PREFIX=... STATIC=0                dynamic link (host debugging)
#
# The result is a single executable with libcurl linked in statically. With
# STATIC=1 (the default) libc is static too, so the binary can be copied to a
# device under test with no runtime dependencies at all.

CURL_PREFIX ?= /usr/local
CURL_CONFIG ?= $(CURL_PREFIX)/bin/curl-config
CC          ?= cc
STATIC      ?= 1
O           ?= build

# -fno-builtin keeps the compiler from folding a workload away at compile time
# (e.g. turning a constant-argument curl_strequal into a literal); libcurl
# itself is compiled separately and is unaffected by this.
CFLAGS  ?= -O2 -g -std=c99 -Wall -Wextra -Wno-unused-parameter
CPPFLAGS += -I$(CURL_PREFIX)/include -DCURL_STATICLIB
LDFLAGS  +=

CURL_STATIC_LIBS := $(shell $(CURL_CONFIG) --static-libs 2>/dev/null)
ifeq ($(strip $(CURL_STATIC_LIBS)),)
CURL_STATIC_LIBS := $(CURL_PREFIX)/lib/libcurl.a
endif
LIBS := $(CURL_STATIC_LIBS)

ifeq ($(STATIC),1)
LDFLAGS += -static
endif

SRCS := src/curlbench.c src/bench_str.c src/bench_printf.c \
        src/bench_multi.c
OBJS := $(patsubst src/%.c,$(O)/%.o,$(SRCS))
BIN  := $(O)/curlbench

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

$(O)/%.o: src/%.c src/curlbench.h | $(O)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(O):
	mkdir -p $(O)

# Report what we linked against, which is the first thing to check when a
# number looks wrong.
info: $(BIN)
	@echo "prefix: $(CURL_PREFIX)"
	@echo "libs:   $(LIBS)"
	@echo "static: $(STATIC)"
	@file $(BIN) 2>/dev/null || true
	@$(BIN) --version 2>/dev/null || true

clean:
	rm -rf $(O)

.PHONY: all clean info
