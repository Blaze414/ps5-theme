PS5_HOST ?= ps5
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

.DEFAULT_GOAL := all

TITLE_ID := THEM00001
VERSION  := 1.91

ELF    := ps5-theme.elf
PLUGIN := ps5-theme.plugin
STOP   := ps5-theme-stop.elf

# The sources and the page, shared with the host test rig (tests/host).
include sources.mk
SRCS   := $(CORE_SRCS) src/platform_ps5.c src/meta.c
# The description the ELF carries (src/meta.c), made from this file's version.
META   := build-meta/ps5meta.json

# Frame pointers keep crash-report backtraces usable.
CFLAGS := -Wall -Wextra -Wno-unused-parameter -Werror -O2 -fno-omit-frame-pointer -Isrc
# The same system libraries etaHEN's own spawned services link (see platform_ps5.c).
LDADD  := -Wl,--no-as-needed -lSceSystemService -lSceUserService -lSceSysmodule -lSceAppInstUtil -lSceNotification

all: $(PLUGIN) $(STOP)

# http.c embeds $(WEB_EMBED) with .incbin, relative to this directory; meta.c embeds $(META) and the logo.
$(ELF): $(SRCS) $(CORE_HDRS) $(WEB_EMBED) $(META)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDADD)
	python3 tools/payload_meta.py check $@ $(TITLE_ID) $(VERSION)
	python3 tools/check_syscalls.py $@

$(META): Makefile tools/payload_meta.py
	mkdir -p $(dir $@)
	python3 tools/payload_meta.py json $(TITLE_ID) $(VERSION) > $@

# A separate payload that stops the plugin and leaves its theme on (tools/stop).
$(STOP): tools/stop/stop.c src/running.c src/platform_ps5.c $(CORE_HDRS)
	$(CC) $(CFLAGS) -o $@ tools/stop/stop.c src/running.c src/platform_ps5.c $(LDADD)
	python3 tools/check_syscalls.py $@

$(PLUGIN): $(ELF) tools/make_plugin.py
	python3 tools/make_plugin.py $(ELF) $@ $(TITLE_ID) $(VERSION)

# One-off run through the ELF loader (no etaHEN plugin management).
test: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $<

clean:
	rm -rf $(ELF) $(PLUGIN) $(STOP) build-meta

.PHONY: all test clean
