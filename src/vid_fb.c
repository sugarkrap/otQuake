/*
 * vid_fb.c  –  Linux framebuffer + evdev input for handheldquake
 *
 * Replaces vid_qt.cpp.  Exports the same interface that vid.c expects:
 *   CreateQtWindow / KillQtApp / RepaintQtWindow
 *   SetQtPalette / DoQtEventLoop / ProcessOneQtEvent / Vid_ShowError
 *
 * Display: direct framebuffer output. The preferred fast path is a 2x
 * landscape blit (320x240 -> 640x480). A generic fallback is used for
 * other framebuffer geometries.
 *
 * Input: /dev/input/event* (evdev).
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>

#include "quakedef.h"  /* pulls in keys.h, vid.h, etc. */

/* ── framebuffer state ─────────────────────────────────────────────────── */

static int   fb_fd        = -1;
static void *fb_mem       = MAP_FAILED;
static int   fb_mem_size  = 0;
static int   fb_line_len  = 0;   /* bytes per screen line (from FSCREENINFO) */

static int fb_width  = 640;
static int fb_height = 480;
static int fb_bpp    = 16;
static int fb_use_vsync = 0;

/* VT ownership: tell fbcon to get out of the way while we own the screen. */
static int tty_fd = -1;
static int tty_in_graphics_mode = 0;

/*
 * Double buffering (page flip) state.
 *
 * The previous approach waited for vsync and then wrote the whole frame
 * directly into the SAME buffer the panel is scanning out. On this
 * hardware a full software raster (2x blit or 1:1) takes long enough that
 * the panel's scan position laps the write position more than once per
 * frame, producing several drifting diagonal tear lines -- waiting for
 * vsync only fixed where the FIRST tear starts, not the rest.
 *
 * If the running kernel's w100fb reports ypanstep (added 2026-07-27 to
 * support this), we instead request a doubled virtual framebuffer, draw
 * each frame entirely into the currently INVISIBLE page, and then pan to
 * it with FBIOPAN_DISPLAY once the draw is complete. w100fb's pan handler
 * itself waits for a clean vblank edge before flipping GRAPHIC_OFFSET, so
 * the flip is atomic from the panel's point of view -- zero tearing,
 * regardless of how long the software blit takes (as long as it's under
 * one refresh period on average).
 *
 * Falls back to the old single-buffer direct-write path (with optional
 * QUAKE_VSYNC gate) on older kernels that don't support panning.
 */
static int fb_pageflip_active = 0;
static int fb_back_page = 1;      /* page index we draw into next */
static int fb_page_bytes = 0;     /* line_len * height, i.e. one page */

/* pre-built 8-bit index → RGB565 palette */
static unsigned short palette16[256];

/* ── input state ────────────────────────────────────────────────────────── */

#define MAX_INPUT_FDS 4
static int input_fds[MAX_INPUT_FDS];
static int input_nfds = 0;
static int input_grab = 0;

/* ── evdev → Quake key mapping ──────────────────────────────────────────── */

static int evkey_to_quake(unsigned int code)
{
    switch (code) {
    /* control */
    case KEY_ESC:        return K_ESCAPE;
    case KEY_TAB:        return K_TAB;
    case KEY_ENTER:      return K_ENTER;
    case KEY_BACKSPACE:  return K_BACKSPACE;
    case KEY_SPACE:      return K_SPACE;

    /* arrows */
    case KEY_UP:         return K_UPARROW;
    case KEY_DOWN:       return K_DOWNARROW;
    case KEY_LEFT:       return K_LEFTARROW;
    case KEY_RIGHT:      return K_RIGHTARROW;

    /* modifiers */
    case KEY_LEFTSHIFT:
    case KEY_RIGHTSHIFT: return K_SHIFT;
    case KEY_LEFTCTRL:
    case KEY_RIGHTCTRL:  return K_CTRL;
    case KEY_LEFTALT:
    case KEY_RIGHTALT:   return K_ALT;

    /* navigation */
    case KEY_INSERT:     return K_INS;
    case KEY_DELETE:     return K_DEL;
    case KEY_HOME:       return K_HOME;
    case KEY_END:        return K_END;
    case KEY_PAGEUP:     return K_PGUP;
    case KEY_PAGEDOWN:   return K_PGDN;
    case KEY_PAUSE:      return K_PAUSE;

    /* function keys */
    case KEY_F1:         return K_F1;
    case KEY_F2:         return K_F2;
    case KEY_F3:         return K_F3;
    case KEY_F4:         return K_F4;
    case KEY_F5:         return K_F5;
    case KEY_F6:         return K_F6;
    case KEY_F7:         return K_F7;
    case KEY_F8:         return K_F8;
    case KEY_F9:         return K_F9;
    case KEY_F10:        return K_F10;
    case KEY_F11:        return K_F11;
    case KEY_F12:        return K_F12;

    /* SL-C860 hardware buttons → function keys */
    case KEY_MENU:       return K_F1;
    case KEY_HOMEPAGE:   return K_F2;
    case KEY_BACK:       return K_ESCAPE;

    /* printable: letters */
    case KEY_A: return 'a'; case KEY_B: return 'b';
    case KEY_C: return 'c'; case KEY_D: return 'd';
    case KEY_E: return 'e'; case KEY_F: return 'f';
    case KEY_G: return 'g'; case KEY_H: return 'h';
    case KEY_I: return 'i'; case KEY_J: return 'j';
    case KEY_K: return 'k'; case KEY_L: return 'l';
    case KEY_M: return 'm'; case KEY_N: return 'n';
    case KEY_O: return 'o'; case KEY_P: return 'p';
    case KEY_Q: return 'q'; case KEY_R: return 'r';
    case KEY_S: return 's'; case KEY_T: return 't';
    case KEY_U: return 'u'; case KEY_V: return 'v';
    case KEY_W: return 'w'; case KEY_X: return 'x';
    case KEY_Y: return 'y'; case KEY_Z: return 'z';

    /* digits */
    case KEY_0: return '0'; case KEY_1: return '1';
    case KEY_2: return '2'; case KEY_3: return '3';
    case KEY_4: return '4'; case KEY_5: return '5';
    case KEY_6: return '6'; case KEY_7: return '7';
    case KEY_8: return '8'; case KEY_9: return '9';

    /* punctuation */
    case KEY_MINUS:      return '-';
    case KEY_EQUAL:      return '=';
    case KEY_LEFTBRACE:  return '[';
    case KEY_RIGHTBRACE: return ']';
    case KEY_SEMICOLON:  return ';';
    case KEY_APOSTROPHE: return '\'';
    case KEY_GRAVE:      return '`';
    case KEY_BACKSLASH:  return '\\';
    case KEY_COMMA:      return ',';
    case KEY_DOT:        return '.';
    case KEY_SLASH:      return '/';

    default: return -1;
    }
}

/* ── VT ownership ───────────────────────────────────────────────────────── */

/*
 * Put the active VT into KD_GRAPHICS so fbcon stops drawing its text
 * cursor / any console output on top of our framebuffer writes. Without
 * this, whatever's still attached to the console can scribble over the
 * game image at any time -- purely a display glitch, not a crash, but a
 * visible one. Best-effort: if there's no accessible tty (e.g. launched
 * detached from any console), just skip it silently.
 */
static void tty_graphics_mode(void)
{
    tty_fd = open("/dev/tty0", O_RDWR);
    if (tty_fd < 0)
        tty_fd = open("/dev/console", O_RDWR);
    if (tty_fd < 0) {
        fprintf(stderr, "vid_fb: no console tty available, "
                "fbcon text/cursor may still draw over the screen\n");
        return;
    }
    if (ioctl(tty_fd, KDSETMODE, KD_GRAPHICS) < 0) {
        fprintf(stderr, "vid_fb: KDSETMODE KD_GRAPHICS failed (%s)\n",
                strerror(errno));
        close(tty_fd);
        tty_fd = -1;
        return;
    }
    tty_in_graphics_mode = 1;
}

static void tty_text_mode(void)
{
    if (tty_fd >= 0) {
        if (tty_in_graphics_mode)
            ioctl(tty_fd, KDSETMODE, KD_TEXT);
        close(tty_fd);
        tty_fd = -1;
    }
    tty_in_graphics_mode = 0;
}

/* ── cleanup ────────────────────────────────────────────────────────────── */

static void fb_cleanup(void)
{
    int i;
    tty_text_mode();
    if (fb_mem != MAP_FAILED) {
        memset(fb_mem, 0, (size_t)fb_mem_size);
        munmap(fb_mem, (size_t)fb_mem_size);
        fb_mem = MAP_FAILED;
    }
    if (fb_fd >= 0) { close(fb_fd); fb_fd = -1; }
    for (i = 0; i < input_nfds; i++) {
        if (input_grab)
            ioctl(input_fds[i], EVIOCGRAB, 0);
        close(input_fds[i]);
    }
    input_nfds = 0;
}

static void sig_handler(int sig)
{
    (void)sig;
    fb_cleanup();
    _exit(0);
}

/* ── public interface ───────────────────────────────────────────────────── */

void CreateQtWindow(void)
{
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    const char *grab_env;
    const char *vsync_env;
    char path[32];
    int i, fd;

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    grab_env = getenv("QUAKE_GRAB_INPUT");
    if (grab_env && *grab_env && strcmp(grab_env, "0") != 0)
        input_grab = 1;

    vsync_env = getenv("QUAKE_VSYNC");
    if (vsync_env && *vsync_env && strcmp(vsync_env, "0") != 0)
        fb_use_vsync = 1;

    /* open framebuffer */
    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0)
        fb_fd = open("/dev/fb/0", O_RDWR);
    if (fb_fd < 0) {
        perror("open /dev/fb0");
        return;
    }

    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
        ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        perror("fb ioctl");
        close(fb_fd); fb_fd = -1;
        return;
    }

    fb_width    = (int)vinfo.xres;
    fb_height   = (int)vinfo.yres;
    fb_bpp      = (int)vinfo.bits_per_pixel;
    fb_line_len = (int)finfo.line_length;
    fb_page_bytes = fb_line_len * fb_height;
    fb_mem_size = fb_page_bytes;

    /*
     * Try to get a doubled virtual framebuffer for page flipping. Only
     * attempt this if the driver actually advertises ypanstep -- older
     * kernels (ypanstep==0) don't support panning at all and would just
     * fail FBIOPAN_DISPLAY every frame.
     */
    if (finfo.ypanstep > 0) {
        struct fb_var_screeninfo req = vinfo;
        req.yres_virtual = vinfo.yres * 2;
        req.xoffset = 0;
        req.yoffset = 0;
        if (ioctl(fb_fd, FBIOPUT_VSCREENINFO, &req) == 0 &&
            req.yres_virtual >= vinfo.yres * 2) {
            fb_mem_size = fb_page_bytes * 2;
            fb_pageflip_active = 1;
        } else {
            fprintf(stderr, "vid_fb: doubled virtual fb not available, "
                    "falling back to single-buffer direct write\n");
        }
    }

    fb_mem = mmap(NULL, (size_t)fb_mem_size,
                  PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_mem == MAP_FAILED) {
        perror("mmap /dev/fb0");
        close(fb_fd); fb_fd = -1;
        return;
    }

    memset(fb_mem, 0, (size_t)fb_mem_size);
    tty_graphics_mode();
    fprintf(stderr, "vid_fb: %dx%d %dbpp line=%d\n",
            fb_width, fb_height, fb_bpp, fb_line_len);
    if (fb_pageflip_active) {
        fb_back_page = 1;
        fprintf(stderr, "vid_fb: page flip enabled (double-buffered)\n");
    } else if (fb_use_vsync) {
        fprintf(stderr, "vid_fb: vsync wait enabled (single-buffer)\n");
    }

    /* open evdev input devices */
    for (i = 0; i < 16 && input_nfds < MAX_INPUT_FDS; i++) {
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            continue;
        if (input_grab)
            ioctl(fd, EVIOCGRAB, 1);   /* optional exclusive access */
        input_fds[input_nfds++] = fd;
        fprintf(stderr, "vid_fb: input %s%s\n", path,
                input_grab ? " (grab)" : "");
    }
    if (input_nfds == 0)
        fprintf(stderr, "vid_fb: warning: no input event devices found\n");
}

void KillQtApp(void)
{
    fb_cleanup();
}

void SetQtPalette(unsigned char *palette)
{
    int i;
    for (i = 0; i < 256; i++) {
        unsigned int r = palette[i * 3 + 0];
        unsigned int g = palette[i * 3 + 1];
        unsigned int b = palette[i * 3 + 2];
        /* RGB565: RRRRR GGGGGG BBBBB */
        palette16[i] = (unsigned short)(((r >> 3) << 11) |
                                        ((g >> 2) <<  5) |
                                         (b >> 3));
    }
}

void RepaintQtWindow(void)
{
    const unsigned char *src;
    int rb;
    unsigned char *dst_base;

    if (fb_mem == MAP_FAILED || fb_bpp != 16)
        return;

    src = (const unsigned char *)vid.buffer;
    rb  = vid.rowbytes;

    if (fb_pageflip_active) {
        /* Always draw into the currently invisible page. */
        dst_base = (unsigned char *)fb_mem + (fb_back_page * fb_page_bytes);
    } else {
        dst_base = (unsigned char *)fb_mem;
#ifdef FBIO_WAITFORVSYNC
        if (fb_use_vsync && fb_fd >= 0) {
            int vblank = 0;
            if (ioctl(fb_fd, FBIO_WAITFORVSYNC, &vblank) < 0) {
                /* Some fb drivers do not implement this ioctl; fall back silently after one warning. */
                if (errno == ENOTTY || errno == EINVAL || errno == ENOSYS) {
                    fprintf(stderr, "vid_fb: vsync ioctl unsupported, disabling\n");
                    fb_use_vsync = 0;
                }
            }
        }
#else
        if (fb_use_vsync) {
            fprintf(stderr, "vid_fb: vsync unavailable at build time\n");
            fb_use_vsync = 0;
        }
#endif
    }

    if (vid.width * 2 == fb_width && vid.height * 2 == fb_height) {
        /*
         * Landscape fb: exact 2× pixel-doubling (e.g. 320×240 → 640×480).
         * Pair of shorts written as a 32-bit store to halve uncached writes.
         */
        int sw = vid.width, sh = vid.height;
        int y, x;
        for (y = 0; y < sh; y++) {
            const unsigned char *srow = src + y * rb;
            unsigned int *drow0 =
                (unsigned int *)(dst_base + (y * 2)     * fb_line_len);
            unsigned int *drow1 =
                (unsigned int *)(dst_base + (y * 2 + 1) * fb_line_len);
            for (x = 0; x < sw; x++) {
                unsigned int cc = (unsigned int)palette16[srow[x]];
                cc |= cc << 16;
                drow0[x] = cc;
                drow1[x] = cc;
            }
        }
    } else {
        /* Fallback: 1:1 blit, black borders if render < physical */
        int sw = vid.width, sh = vid.height;
        int bw = sw < fb_width  ? sw : fb_width;
        int bh = sh < fb_height ? sh : fb_height;
        int stride_shorts = fb_line_len / 2;
        int y, x;
        for (y = 0; y < fb_height; y++) {
            unsigned short *drow =
                (unsigned short *)dst_base + y * stride_shorts;
            if (y < bh) {
                const unsigned char *srow = src + y * rb;
                for (x = 0; x < bw; x++)
                    drow[x] = palette16[srow[x]];
                for (; x < fb_width; x++)
                    drow[x] = 0;
            } else {
                memset(drow, 0,
                       (size_t)fb_width * sizeof(unsigned short));
            }
        }
    }

    if (fb_pageflip_active) {
        struct fb_var_screeninfo pan;

        memset(&pan, 0, sizeof(pan));
        if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &pan) < 0) {
            fprintf(stderr, "vid_fb: FBIOGET_VSCREENINFO failed, "
                    "disabling page flip\n");
            fb_pageflip_active = 0;
            return;
        }
        pan.xoffset = 0;
        pan.yoffset = fb_back_page * fb_height;
        if (ioctl(fb_fd, FBIOPAN_DISPLAY, &pan) < 0) {
            fprintf(stderr, "vid_fb: FBIOPAN_DISPLAY failed (%s), "
                    "disabling page flip\n", strerror(errno));
            fb_pageflip_active = 0;
            return;
        }
        /* The page we just made visible becomes off-screen again next
         * frame's write target's PAIR -- draw into the other page now. */
        fb_back_page = 1 - fb_back_page;
    }
}

void DoQtEventLoop(void)
{
    /* sys_linux.c drives the loop via Host_Frame — nothing to do here */
}

void ProcessOneQtEvent(void)
{
    struct input_event ev;
    ssize_t n;
    int i, qk;

    for (i = 0; i < input_nfds; i++) {
        while (1) {
            n = read(input_fds[i], &ev, sizeof(ev));
            if (n < (ssize_t)sizeof(ev))
                break;
            if (ev.type != EV_KEY)
                continue;
            if (ev.value == 2)          /* auto-repeat, ignore */
                continue;
            qk = evkey_to_quake(ev.code);
            if (qk < 0)
                continue;
            Key_Event(qk, (qboolean)(ev.value != 0));
        }
    }
}

void Vid_ShowError(const char *string)
{
    fprintf(stderr, "Quake error: %s\n", string);
}
