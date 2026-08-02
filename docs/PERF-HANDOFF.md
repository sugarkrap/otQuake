# Performance work on the SL-C860 — state, results, and what not to repeat

Written at the end of a profiling/optimisation session on real hardware
(Sharp Zaurus SL-C860, PXA255 @ 400 MHz, ARMv5TE, no FPU, w100 framebuffer).

The short version: **the framebuffer blit was 61% of the frame and nobody had
looked at it.** The rasteriser everyone assumes is the bottleneck was 20%.
Every real gain in this session came after measuring; the change made *before*
measuring delivered nothing.

---

## 1. The baseline profile

Add `r_profile 1` and run a timedemo; totals are reported once at the end from
`CL_FinishTimeDemo`. On `demo1`, 969 frames, 87.5 ms/frame (11.4 fps), quiet
device:

| phase | ms/frame | % of frame |
|---|---:|---:|
| **blit to /dev/fb0** | **53.21** | **61%** |
| — blit: pixel loop | 53.00 | 60.4% |
| — blit: pan ioctl | 0.21 | 0.15% |
| scan edges (total) | 17.25 | 20% |
| — span texturing | 7.79 | 8.9% |
| — surface cache build | 3.11 | 3.6% |
| — z spans | 1.78 | 2.0% |
| — edge scan proper | 4.57 | 5.2% |
| world (BSP walk + edges) | 6.24 | 7.1% |
| entities (alias models) | 3.33 | 3.8% |
| view model | 3.10 | 3.5% |
| bmodels | 1.31 | 1.5% |
| particles | 0.47 | 0.5% |

Renderer + blit = 85.2 of 87.5 ms, i.e. **97% accounted for**, so the buckets
can be trusted. Two runs at ~1.5× different absolute speed (one CPU-contended)
produced near-identical *proportions*, which cross-validates them.

The decisive detail: the page-flip ioctl is **0.15%**. The blit is not waiting
on the panel — it is raw uncached store bandwidth.

---

## 2. What shipped, and what it actually bought

| Change | Where | Measured effect |
|---|---|---|
| `USE_JR_OPT1` enabled, then **completed** (8 → 5 → 2 `__aeabi_idiv` in `D_DrawSpans8`) | PR #6, #7 | **None measurable.** Means within 1.2% over 12 counterbalanced runs |
| Blit: build each doubled row in cached scratch, `memcpy` it out (LDM/STM bursts) | PR #7 | see below |
| Blit: honour the dirty rect — `vid.c` was *discarding* the rect and repainting all 640×480 every frame | PR #7 | combined: blit ÷ renderer **1.66 → 1.33 (−20%)**; scanlines to 88% of full; observed ~11 → 15 fps peak, "noticeably more fluid" |
| `r_profile` accumulating per-phase profiler | PR #7 | the tool that made the rest possible |
| `vid_fb`: `fflush(stdout)` before `_exit` in the signal handler | PR #7 | bug fix — console output was silently lost on any signal exit |
| `sys_linux`: line-buffer stdout | PR #7 | bug fix — redirected output appeared thousands of chars late |
| **Makefile `-MMD -MP`** header dependency tracking | PR #7 | there was **none**. Editing a header recompiled only the edited `.c` files and linked them against stale objects |
| `w100fb`: framebuffer mapped write-combining (`L_PTE_MT_BUFFERABLE`) instead of uncached-unbuffered | piko PR #74 | **−3.7%, within noise.** Kept as architecturally correct and free, *not* as a demonstrated win |

### Unverified

**otQuake PR #8 (draft)** — per-page *shadow* dirty detection, replacing the
dirty-rect scheme. Builds clean; **never run on hardware** (RetroArch held
`/dev/fb0`). The rect can only skip regions the engine declares clean; a
shadow also skips unchanged scanlines *inside* the viewport. It also deletes
the union-of-two-rects bookkeeping, because a per-page shadow answers "what
does this page already contain" exactly rather than conservatively.

---

## 3. Rejected, with evidence — do not spend time re-deriving these

**Hardware pixel-doubling: does not exist.** The w100's `GRAPHIC_CTRL` exposes
only `color_depth`, `portrait_mode`, `low_power_on`, `req_freq`, CRTC enables
and clock bits — no scaler, no zoom, no pixel-replication field anywhere in
`w100fb.h`. The 2D "DP" engine does non-scaling BitBLT (`fillrect`/`copyarea`)
only. Scanout is strictly 1:1 with the panel, so writing fewer bytes
necessarily means a smaller image with borders.

**Hand-written ARM asm for the span inner loop: no headroom.** GCC already
emits

```
ldrb  ip, [r2, r5, asr #16]    @ texel fetch: shift + index in ONE instruction
strb  ip, [r1], #1             @ store, post-increment
```

having strength-reduced the `(t>>16) * cachewidth` multiply into an
incremental row pointer. There is no multiply in the per-pixel loop at all.
The remaining cost is the *dependent texture load* — cache behaviour, not
instruction count.

**Removing soft-float divides from the span drawer: real, but irrelevant.**
The divides are per-*8-pixels*; the inner loop that runs 8× as often is a
cache-missing load plus a byte store. Even making span texturing infinitely
fast caps out at ~9% of the frame.

---

## 4. Benchmarking methodology — the traps, all of which bit us

* **`r_dynamicscale` must be off.** It adapts viewsize to measured fps, so a
  faster build quietly renders *more pixels* and hides its own win. Use
  `+r_dynamicscale 0 +viewsize 100`.
* **Discard the first run.** The first timedemo after X stops reads `pak0.pak`
  and the map cold off the SD card and ran ~8% slow — the same magnitude as
  the effect being measured. It made a null result look like +9%.
* **Counterbalance the order.** Times drift upward across a session; a fixed
  `base,jr,full` order aliases that drift onto build identity. Reverse the
  order on alternate rounds.
* **Your own SSH polling perturbs the run.** Each connection's crypto
  handshake steals measurable CPU on a 400 MHz device; one poll produced a 20%
  outlier. Go quiet during timing runs.
* **Normalise against the renderer** when comparing blit changes under
  unavoidable background load: the blit changes cannot affect renderer time,
  so `blit ÷ renderer` cancels uniform contention. (Caveat: not valid if the
  interfering load is memory-bus heavy rather than CPU heavy.)
* **`-condebug` does nothing** — `Con_DebugLog`'s body is commented out in
  `console.c`. Read results from stdout instead.
* **Verify blit changes by capturing the real framebuffer**, never the in-game
  `screenshot` command — that dumps `vid.buffer`, the *render target*, and
  will look perfect even if the blit is completely broken. And note the back
  page is **legitimately torn mid-blit**: compare the page reported visible
  via `yoffset`, or you will chase a bug that isn't there.

---

## 5. Device gotchas that cost real time

* **busybox here has no `kill`, `killall`, `awk`, `nohup`, or `dd`.** Every
  `kill -SIG pid` silently does nothing, which looks exactly like "the process
  ignored the signal". Small static ARM helpers (`sig`, `fbgrab`) are the way
  round it.
* **`cat /dev/fb0` fails with `EINVAL`** — w100fb implements `mmap` but not
  `read`. Capture via mmap.
* **The SD card unmounts itself at runtime.** corgi's pxamci card-detect is
  unreliable (piko's own `chunked-deploy.sh` documents this). Anything written
  to `/mnt/card` while it is unmounted silently lands on the ~68 MiB root
  jffs2 instead. This is the most likely explanation for card-installed
  software appearing to vanish — nothing erased it; check `mount | grep card`
  first. **Stage test binaries on the rootfs**, not the card.
* **X is respawned by init** (`tty1::respawn`), so killing it just brings it
  back and it takes `/dev/fb0`. SIGSTOP the xsession shell first; and init can
  hit its respawn rate limit, after which the session must be started by hand.
* Kernel changes must land in piko's `modules/`, never in `kernel-src/` — the
  latter is regenerated and edits disappear silently.

---

## 6. Where to pick up

1. **Verify PR #8.** The only outstanding item with real upside on what is
   still ~56% of the frame. Compare `blit scanlines: N of 240` against the
   **212/240 (88%)** the rect scheme achieved, and capture the framebuffer in
   gameplay *and* with the console down.
2. **Try `d_mipscale` / `d_mipcap`** — already cvars (`d_init.c`), zero code.
   The span loop is memory-bound on texture fetches, so biasing toward higher
   mip levels shrinks the working set and attacks span texturing (8.9%) *and*
   surface cache build (3.6%) together. Costs texture sharpness, not geometry.
3. Anything else worth doing has to come from a fresh `r_profile` run — the
   balance has shifted since the numbers above were taken.
