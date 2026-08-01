# otQuake

*Quake on a Sharp Zaurus from 2004??? Nani?*

otQuake — **Old Tux Quake** — is Quake, ported to run on old Linux distros
and devices that time (and upstream support) forgot.

The current target is the Sharp Zaurus SL-C860: a Qtopia-era PDA that
originally ran Quake through the Qt/Qtopia windowing stack. otQuake drops
that dependency and talks to the Linux kernel framebuffer directly, so it
can run on a bare kernel + initramfs with no windowing system at all. The
video driver reads whatever screen mode the device already booted into and
renders straight into `/dev/fb0` — no Qt, no X, no rotation guesswork.

What's next is mostly chasing performance on ARMv5TE/XScale hardware that
never had a working FPU to begin with.

## Building

You'll need the ARMv5TE EABI uClibc-ng cross-toolchain from the sibling
`dosbox-armv5-zaurus` Buildroot. Build that first, then:

```sh
make check-toolchain   # verify the cross-compiler is present and EABI
make                    # builds ./quake-fb and ./quake-fb-launcher
make strip              # strip both binaries in place
```

### Testing on your dev machine

`make host` builds `./quake-host`, a native (non-ARM) build for quickly
testing changes without cross-compiling and copying to real hardware. It
reuses `vid_fb.c` as-is (harmless no-op if `/dev/fb0` isn't accessible —
desktop Linux usually has a compositor holding the display already) but
swaps the Zaurus's mmap'd OSS sound driver for a plain ALSA one
(`src/snd_alsa.c`), since desktop Linux has neither `/dev/dsp` nor OSS mmap
support. Run it from a directory containing `id1/` (game data + any
converted `music/`); use the `cd` console command (`cd play 2`, `cd stop`,
`cd info`, ...) to test CD music playback directly without loading a map.

## Music

There's no CD drive, so CD tracks are emulated by streaming plain PCM
`music/trackNN.wav` files (see `src/cd_wav.c`) straight into the same mixer
the sound effects use. There's no decoder or resampler — this hardware has
no FPU — so tracks must already be 16-bit PCM at the exact rate/channels
`quake-fb` negotiates with `/dev/dsp` (11025 Hz stereo by default; see
`tryrates[]` in `src/snd_sun.c`, or `-sndspeed`/`-sndmono`/`-sndstereo`).

`convert-music.sh` converts a folder of Quake's original `trackNN.ogg`
files (or a mod's own music folder) to matching `.wav` files with ffmpeg:

```sh
./convert-music.sh /path/to/extracted/id1/music id1/music
```

## Screenshots

Coming soon.
