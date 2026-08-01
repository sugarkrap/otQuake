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

# crosstool-NG EABI uClibc-ng toolchain from the sibling piko repo. Its
# sysroot ships libuClibc-1.0.54.so, byte-for-byte the runtime on the device,
# so dynamically linked binaries built here load there.
PIKO       := $(abspath ../piko)
CROSS      ?= arm-unknown-linux-uclibcgnueabi
CROSS_PATH ?= $(PIKO)/toolchain/x-tools/$(CROSS)/bin

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
	cd_null.c     chase.c       cl_demo.c     cl_input.c    cl_main.c  \
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

.PHONY: all clean extract strip strip-x11 check-toolchain check-x11 x11

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

# ── X11 target ────────────────────────────────────────────────────────────────
#
# Same engine, vid_x11.c in place of vid_fb.c: renders into an X window
# instead of taking over /dev/fb0, so it can run alongside Matchbox with the
# panel visible. See the header of src/vid_x11.c for why that is needed.
#
# This one links DYNAMICALLY, unlike quake-fb. The device has a dynamic
# linker (/lib/ld-uClibc.so.1) and the whole X11 stack in /lib, and piko
# stages libX11 as a shared object only -- there is no libX11.a to link
# statically against.

PIKO_STAGE  ?= $(PIKO)/userspace/stage-target
X11_CFLAGS  := -I$(PIKO_STAGE)/usr/include
# -rpath-link lets the linker resolve libX11's own NEEDED entries (libxcb,
# libXau, libXdmcp) without us naming each one.
X11_LDFLAGS := -L$(PIKO_STAGE)/usr/lib -Wl,-rpath-link,$(PIKO_STAGE)/usr/lib \
               -lX11 -lXext -lm

X11_SRCS   := $(patsubst vid_fb.c,vid_x11.c,$(SRCS))
X11_OBJDIR := obj-x11
X11_OBJS   := $(patsubst %.c,$(X11_OBJDIR)/%.o,$(X11_SRCS))
TARGET_X11 := quake-x11

x11: $(TARGET_X11)

$(TARGET_X11): $(X11_OBJS)
	$(CC) $(ARCHFLAGS) -o $@ $^ $(X11_LDFLAGS)
	@echo "Built $@ (EABI, dynamic)"

$(X11_OBJDIR)/%.o: $(SRCDIR)/%.c | $(X11_OBJDIR)
	$(CC) $(CFLAGS) $(X11_CFLAGS) -c -o $@ $<

$(X11_OBJDIR):
	mkdir -p $(X11_OBJDIR)

check-x11:
	@if [ -f "$(PIKO_STAGE)/usr/include/X11/Xlib.h" ]; then \
		echo "X11 stage OK: $(PIKO_STAGE)"; \
	else \
		echo "ERROR: no X11 headers at $(PIKO_STAGE)/usr/include/X11/"; \
		echo "Build piko's X11 stack first (tools/build-x11-stack.sh)."; \
		exit 1; \
	fi

strip: $(TARGET) $(LAUNCHER)
	$(STRIP) $(TARGET) $(LAUNCHER)
	@echo "Stripped $(TARGET) ($(shell wc -c < $(TARGET)) bytes)"
	@echo "Stripped $(LAUNCHER) ($(shell wc -c < $(LAUNCHER)) bytes)"

strip-x11: $(TARGET_X11)
	$(STRIP) $(TARGET_X11)
	@echo "Stripped $(TARGET_X11) ($(shell wc -c < $(TARGET_X11)) bytes)"

extract:
	python3 extract-src.py

clean:
	rm -rf $(OBJDIR) $(X11_OBJDIR) $(TARGET) $(TARGET_X11) $(LAUNCHER)

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
