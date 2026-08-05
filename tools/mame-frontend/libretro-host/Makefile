# MAMESTer libretro host -- links a libretro MAME engine into a plain ARM
# binary. No dlopen, no RetroArch.
#
#   make CROSS=1                 mame2003-plus  -> ./mame2003
#   make CROSS=1 ENGINE=lrmame   MAME 0.289     -> ./lrmame
#   make CROSS=1 clean
#
# The target is always armhf/Cortex-A9. When arm-linux-gnueabihf-gcc is not on
# PATH -- i.e. on the macOS or x86_64 build host -- this Makefile re-executes
# itself inside the same cross container tools/build-m2003p.sh uses, so the two
# halves of the binary are emitted by one compiler. CROSS=1 is accepted for
# symmetry with tools/build-mame.sh; there is no non-cross mode, because the
# archive it links is ARM.
#
# Flags mirror the archive's (tools/build-m2003p.sh) except for -O: the archive
# defaults to -O3 there. This is a few hundred lines of glue plus nv_present.c,
# so its own -O level is not measurable either way; -O2 keeps compile time down
# on the file that gets edited most.

REPO    := $(abspath $(CURDIR)/../../..)
BACKEND := $(REPO)/tools/mame-frontend/mister-backend

# --- engine selection ------------------------------------------------------
#
#   make CROSS=1                 mame2003-plus (default)
#   make CROSS=1 ENGINE=lrmame   libretro's current MAME (0.289)
#
# The two engines bind differently, and the difference is not cosmetic:
#
#   m2003p  is a STATIC ARCHIVE (tools/build-m2003p.sh, `ar rcs`). Upstream
#           omits libretro-common from a STATIC_LINKING build on the assumption
#           that the frontend already carries it -- so this Makefile compiles
#           those 22 files itself. See the LRC block below.
#
#   lrmame  is a SHARED OBJECT. MAME's genie target links it with
#           `-shared -Wl,--version-script=link.T -Wl,--no-undefined`
#           (scripts/src/osd/retro.lua:83), and link.T exports `retro_*` and
#           hides everything else -- which is exactly the frontend contract. So
#           it links directly, with NO dlopen and NO host source change, and
#           --no-undefined means it drags no libretro-common obligation along.
#           It does need an rpath, because the .so deploys beside the binary
#           rather than into a system library directory.
ENGINE ?= m2003p

ifeq ($(ENGINE),lrmame)
CORE       := $(REPO)/vendor/lrmame
BIN        ?= lrmame
# genie names the artefact `<SUBTARGET>_libretro.so` and drops it in the SOURCE
# ROOT, not under libretro/bin/ (that is only a -L search path). Read out of the
# generated build/projects/retro/mame<SUBTARGET>/gmake-linux/<SUBTARGET>.make,
# `config=libretro` block, rather than guessed -- the same file also defines a
# `libretro32` config that builds `-m32` and would be the one selected if
# ARCHITECTURE= were ever dropped from tools/build-lrmame.sh.
LRMAME_SUBTARGET ?= lrmame
ENGINE_LIB ?= $(CORE)/$(LRMAME_SUBTARGET)_libretro.so
# MAME vendors its own libretro.h under the OSD rather than in a
# libretro-common tree, and it is a NEWER vintage than 2003-plus's. host_env.c
# is #ifdef-guarded per command so that one source compiles against either.
ENGINE_CPPFLAGS := -I$(CORE)/src/osd/libretro/libretro-internal \
                   -DMAMESTER_ENGINE_LRMAME
# $ORIGIN, not the build directory: the binary and the .so are deployed
# together into games/mame/ and the runtime path has to be relative to the
# binary. The $$ survives make's expansion so the linker sees a literal
# $ORIGIN, which is resolved at load time and not by the shell.
ENGINE_LDFLAGS  := -Wl,-rpath,'$$ORIGIN' -Wl,-rpath-link,$(dir $(ENGINE_LIB))
ENGINE_HINT     := run tools/build-lrmame.sh
else
CORE       := $(REPO)/vendor/mame2003-plus
BIN        ?= mame2003
ENGINE_LIB ?= $(CORE)/mame2003_plus_libretro.a
ENGINE_CPPFLAGS := -I$(CORE)/src/libretro-common/include
ENGINE_LDFLAGS  :=
ENGINE_HINT     := run tools/build-m2003p.sh
endif

# The cross container differs per engine too: MAME 0.289 is -std=c++20 and the
# bullseye/gcc-10.2.1 image cannot build it. Only the ENGINE ARCHIVE is built in
# these images -- this host is C99 either way -- but they must match so the two
# halves of the binary come from one toolchain.
ifeq ($(ENGINE),lrmame)
IMAGE      := mamester-cross-armhf-cxx20
DOCKERFILE := $(REPO)/tools/mister/Dockerfile.cross-armhf-cxx20
else
IMAGE      := mamester-cross-armhf
DOCKERFILE := $(REPO)/tools/mister/Dockerfile.cross-armhf
endif

# Kept as an alias so existing invocations and docs that say ARCHIVE=... still
# work; ENGINE_LIB is the name that makes sense once one engine is a .so.
ARCHIVE ?= $(ENGINE_LIB)

CROSS_PREFIX ?= arm-linux-gnueabihf-
CC           := $(CROSS_PREFIX)gcc

# Re-exec in the container unless the cross toolchain is already reachable.
HAVE_CROSS := $(shell command -v $(CC) >/dev/null 2>&1 && echo yes)

ifneq ($(HAVE_CROSS),yes)

DOCKER := docker run --rm -v "$(REPO):/repo" \
                     -w /repo/tools/mame-frontend/libretro-host $(IMAGE)

# `make`, not $(MAKE): $(MAKE) is the host's absolute path (on macOS,
# /Library/Developer/CommandLineTools/usr/bin/make), which does not exist in the
# container.
#
# $(MAKEOVERRIDES) forwards the command-line assignments -- without it,
# `make BIN=mame2003-o2 ARCHIVE=...` silently rebuilds the DEFAULT target inside
# the container and reports success. Note that an ARCHIVE passed this way must
# be a CONTAINER path (/repo/vendor/...), because that is where the bind mount
# puts the repo.
.PHONY: all clean image
all clean: image
	$(DOCKER) make $@ $(MAKEOVERRIDES)

image:
	@docker image inspect $(IMAGE) >/dev/null 2>&1 || \
	  docker build -t $(IMAGE) -f $(DOCKERFILE) $(REPO)/tools/mister

else

CFLAGS ?= -O2 -g -std=gnu99 -Wall -Wextra \
          -marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard \
          -fomit-frame-pointer -fno-math-errno

CPPFLAGS := $(ENGINE_CPPFLAGS) -I$(BACKEND)

# zlib is inside the archive (Makefile.common: src/lib/zlib/*). libasound comes
# from libasound2-dev:armhf in Dockerfile.cross-armhf and is present on the
# MiSTer rootfs, so it stays a shared dependency like mame4all's.
#
# -lpthread is for host_present.c's worker. glibc only folded pthread into libc
# in 2.34; this toolchain is 2.31, so the explicit link is required.
LDLIBS := -lm -lasound -lpthread

# --- libretro-common, which the ARCHIVE deliberately does not contain -------
#
# m2003p ONLY. MAME's .so is linked with -Wl,--no-undefined, so it resolves
# every symbol it needs internally and this whole block is empty for lrmame --
# compiling 2003-plus's copy into an lrmame binary would put a second, older
# libretro-common in the link for nothing to call.
#
# Makefile.common:2692 adds these 22 files to SOURCES_C only when
# STATIC_LINKING is NOT 1: in a static build upstream expects the FRONTEND to
# already have them, because RetroArch does. This host is the frontend, so
# supplying them here is the intended contract rather than a workaround. Leave
# them out and the link fails on filestream_*, intfstream_*, path_* and MD5_*.
#
# The list is copied verbatim from that block. Their flags matter: file_stream
# and vfs_implementation carry off_t across the boundary to core code compiled
# with -D_FILE_OFFSET_BITS=64 (Makefile:823), and rzip/trans_stream select their
# implementation on HAVE_ZLIB (Makefile:831). Compiling them without those is a
# silent ABI mismatch, not a compile error.
ifeq ($(ENGINE),lrmame)
LRC_SRC :=
else
LRC     := $(CORE)/src/libretro-common
LRC_SRC := \
	$(LRC)/compat/compat_posix_string.c \
	$(LRC)/compat/compat_snprintf.c \
	$(LRC)/compat/compat_strcasestr.c \
	$(LRC)/compat/compat_strl.c \
	$(LRC)/compat/fopen_utf8.c \
	$(LRC)/encodings/encoding_crc32.c \
	$(LRC)/encodings/encoding_utf.c \
	$(LRC)/file/file_path.c \
	$(LRC)/file/file_path_io.c \
	$(LRC)/streams/file_stream.c \
	$(LRC)/streams/file_stream_transforms.c \
	$(LRC)/streams/interface_stream.c \
	$(LRC)/streams/memory_stream.c \
	$(LRC)/streams/rzip_stream.c \
	$(LRC)/streams/stdin_stream.c \
	$(LRC)/streams/trans_stream.c \
	$(LRC)/streams/trans_stream_pipe.c \
	$(LRC)/streams/trans_stream_zlib.c \
	$(LRC)/string/stdstring.c \
	$(LRC)/utils/md5.c \
	$(LRC)/vfs/vfs_implementation.c \
	$(LRC)/time/rtime.c
endif

LRC_CPPFLAGS := -I$(LRC)/include -I$(CORE)/src/lib/zlib \
                -D__LIBRETRO__ -DHAVE_ZLIB -DHAVE_INTTYPES_H \
                -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64
# Upstream compiles these with -fsigned-char too (Makefile:815); MAME code
# assumes it and these files share headers with it.
LRC_CFLAGS   := $(CFLAGS) -fsigned-char -Wno-unused-parameter

LRC_OBJ := $(addprefix lrc/,$(notdir $(LRC_SRC:.c=.o)))

OBJS := host_main.o host_env.o host_video.o host_present.o host_input.o \
        host_audio.o host_throttle.o nv_present.o

.PHONY: all clean
all: $(BIN)

$(BIN): $(OBJS) $(LRC_OBJ) $(ARCHIVE)
	$(CC) $(CFLAGS) $(ENGINE_LDFLAGS) -o $@ $(OBJS) $(LRC_OBJ) $(ARCHIVE) $(LDLIBS)
	@echo "# $@: $$($(CROSS_PREFIX)size -d $@ | tail -1)"

%.o: %.c host.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

# The present path is shared with mame4all rather than copied; see nv_present.h.
nv_present.o: $(BACKEND)/nv_present.c $(BACKEND)/nv_present.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

# One rule per source directory, because the file names are unique but the
# directories are not on the include path.
lrc/%.o:
	@mkdir -p lrc
	$(CC) $(LRC_CFLAGS) $(LRC_CPPFLAGS) -c -o $@ \
	    $(filter %/$(notdir $(basename $@)).c,$(LRC_SRC))

$(LRC_OBJ): $(LRC_SRC)

$(ARCHIVE):
	@echo "missing $(ARCHIVE) -- $(ENGINE_HINT)" >&2; exit 1

clean:
	rm -rf $(BIN) $(OBJS) lrc

endif
