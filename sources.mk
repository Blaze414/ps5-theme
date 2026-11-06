# What the plugin is built from, shared by the PS5 build (Makefile) and the host test rig (tests/host/Makefile).
# A new source file is added here once.

# The plugin's C, all but the platform layer (src/platform_ps5.c on the console, tests/host/platform_linux.c on
# the rig).
CORE_SRCS := src/main.c src/status.c src/theme.c src/apply.c src/watch.c src/diag.c src/update.c src/running.c \
             src/mounts.c src/shellui.c src/autostyle.c src/tile.c src/log.c src/crash.c src/fsguard.c src/prefs.c \
             src/policy.c src/verify.c src/svg.c src/png.c src/inflate.c src/rco.c src/font.c src/atrac9.c \
             src/texture.c src/zstd.c src/sha256.c \
             src/http.c src/routes.c src/access.c src/store.c src/upload.c src/api_themes.c src/api_files.c \
             src/api_parts.c src/api_style.c src/api_mix.c src/api_console.c src/api_diag.c src/api_system.c
CORE_HDRS := $(wildcard src/*.h)

# The manager's page: web/index.html lists the style sheets and scripts, tools/bundle_page.py joins them.
PAGE_SRCS := web/index.html $(wildcard web/css/*.css web/js/*.js web/js/*/*.js)

# What the plugin embeds (src/http.c): the joined page, and the page and the ATRAC9 encoder compressed (gzip -9 -n,
# no name or time, so the same input gives the same bytes) for browsers that take gzip.
WEB_BUILT := build-meta/index.html build-meta/index.html.gz build-meta/atrac9.js.gz
WEB_EMBED := $(WEB_BUILT) web/atrac9.js assets/icon0.png

build-meta/index.html: $(PAGE_SRCS) tools/bundle_page.py
	mkdir -p $(dir $@)
	python3 tools/bundle_page.py web/index.html $@

build-meta/index.html.gz: build-meta/index.html
	gzip -9 -n -c $< > $@

build-meta/atrac9.js.gz: web/atrac9.js
	mkdir -p $(dir $@)
	gzip -9 -n -c $< > $@
