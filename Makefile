PS5_HOST ?= ps5
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

TITLE_ID := THEM00001
VERSION  := 1.56

ELF    := ps5-theme.elf
PLUGIN := ps5-theme.plugin
STOP   := ps5-theme-stop.elf

# Portable sources are shared with the host test rig (tests/host).
SRCS   := src/main.c src/web.c src/tile.c src/log.c src/fsguard.c src/verify.c \
          src/crash.c src/policy.c src/atrac9.c src/texture.c src/zstd.c src/update.c src/rco.c src/font.c src/png.c src/inflate.c src/running.c src/platform_ps5.c
HDRS   := src/theme.h src/platform.h src/atrac9_tables.h
ASSETS := web/index.html web/atrac9.js assets/icon0.png

# Frame pointers keep crash-report backtraces usable.
CFLAGS := -Wall -Wextra -Wno-unused-parameter -Werror -O2 -fno-omit-frame-pointer -Isrc
# The same system libraries etaHEN's own spawned services link (see platform_ps5.c).
LDADD  := -Wl,--no-as-needed -lSceSystemService -lSceUserService -lSceSysmodule -lSceAppInstUtil

all: $(PLUGIN) $(STOP)

# web.c embeds $(ASSETS) with .incbin, relative to this directory.
$(ELF): $(SRCS) $(HDRS) $(ASSETS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDADD)

# A separate payload that stops the plugin and leaves its theme on (tools/stop).
$(STOP): tools/stop/stop.c src/running.c src/platform_ps5.c $(HDRS)
	$(CC) $(CFLAGS) -o $@ tools/stop/stop.c src/running.c src/platform_ps5.c $(LDADD)

$(PLUGIN): $(ELF) tools/make_plugin.py
	python3 tools/make_plugin.py $(ELF) $@ $(TITLE_ID) $(VERSION)

# One-off run through the ELF loader (no etaHEN plugin management).
test: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $<

clean:
	rm -f $(ELF) $(PLUGIN) $(STOP)

.PHONY: all test clean
