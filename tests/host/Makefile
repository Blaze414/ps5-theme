# Host (Linux) builds of the plugin for the end-to-end tests.
#   asan:  AddressSanitizer + UBSan, for the functional and robustness tests
#   crash: plain build with a debug crash route, for the crash-handler test

.DEFAULT_GOAL := all

# The plugin's sources and page, as the PS5 build has them (sources.mk), on Linux's platform layer.
include sources.mk
SRCS := $(CORE_SRCS) tests/host/platform_linux.c
DEPS := $(SRCS) $(CORE_HDRS) $(WEB_EMBED)

CFLAGS := -std=gnu11 -D_GNU_SOURCE -DTEST_HOOKS -Isrc -g -O1 -fno-omit-frame-pointer \
          -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -Werror

all: build-host/ps5-theme-asan build-host/ps5-theme-crash build-host/ps5-theme-stop

build-host/ps5-theme-asan: $(DEPS)
	mkdir -p build-host
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=all \
	      -o $@ $(SRCS) -lpthread -lm

# ThreadSanitizer, for tests/host/run.sh tsan: the same suite, stopping at the first data race between threads.
build-host/ps5-theme-tsan: $(DEPS)
	mkdir -p build-host
	$(CC) $(CFLAGS) -fsanitize=thread -o $@ $(SRCS) -lpthread -lm

build-host/ps5-theme-crash: $(DEPS)
	mkdir -p build-host
	$(CC) $(CFLAGS) -DHOST_TEST -o $@ $(SRCS) -lpthread -lm

build-host/ps5-theme-stop: tools/stop/stop.c src/running.c tests/host/platform_linux.c $(CORE_HDRS)
	mkdir -p build-host
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=all \
	      -o $@ tools/stop/stop.c src/running.c tests/host/platform_linux.c -lpthread

.PHONY: all
