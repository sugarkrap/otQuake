# Makefile for handheldquake — ARMv5 Zaurus targets
#
# Toolchain: ARM EABI uClibc-ng, built by the dosbox-armv5-zaurus Buildroot.
# Run the dosbox Buildroot first (scripts/02-build.sh in that repo) to
# produce the toolchain at:
#   ../dosbox-armv5-zaurus/buildroot/output/host/bin/
#
# Binaries are built as ARM EABI soft-float and linked statically for
# portability across minimal rootfs images.
#
# Usage:
#   make              → cross-compile for SL-C860
#   make strip        → strip the binary in-place
#   make clean        → remove obj/ and binary
#   make extract      → (re)extract CVS source into src/
#   make check-toolchain → verify the toolchain is present

# ── Toolchain ────────────────────────────────────────────────────────────────

# Buildroot EABI uClibc-ng toolchain produced by dosbox-armv5-zaurus.
DOSBOX_BR := $(abspath ../dosbox-armv5-zaurus/buildroot/output/host/bin)
CROSS      ?= arm-buildroot-linux-uclibcgnueabi
CROSS_PATH := $(DOSBOX_BR)

CC    := $(CROSS_PATH)/$(CROSS)-gcc
STRIP := $(CROSS_PATH)/$(CROSS)-strip

# ── Compiler flags ────────────────────────────────────────────────────────────

# PXA250 / XScale ARMv5TE, no hardware FPU, EABI soft-float.
# ABI selection is controlled by the toolchain; these flags control codegen.
ARCHFLAGS := -march=armv5te -mtune=xscale -mfloat-abi=soft

CFLAGS  := $(ARCHFLAGS) -std=gnu99 -O2 -fcommon -Wall -Wno-unused -Isrc
LDFLAGS := -static -lm

# ── Targets ───────────────────────────────────────────────────────────────────

SRCDIR := src
OBJDIR := obj

# Source list from original Makefile.in, with vid_qt.cpp replaced by vid_fb.c.
SRCS := \
	FixedPointMath.c \
	cd_wav.c      chase.c       cl_demo.c     cl_input.c    cl_main.c  \
	cl_parse.c    cl_tent.c     cmd.c         common.c      console.c  \
	crc.c         cvar.c        d_edge.c      d_fill.c      d_init.c   \
	d_modech.c    d_part.c      d_polyse.c    d_scan.c      d_sky.c    \
	d_sprite.c    d_surf.c      d_vars.c      d_zpoint.c    draw.c     \
	host.c        host_cmd.c    keys.c        mathlib.c     menu.c     \
	model.c       net_bsd.c     net_dgrm.c    net_loop.c    net_main.c \
	net_udp.c     net_vcr.c     nonintel.c    pr_cmds.c     pr_edict.c \
	pr_exec.c     r_aclip.c     r_alias.c     r_bsp.c       r_draw.c   \
	r_edge.c      r_efrag.c     r_light.c     r_main.c      r_misc.c   \
	r_part.c      r_sky.c       r_sprite.c    r_surf.c      r_vars.c   \
	sbar.c        screen.c      snd_dma.c     snd_mem.c     snd_mix.c  \
	snd_sun.c     sv_main.c     sv_move.c     sv_phys.c     sv_user.c  \
	sys_linux.c   vid.c         vid_fb.c      view.c        wad.c      \
	world.c       zone.c

OBJS   := $(patsubst %.c,$(OBJDIR)/%.o,$(SRCS))
TARGET    := quake-fb
LAUNCHER  := quake-fb-launcher

# ── Host test build ──────────────────────────────────────────────────────────
#
# Native build for the dev machine, so changes (e.g. CD music) can be heard
# without cross-compiling and copying to real Zaurus hardware. Reuses
# vid_fb.c as-is -- it silently no-ops if /dev/fb0 isn't accessible, which
# is fine for testing sound -- but swaps snd_sun.c's mmap'd OSS driver for
# a plain ALSA one (snd_alsa.c), since desktop Linux has neither /dev/dsp
# nor OSS mmap support. Not used for the real device.
HOST_CC      := gcc
HOST_CFLAGS  := -std=gnu99 -O0 -g -fcommon -Wall -Wno-unused -Isrc
HOST_LDFLAGS := -lm -lasound
HOST_OBJDIR  := obj-host
HOST_TARGET  := quake-host

HOST_SRCS := $(filter-out snd_sun.c,$(SRCS)) snd_alsa.c
HOST_OBJS := $(patsubst %.c,$(HOST_OBJDIR)/%.o,$(HOST_SRCS))

.PHONY: all clean extract strip check-toolchain host clean-host

all: $(SRCDIR)/vid_fb.c $(TARGET) $(LAUNCHER)

$(TARGET): $(OBJS)
	$(CC) $(ARCHFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Built $@ ($(shell wc -c < $@) bytes, EABI)"

$(LAUNCHER): $(SRCDIR)/quake-fb-launcher.c
	$(CC) $(ARCHFLAGS) -std=gnu99 -O2 -static -o $@ $<
	@echo "Built $@ ($(shell wc -c < $@) bytes, EABI)"

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJDIR):
	mkdir -p $(OBJDIR)

strip: $(TARGET) $(LAUNCHER)
	$(STRIP) $(TARGET) $(LAUNCHER)
	@echo "Stripped $(TARGET) ($(shell wc -c < $(TARGET)) bytes)"
	@echo "Stripped $(LAUNCHER) ($(shell wc -c < $(LAUNCHER)) bytes)"

extract:
	python3 extract-src.py

clean:
	rm -rf $(OBJDIR) $(TARGET) $(LAUNCHER)

host: $(HOST_TARGET)

$(HOST_TARGET): $(HOST_OBJS)
	$(HOST_CC) -o $@ $^ $(HOST_LDFLAGS)
	@echo "Built $@ ($(shell wc -c < $@) bytes, host)"

$(HOST_OBJDIR)/%.o: $(SRCDIR)/%.c | $(HOST_OBJDIR)
	$(HOST_CC) $(HOST_CFLAGS) -c -o $@ $<

$(HOST_OBJDIR):
	mkdir -p $(HOST_OBJDIR)

clean-host:
	rm -rf $(HOST_OBJDIR) $(HOST_TARGET)

check-toolchain:
	@if [ -x "$(CC)" ]; then \
		echo "Toolchain OK: $(CC)"; \
		$(CC) --version | head -1; \
		echo 'int main(void){return 0;}' > /tmp/eabi-probe.c; \
		$(CC) $(ARCHFLAGS) -static -o /tmp/eabi-probe /tmp/eabi-probe.c; \
		readelf -h /tmp/eabi-probe | grep Flags; \
		readelf -h /tmp/eabi-probe | grep -q 'EABI' || { echo "ERROR: non-EABI output"; exit 1; }; \
		echo "(EABI flag present — correct)"; \
	else \
		echo "ERROR: toolchain not found at $(CROSS_PATH)/"; \
		echo "Build the dosbox-armv5-zaurus Buildroot first."; \
		exit 1; \
	fi
