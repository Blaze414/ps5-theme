# Host (Linux) builds of the plugin for the end-to-end tests.
#   asan:  AddressSanitizer + UBSan, for the functional and robustness tests
#   crash: plain build with a debug crash route, for the crash-handler test

SRCS := src/main.c src/web.c src/tile.c src/log.c src/fsguard.c src/verify.c \
        src/crash.c src/policy.c src/atrac9.c src/texture.c src/zstd.c src/update.c src/rco.c src/font.c src/png.c src/inflate.c src/running.c src/autostyle.c tests/host/platform_linux.c
GZ   := build-meta/index.html.gz build-meta/atrac9.js.gz
DEPS := $(SRCS) src/theme.h src/platform.h src/atrac9_tables.h web/index.html web/atrac9.js assets/icon0.png $(GZ)

CFLAGS := -std=gnu11 -D_GNU_SOURCE -DTEST_HOOKS -Isrc -g -O1 -fno-omit-frame-pointer \
          -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -Werror

all: build-host/ps5-theme-asan build-host/ps5-theme-crash build-host/ps5-theme-stop

# As the plugin's own Makefile: the page and the encoder compressed (web.c embeds them).
build-meta/%.gz: web/%
	mkdir -p $(dir $@)
	gzip -9 -n -c $< > $@

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

build-host/ps5-theme-stop: tools/stop/stop.c src/running.c tests/host/platform_linux.c src/theme.h src/platform.h
	mkdir -p build-host
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=all \
	      -o $@ tools/stop/stop.c src/running.c tests/host/platform_linux.c -lpthread

.PHONY: all
