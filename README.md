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

## Screenshots

Coming soon.
