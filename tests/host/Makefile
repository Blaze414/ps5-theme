# Host (Linux) builds of the plugin for the end-to-end tests.
#   asan:  AddressSanitizer + UBSan, for the functional and robustness tests
#   crash: plain build with a debug crash route, for the crash-handler test

SRCS := src/main.c src/web.c src/tile.c src/log.c src/fsguard.c src/verify.c \
        src/crash.c src/policy.c src/atrac9.c src/texture.c src/zstd.c tests/host/platform_linux.c
DEPS := $(SRCS) src/theme.h src/platform.h src/atrac9_tables.h web/index.html web/atrac9.js assets/icon0.png

CFLAGS := -std=gnu11 -D_GNU_SOURCE -Isrc -g -O1 -fno-omit-frame-pointer \
          -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -Werror

all: build-host/ps5-theme-asan build-host/ps5-theme-crash

build-host/ps5-theme-asan: $(DEPS)
	mkdir -p build-host
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=all \
	      -o $@ $(SRCS) -lpthread -lm

build-host/ps5-theme-crash: $(DEPS)
	mkdir -p build-host
	$(CC) $(CFLAGS) -DHOST_TEST -o $@ $(SRCS) -lpthread -lm

.PHONY: all
