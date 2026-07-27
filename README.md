# handheldquake — framebuffer port for Sharp Zaurus SL-C860

Quake (`handheldquake`, id1 engine + FixedPointMath soft-float renderer)
ported to talk to the Linux kernel framebuffer directly, with no Qt/Qtopia
dependency. Built for the custom Linux 7.1.4 kernel + initramfs environment
in the sibling `zaurus-refresh` repo (no Cacko/Qtopia userland there at all).

Note: the stock Cacko ROM already ships a working Qtopia Quake
(`qpe-quake_1.5.0-2`) that runs fine from the Games tab — this port exists
for the *new*, Qt-free kernel/initramfs environment, not to replace that.

## Why drop Qt

The original `vid_qt.cpp` bridge (Trolltech, 2001) created a `QWidget`,
pumped the Qt event loop, and relied on Qtopia/QWS to pick the right screen
rotation. Getting Qtopia to rotate correctly needed hacky, resolution-formula
workarounds (see `handheldquake-borked/` in the sibling folder for that
history) and still produced a glitchy display. The new kernel/initramfs
target has no Qtopia at all, so the whole rotation-guessing problem is
sidestepped by talking to `/dev/fb0` directly.

## Display path

The SL-C860 (Corgi) panel is physically portrait (480×640), but the
`w100fb` kernel driver hardware-rotates it. The board file
(`corgi_patched.c` in `zaurus-refresh`) sets `init_mode = INIT_MODE_ROTATED`,
so **by default at boot the framebuffer already reports 640×480 landscape**
via `FBIOGET_VSCREENINFO` — no userspace mode-setting is required.

`src/vid_fb.c` reads whatever mode is current (`FBIOGET_VSCREENINFO` /
`FBIOGET_FSCREENINFO`), mmaps `/dev/fb0`, and:
- takes a fast 2× pixel-doubling path when `fb_width == vid.width*2 &&
  fb_height == vid.height*2` (the expected 640×480 ← 320×240 case)
- falls back to a 1:1 top-left blit (letterboxed) for any other geometry

It exports the exact function names `vid.c` already expects from the old Qt
bridge (`CreateQtWindow`, `KillQtApp`, `RepaintQtWindow`, `SetQtPalette`,
`DoQtEventLoop`, `ProcessOneQtEvent`) — so **`vid.c` itself is untouched**,
only the bridge implementation changed. Input comes from `/dev/input/event*`
(evdev), mapped to Quake keys in `evkey_to_quake()`; SL-C860 hardware buttons
(`KEY_MENU`, `KEY_HOMEPAGE`, `KEY_BACK`) are mapped to F1/F2/Escape.

Confirmed 16bpp RGB565 is the only pixel format `w100fb` presents
(`w100fb.c`: "16-bit True Colour"), which matches `vid_fb.c`'s
`fb_bpp != 16` guard and its RGB565 palette pack in `SetQtPalette`.

Known pre-existing kernel-side quirk (not fixed here, out of scope for this
repo): `w100fb_patched.c:829` has `if (inf->init_mode &= INIT_MODE_FLIPPED)`
— an assignment where a comparison (`&`) was almost certainly intended. It
mutates the shared platform-data struct as a side effect. Only matters if
`init_mode` is read again after probe; noted for whoever next touches
`w100fb_patched.c`.

## Layout

```
handheldquake/          ← raw CVS/RCS vault (,v files), historical, gitignored
Attic/ CVSROOT/         ← CVS bookkeeping, gitignored
zlib/ fdlibm/ libfloat/ ← CVS vault for libs the fb build doesn't use, gitignored
data/                   ← id1/pak0.pak.gz,v (Id Software game data, gitignored — not ours to redistribute)
extract-src.py          ← pulls head revisions out of the CVS vault into src/
src/                    ← extracted + hand-patched source actually built
Makefile                ← cross-builds quake-fb + quake-fb-launcher
```

`src/vid_fb.c` and `src/quake-fb-launcher.c` are new files, not from CVS.
Four CVS-derived files got small portability/safety fixes along the way:
- `console.c`, `FixedPointMath.c` — missing `<unistd.h>`/`<stdlib.h>` includes
- `d_surf.c` — `return;` → `return NULL;` in a non-`void` function
- `r_edge.c` — null-pointer guards in the active-edge-table walk (was crashing)

## Build

Requires the EABI uClibc-ng toolchain from the sibling `dosbox-armv5-zaurus`
Buildroot (`../dosbox-armv5-zaurus/buildroot/output/host/bin/`).

```sh
make check-toolchain   # verify the cross-compiler is present and EABI
make                    # builds ./quake-fb and ./quake-fb-launcher
make strip              # strip both binaries in place
make extract            # re-extract src/ from the CVS vault (rarely needed)
make clean
```

Produces static ARMv5TE/XScale EABI soft-float binaries — no libc/SDL/Qt
runtime dependencies on the device.

## Deploy

Copy `quake-fb`, `quake-fb-launcher`, and the game data
(`/mnt/card/id1/pak0.pak`, 172 MB, not tracked here) to the device, then run
`quake-fb-launcher` (wraps `quake-fb -nosound -basedir $QUAKE_BASEDIR`,
default `/mnt/card`).

Zaurus SSH access (current lease): `10.208.47.22`. See `zaurus-refresh`'s
`docs/DEADLETTER.md` for the full auth flags (old KEX/HostKey algorithms
needed for this device's ancient OpenSSH) — those haven't changed, only the
DHCP-assigned IP has.

## Status

- [x] Source extracted from CVS vault, builds clean for ARMv5TE EABI
- [x] `vid_fb.c` written: mmap `/dev/fb0`, RGB565 palette, evdev input
- [x] Confirmed board default boot mode is already 640×480 landscape
      (`INIT_MODE_ROTATED` in `corgi_patched.c`) — fast path will engage
- [ ] Not yet run on real hardware or in QEMU — display path unverified live
- [ ] Sound is disabled (`-nosound` in the launcher); no fb-side audio driver
- [ ] `w100fb_patched.c:829` `&=`/`&` bug — flagged, not fixed (kernel repo)
