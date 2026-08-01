/*
 * vid_x11.c  --  X11 window output + X input for otQuake
 *
 * The framebuffer driver (vid_fb.c) owns the whole screen: it writes
 * /dev/fb0 directly and reads evdev. That cannot coexist with an X server,
 * which owns both -- Xfbdev holds an EVIOCGRAB on the keyboard and
 * touchscreen, so an fb-mode Quake started under X renders but never
 * receives a keystroke.
 *
 * This driver renders into an ordinary X window instead. Input arrives as
 * X events, so there is nothing to grab and nothing to fight over, and the
 * Matchbox panel stays visible alongside the game.
 *
 * It exports exactly the same seven entry points vid.c expects, so the two
 * drivers are drop-in alternatives and vid.c is untouched:
 *   CreateQtWindow / KillQtApp / RepaintQtWindow
 *   SetQtPalette / DoQtEventLoop / ProcessOneQtEvent / Vid_ShowError
 *
 * Scaling. Quake rasterises 8-bit paletted pixels at vid.width x vid.height
 * (320x240 by default, see vid.c). We scale that up by an integer factor on
 * the way into the XImage -- 2x by default. That is not just cosmetic: it
 * means the software renderer, which is what actually costs time on a
 * 400MHz PXA with no FPU, keeps working at quarter resolution while the
 * window still fills the screen. The doubling itself is a cheap pair of
 * 32-bit stores per source pixel.
 *
 * Transport. MIT-SHM is used when the server offers it (Xfbdev does), which
 * puts the frame in shared memory instead of pushing every pixel through
 * the X socket each frame. Falls back to XPutImage automatically.
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <X11/extensions/XShm.h>

#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>
#include <linux/fb.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "quakedef.h"

/* Globals owned by vid.c. */
extern viddef_t vid;
extern float    mouse_x, mouse_y;
extern int      mouse_buttonstate;

/* ── X state ────────────────────────────────────────────────────────────── */

static Display *x_dpy;
static int      x_screen;
static Window   x_win;
static GC       x_gc;
static Visual  *x_visual;
static int      x_depth;

static XImage          *x_image;
static XShmSegmentInfo  x_shm;
static int              x_use_shm;

/*
 * Frame pacing for MIT-SHM.
 *
 * The image lives in memory the server reads directly, so we must not start
 * drawing frame N+1 until the server has finished copying frame N out of it.
 * The obvious way to guarantee that is XSync() straight after XShmPutImage,
 * but that blocks on a full round trip with the CPU idle -- and on this
 * hardware the server's copy of a 640x448 frame is not cheap, so that stall
 * dominates.
 *
 * Instead we ask for a ShmCompletion event (send_event True) and only wait
 * for it at the START of the next frame. Everything Quake does in between --
 * running the server frame, the whole software rasterisation -- overlaps
 * with the server's copy for free.
 */
static int shm_completion_type = -1;
static int shm_put_pending;

/*
 * ── direct-framebuffer output (QUAKE_X11_DIRECTFB=1) ───────────────────────
 *
 * Going through the X server costs a second full copy of every frame
 * (client blits into shared memory, server copies that into the
 * framebuffer), and Xfbdev has no vblank sync at all -- it writes straight
 * into the buffer being scanned out, so every X client tears on this
 * hardware.
 *
 * This path keeps the X window purely for what X is good at here --
 * placement, focus, and input events -- and writes the pixels itself,
 * exactly like vid_fb.c does, into the window's rectangle of /dev/fb0.
 * That removes the extra copy and lets us page-flip again.
 *
 * The trick that makes it coexist with the desktop: we render only into
 * the page that is NOT currently being displayed, then flip. w100fb's pan
 * handler waits for a clean vblank edge, so the flip is atomic and nothing
 * tears -- and because we never write to the visible page, a slow frame
 * cannot be caught half-drawn.
 *
 * X, meanwhile, only ever draws into page 0: its mmap is at offset 0 and it
 * knows nothing about the second page. So:
 *
 *   back == 0  the panel and the rest of the desktop are already whatever X
 *              last drew -- correct with no work from us.
 *   back == 1  page 1 has stale desktop content, so we copy the regions
 *              outside our window (here just the panel strip, ~40KB) across
 *              from page 0 first. That also propagates X's own updates --
 *              a ticking clock, say -- without us having to detect them.
 *
 * Falls back to the XShm path if anything here is unavailable: no fb, not
 * 16bpp, or a kernel whose driver cannot pan.
 */
static int   fbo_active;
static int   fbo_fd        = -1;
static void *fbo_mem       = MAP_FAILED;
static int   fbo_mem_size;
static int   fbo_line_len;
static int   fbo_width, fbo_height, fbo_bpp;
static int   fbo_page_bytes;
static int   fbo_front_page;      /* page currently being displayed */

/* Window origin in root coordinates -- where in the fb our pixels go. */
static int win_x, win_y;

static Atom x_wm_delete;

static int win_w, win_h;   /* window size in device pixels */
static int x_scale = 2;    /* integer upscale factor */

/*
 * Pixel values for the 256 palette entries, already packed for the server's
 * visual. Kept as 32-bit so the same table serves both the 16bpp fast path
 * and the generic XPutPixel path.
 */
static unsigned long x_pixel[256];

/*
 * Whether the 16bpp fast blit is usable: depth 15/16, 16 bits per stored
 * pixel, and server byte order matching ours. Anything else (odd depth,
 * a big-endian server) falls back to XPutPixel, which is slow but correct.
 */
static int fast16;

/* QUAKE_X11_DEBUG_KEYS=1: log every key event the server delivers. */
static int debug_keys;

/* ── X error trapping (for the MIT-SHM probe) ──────────────────────────── */

static int shm_error;

static int shm_error_handler(Display *d, XErrorEvent *e)
{
    (void)d; (void)e;
    shm_error = 1;
    return 0;
}

/* ── palette ────────────────────────────────────────────────────────────── */

/* Number of low zero bits in mask, and how many bits wide it is. */
static void mask_shift_bits(unsigned long mask, int *shift, int *bits)
{
    int s = 0, n = 0;

    if (!mask) { *shift = 0; *bits = 8; return; }
    while (!(mask & 1)) { mask >>= 1; s++; }
    while (mask & 1)    { mask >>= 1; n++; }
    *shift = s;
    *bits  = n;
}

void SetQtPalette(unsigned char *palette)
{
    int rs, rb, gs, gb, bs, bb, i;

    if (!x_visual)
        return;

    mask_shift_bits(x_visual->red_mask,   &rs, &rb);
    mask_shift_bits(x_visual->green_mask, &gs, &gb);
    mask_shift_bits(x_visual->blue_mask,  &bs, &bb);

    for (i = 0; i < 256; i++) {
        unsigned long r = palette[i * 3 + 0];
        unsigned long g = palette[i * 3 + 1];
        unsigned long b = palette[i * 3 + 2];

        /* 8-bit component down to however many bits this visual has. */
        x_pixel[i] = ((r >> (8 - rb)) << rs) |
                     ((g >> (8 - gb)) << gs) |
                     ((b >> (8 - bb)) << bs);
    }
}

/* ── keyboard mapping ───────────────────────────────────────────────────── */

/*
 * The d-pad, resolved by keycode rather than keysym.
 *
 * The running Xfbdev does not have the evdev keycode set loaded -- xkbcomp's
 * live upload of zaurus.xkb is unreliable here (see piko's
 * docs/DEADLETTER-XKB-LIVE-SETMAP.md), and the server falls back to the
 * legacy XFree86 set, in which these keycodes mean something else entirely:
 *
 *   111  Up     -> Print          unmapped
 *   113  Left   -> Alt_R          worse than unmapped: K_ALT is +strafe
 *   114  Right  -> no keysym      unmapped
 *   116  Down   -> Super_R        unmapped
 *
 * Going by keycode is not a workaround that some later keymap fix would
 * invalidate: under the correct evdev keycodes these are <UP>, <LEFT>,
 * <RGHT> and <DOWN> -- the kernel's KEY_UP/LEFT/RIGHT/DOWN plus the usual
 * offset of 8 -- so the mapping is right either way. It takes precedence
 * over the keysym lookup precisely because of the Left/Alt_R case, which a
 * fallback-on-unmapped scheme would not catch.
 */
static int xkeycode_to_quake(unsigned int keycode)
{
    switch (keycode) {
    case 111: return K_UPARROW;
    case 113: return K_LEFTARROW;
    case 114: return K_RIGHTARROW;
    case 116: return K_DOWNARROW;
    default:  return -1;
    }
}

static int xkey_to_quake(KeySym ks)
{
    switch (ks) {
    case XK_Escape:       return K_ESCAPE;
    case XK_Tab:          return K_TAB;
    case XK_Return:
    case XK_KP_Enter:     return K_ENTER;
    case XK_BackSpace:    return K_BACKSPACE;
    case XK_space:        return K_SPACE;

    case XK_Up:
    case XK_KP_Up:        return K_UPARROW;
    case XK_Down:
    case XK_KP_Down:      return K_DOWNARROW;
    case XK_Left:
    case XK_KP_Left:      return K_LEFTARROW;
    case XK_Right:
    case XK_KP_Right:     return K_RIGHTARROW;

    case XK_Shift_L:
    case XK_Shift_R:      return K_SHIFT;
    case XK_Control_L:
    case XK_Control_R:    return K_CTRL;
    case XK_Alt_L:
    case XK_Alt_R:
    case XK_Meta_L:
    case XK_Meta_R:
    case XK_ISO_Level3_Shift:  /* the Zaurus Fn key, see zaurus.xkb */
                          return K_ALT;

    case XK_Insert:
    case XK_KP_Insert:    return K_INS;
    case XK_Delete:
    case XK_KP_Delete:    return K_DEL;
    case XK_Home:
    case XK_KP_Home:      return K_HOME;
    case XK_End:
    case XK_KP_End:       return K_END;
    case XK_Prior:
    case XK_KP_Prior:     return K_PGUP;
    case XK_Next:
    case XK_KP_Next:      return K_PGDN;
    case XK_Pause:        return K_PAUSE;

    /*
     * The SL-C860's hardware buttons are wired to F-keys by the corgi
     * keypad driver, and this thumb keyboard has no F-row of its own, so
     * these are the buttons and nothing else:
     *
     *   F1 Calendar   F2 Address   F3 Fn    F4 Cancel
     *   F10 Mail      F11 OK       F12 Menu  F7/F8 jog
     *
     * Cancel is the natural "get me out of here" button, so it becomes
     * ESCAPE -- which opens the menu in game and backs out of it once
     * there. vid_fb.c does the same thing for its equivalent button.
     */
    case XK_F4:  return K_ESCAPE;

    case XK_F1:  return K_F1;   case XK_F2:  return K_F2;
    case XK_F3:  return K_F3;
    case XK_F5:  return K_F5;   case XK_F6:  return K_F6;
    case XK_F7:  return K_F7;   case XK_F8:  return K_F8;
    case XK_F9:  return K_F9;   case XK_F10: return K_F10;
    case XK_F11: return K_F11;  case XK_F12: return K_F12;

    default:
        /*
         * Everything printable maps to itself. Index 0 of the keycode's
         * keysym list is the unshifted symbol, so letters arrive lowercase,
         * which is what Key_Event expects.
         */
        if (ks >= 32 && ks <= 126)
            return (int)ks;
        return -1;
    }
}

/* ── window setup ───────────────────────────────────────────────────────── */

/*
 * Matchbox decorates ordinary windows with a titlebar and will happily
 * resize them. We want neither: the game is a fixed-size borderless
 * surface. Removing decorations is the _MOTIF_WM_HINTS convention, which
 * matchbox-window-manager honours.
 *
 * Deliberately NOT _NET_WM_STATE_FULLSCREEN: matchbox takes that to mean
 * "cover everything", including the panel we are trying to keep visible.
 */
static void set_borderless(void)
{
    struct {
        unsigned long flags, functions, decorations;
        long          input_mode;
        unsigned long status;
    } mwm;
    Atom prop;

    prop = XInternAtom(x_dpy, "_MOTIF_WM_HINTS", False);
    if (prop == None)
        return;

    memset(&mwm, 0, sizeof(mwm));
    mwm.flags       = (1L << 1);   /* MWM_HINTS_DECORATIONS */
    mwm.decorations = 0;

    XChangeProperty(x_dpy, x_win, prop, prop, 32, PropModeReplace,
                    (unsigned char *)&mwm, 5);
}

/*
 * Pin the window to exactly win_w x win_h. Without this matchbox is free to
 * resize us to fill the screen, and the blit assumes the size it was built
 * for.
 */
static void set_fixed_size(void)
{
    XSizeHints *sh = XAllocSizeHints();

    if (!sh)
        return;
    sh->flags      = PMinSize | PMaxSize | PSize;
    sh->min_width  = sh->max_width  = sh->base_width  = win_w;
    sh->min_height = sh->max_height = sh->base_height = win_h;
    XSetWMNormalHints(x_dpy, x_win, sh);
    XFree(sh);
}

/*
 * Work out the render size the same way vid.c will, a few lines after it
 * calls us. vid.c sets vid.width/vid.height *after* CreateQtWindow returns,
 * so we cannot read them here -- but we need them now to size the window.
 * Parsing the same arguments with the same precedence keeps the two in step;
 * if they ever disagree anyway, RepaintQtWindow scales generically rather
 * than trusting this.
 */
static void guess_render_size(int *w, int *h)
{
    int pnum;

    *w = 320;
    *h = 240;

    if ((pnum = COM_CheckParm("-winsize")) && pnum < com_argc - 2) {
        *w = Q_atoi(com_argv[pnum + 1]);
        *h = Q_atoi(com_argv[pnum + 2]);
    }
    if ((pnum = COM_CheckParm("-width")) && pnum < com_argc - 1)
        *w = Q_atoi(com_argv[pnum + 1]);
    if ((pnum = COM_CheckParm("-height")) && pnum < com_argc - 1)
        *h = Q_atoi(com_argv[pnum + 1]);

    if (*w <= 0) *w = 320;
    if (*h <= 0) *h = 240;
}

static void create_image(void)
{
    int (*old_handler)(Display *, XErrorEvent *);

    /* Try MIT-SHM first. */
    if (XShmQueryExtension(x_dpy)) {
        memset(&x_shm, 0, sizeof(x_shm));
        x_image = XShmCreateImage(x_dpy, x_visual, x_depth, ZPixmap,
                                  NULL, &x_shm, win_w, win_h);
        if (x_image) {
            x_shm.shmid = shmget(IPC_PRIVATE,
                                 (size_t)x_image->bytes_per_line * x_image->height,
                                 IPC_CREAT | 0777);
            if (x_shm.shmid >= 0) {
                x_shm.shmaddr = x_image->data = shmat(x_shm.shmid, NULL, 0);
                x_shm.readOnly = False;
                if (x_shm.shmaddr != (char *)-1) {
                    shm_error = 0;
                    old_handler = XSetErrorHandler(shm_error_handler);
                    XShmAttach(x_dpy, &x_shm);
                    XSync(x_dpy, False);
                    XSetErrorHandler(old_handler);

                    if (!shm_error) {
                        x_use_shm = 1;
                        shm_completion_type =
                            XShmGetEventBase(x_dpy) + ShmCompletion;
                        /*
                         * Mark the segment destroyed now: it stays alive
                         * while attached, and this way it cannot leak if we
                         * die without running the cleanup path.
                         */
                        shmctl(x_shm.shmid, IPC_RMID, NULL);
                        fprintf(stderr, "vid_x11: using MIT-SHM\n");
                        return;
                    }
                    shmdt(x_shm.shmaddr);
                }
                shmctl(x_shm.shmid, IPC_RMID, NULL);
            }
            XDestroyImage(x_image);
            x_image = NULL;
        }
        fprintf(stderr, "vid_x11: MIT-SHM unavailable, using XPutImage\n");
    }

    /* Plain XImage over the socket. */
    {
        int    pad   = 32;
        size_t bytes = (size_t)win_w * win_h * 4;   /* worst case 32bpp */
        char  *data  = (char *)malloc(bytes);

        if (!data)
            Sys_Error("vid_x11: out of memory for image");

        x_image = XCreateImage(x_dpy, x_visual, x_depth, ZPixmap, 0,
                               data, win_w, win_h, pad, 0);
        if (!x_image)
            Sys_Error("vid_x11: XCreateImage failed");
    }
}

/*
 * Put the visible page back to 0 before we go away.
 *
 * This matters more than it looks. X only ever draws into page 0, so if we
 * exit while page 1 is being displayed, the screen is frozen on our last
 * frame forever: X carries on redrawing the desktop into a page nobody is
 * looking at, and the machine appears hung when it is perfectly healthy.
 *
 * Written to be safe from a signal handler: only ioctl, no allocation, no
 * stdio.
 */
static void restore_front_page(void)
{
    struct fb_var_screeninfo pan;

    if (fbo_fd < 0 || fbo_front_page == 0)
        return;

    memset(&pan, 0, sizeof(pan));
    if (ioctl(fbo_fd, FBIOGET_VSCREENINFO, &pan) == 0) {
        pan.xoffset = 0;
        pan.yoffset = 0;
        ioctl(fbo_fd, FBIOPAN_DISPLAY, &pan);
    }
    fbo_front_page = 0;
}

/*
 * Quake's own shutdown path runs KillQtApp, but a plain `kill` does not go
 * anywhere near it -- and leaving the panel scanning out page 1 looks
 * exactly like a frozen device. vid_fb.c installs handlers for the same
 * reason (it has a VT mode to put back); this one has a page to put back.
 *
 * SIGKILL still cannot be caught, so it can still strand the display. The
 * fix for that case is to restart X, which reprograms the mode.
 */
static void sig_handler(int sig)
{
    (void)sig;
    restore_front_page();
    _exit(0);
}

/* Current absolute position of our window, for the direct-fb blit. */
static void update_window_origin(void)
{
    Window child;
    int rx = 0, ry = 0;

    if (XTranslateCoordinates(x_dpy, x_win, RootWindow(x_dpy, x_screen),
                              0, 0, &rx, &ry, &child)) {
        win_x = rx;
        win_y = ry;
    }
}

/*
 * Map /dev/fb0 with a doubled virtual resolution so we have a second page to
 * render into. Everything here is best-effort: any failure just leaves
 * fbo_active clear and we keep using XShm.
 */
static void init_directfb(void)
{
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    struct fb_var_screeninfo req;

    fbo_fd = open("/dev/fb0", O_RDWR);
    if (fbo_fd < 0)
        fbo_fd = open("/dev/fb/0", O_RDWR);
    if (fbo_fd < 0) {
        fprintf(stderr, "vid_x11: directfb: cannot open /dev/fb0 (%s)\n",
                strerror(errno));
        return;
    }

    if (ioctl(fbo_fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
        ioctl(fbo_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        fprintf(stderr, "vid_x11: directfb: fb ioctl failed\n");
        goto fail;
    }

    fbo_width    = (int)vinfo.xres;
    fbo_height   = (int)vinfo.yres;
    fbo_bpp      = (int)vinfo.bits_per_pixel;
    fbo_line_len = (int)finfo.line_length;

    if (fbo_bpp != 16) {
        fprintf(stderr, "vid_x11: directfb: need 16bpp, got %d\n", fbo_bpp);
        goto fail;
    }

    /*
     * Page flipping is the whole point -- without it we would be writing
     * into the live scanout buffer and tearing just as badly as XShm does.
     * ypanstep == 0 means the driver cannot pan, so don't bother.
     */
    if (finfo.ypanstep == 0) {
        fprintf(stderr, "vid_x11: directfb: driver cannot pan, no page flip\n");
        goto fail;
    }

    fbo_page_bytes = fbo_line_len * fbo_height;

    req = vinfo;
    req.yres_virtual = vinfo.yres * 2;
    req.xoffset = 0;
    req.yoffset = 0;
    if (ioctl(fbo_fd, FBIOPUT_VSCREENINFO, &req) < 0 ||
        req.yres_virtual < vinfo.yres * 2) {
        fprintf(stderr, "vid_x11: directfb: no doubled virtual fb\n");
        goto fail;
    }

    fbo_mem_size = fbo_page_bytes * 2;
    fbo_mem = mmap(NULL, (size_t)fbo_mem_size, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fbo_fd, 0);
    if (fbo_mem == MAP_FAILED) {
        fprintf(stderr, "vid_x11: directfb: mmap failed (%s)\n",
                strerror(errno));
        goto fail;
    }

    /*
     * Seed page 1 with what is on screen now, so the very first flip shows
     * a complete desktop rather than uninitialised memory.
     */
    memcpy((unsigned char *)fbo_mem + fbo_page_bytes, fbo_mem,
           (size_t)fbo_page_bytes);

    fbo_front_page = 0;
    fbo_active = 1;

    /* Only now that a flip is possible is there anything to restore. */
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGHUP,  sig_handler);

    fprintf(stderr, "vid_x11: direct fb output, %dx%d %dbpp line=%d, "
            "page flip enabled\n", fbo_width, fbo_height, fbo_bpp, fbo_line_len);
    return;

fail:
    if (fbo_mem != MAP_FAILED) {
        munmap(fbo_mem, (size_t)fbo_mem_size);
        fbo_mem = MAP_FAILED;
    }
    if (fbo_fd >= 0) {
        close(fbo_fd);
        fbo_fd = -1;
    }
    fprintf(stderr, "vid_x11: directfb unavailable, falling back to XShm\n");
}

void CreateQtWindow(void)
{
    XSetWindowAttributes attr;
    const char *env;
    int src_w, src_h;
    int screen_w, screen_h;
    XEvent ev;

    x_dpy = XOpenDisplay(NULL);
    if (!x_dpy)
        Sys_Error("vid_x11: cannot open display '%s' -- is X running?",
                  getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");

    x_screen = DefaultScreen(x_dpy);
    x_visual = DefaultVisual(x_dpy, x_screen);
    x_depth  = DefaultDepth(x_dpy, x_screen);

    guess_render_size(&src_w, &src_h);

    env = getenv("QUAKE_X11_DEBUG_KEYS");
    if (env && *env && strcmp(env, "0") != 0)
        debug_keys = 1;

    env = getenv("QUAKE_X11_SCALE");
    if (env && *env) {
        x_scale = atoi(env);
        if (x_scale < 1)
            x_scale = 1;
    }

    /* Never ask for a window larger than the screen. */
    screen_w = DisplayWidth(x_dpy, x_screen);
    screen_h = DisplayHeight(x_dpy, x_screen);
    while (x_scale > 1 &&
           (src_w * x_scale > screen_w || src_h * x_scale > screen_h))
        x_scale--;

    win_w = src_w * x_scale;
    win_h = src_h * x_scale;

    memset(&attr, 0, sizeof(attr));
    attr.background_pixel = BlackPixel(x_dpy, x_screen);
    attr.border_pixel     = BlackPixel(x_dpy, x_screen);
    attr.event_mask       = KeyPressMask | KeyReleaseMask |
                            ButtonPressMask | ButtonReleaseMask |
                            PointerMotionMask | ExposureMask |
                            StructureNotifyMask | FocusChangeMask;

    x_win = XCreateWindow(x_dpy, RootWindow(x_dpy, x_screen),
                          0, 0, win_w, win_h, 0,
                          x_depth, InputOutput, x_visual,
                          CWBackPixel | CWBorderPixel | CWEventMask, &attr);

    XStoreName(x_dpy, x_win, "otQuake");
    set_fixed_size();
    set_borderless();

    x_wm_delete = XInternAtom(x_dpy, "WM_DELETE_WINDOW", False);
    if (x_wm_delete != None)
        XSetWMProtocols(x_dpy, x_win, &x_wm_delete, 1);

    x_gc = XCreateGC(x_dpy, x_win, 0, NULL);

    XMapWindow(x_dpy, x_win);
    /* Wait until the window is actually mapped before drawing into it. */
    do {
        XNextEvent(x_dpy, &ev);
    } while (ev.type != MapNotify);

    /*
     * Without this, X synthesises a KeyRelease/KeyPress pair for every
     * auto-repeat tick, so a held movement key reads as frantic tapping.
     */
    XkbSetDetectableAutoRepeat(x_dpy, True, NULL);

    update_window_origin();

    env = getenv("QUAKE_X11_DIRECTFB");
    if (env && *env && strcmp(env, "0") != 0)
        init_directfb();

    if (fbo_active) {
        fprintf(stderr, "vid_x11: %dx%d window at %d,%d, %dx%d render, %dx scale\n",
                win_w, win_h, win_x, win_y, src_w, src_h, x_scale);
        return;                    /* no XImage needed on this path */
    }

    create_image();

    /*
     * Decide once whether the fast path applies, rather than re-testing per
     * pixel. LSBFirst matches this ARM build; a mismatch would need byte
     * swapping, which the generic path handles for us.
     */
    fast16 = (x_image->bits_per_pixel == 16 &&
              x_image->byte_order == LSBFirst);

    fprintf(stderr, "vid_x11: %dx%d window, %dx%d render, %dx scale, depth %d%s\n",
            win_w, win_h, src_w, src_h, x_scale, x_depth,
            fast16 ? "" : " (generic slow blit)");
}

/* ── cleanup ────────────────────────────────────────────────────────────── */

void KillQtApp(void)
{
    if (fbo_mem != MAP_FAILED) {
        restore_front_page();
        munmap(fbo_mem, (size_t)fbo_mem_size);
        fbo_mem = MAP_FAILED;
    }
    if (fbo_fd >= 0) {
        close(fbo_fd);
        fbo_fd = -1;
    }
    fbo_active = 0;

    if (!x_dpy)
        return;

    if (x_image) {
        if (x_use_shm) {
            XShmDetach(x_dpy, &x_shm);
            XDestroyImage(x_image);
            shmdt(x_shm.shmaddr);
        } else {
            XDestroyImage(x_image);   /* frees the malloc'd data too */
        }
        x_image = NULL;
    }

    XCloseDisplay(x_dpy);
    x_dpy = NULL;
}

/* ── frame output ───────────────────────────────────────────────────────── */

/*
 * Copy the parts of the screen that are NOT ours from page 0 (the only page
 * X draws into) to the page we are about to display. Without this, flipping
 * to page 1 would show whatever the desktop looked like when we started,
 * frozen -- a clock that never ticks, menus that never appear.
 *
 * Only the rows above and below our window plus the left/right margins are
 * copied; on the usual 640x448-window-over-640x480-screen layout that is
 * just the 32-pixel panel strip.
 */
static void preserve_desktop(unsigned char *back)
{
    const unsigned char *page0 = (const unsigned char *)fbo_mem;
    int y0 = win_y;
    int y1 = win_y + win_h;
    int y;

    if (y0 < 0) y0 = 0;
    if (y1 > fbo_height) y1 = fbo_height;

    /* Full rows above and below the window. */
    if (y0 > 0)
        memcpy(back, page0, (size_t)y0 * fbo_line_len);
    if (y1 < fbo_height)
        memcpy(back + (size_t)y1 * fbo_line_len,
               page0 + (size_t)y1 * fbo_line_len,
               (size_t)(fbo_height - y1) * fbo_line_len);

    /* Left and right margins on the window's own rows. */
    if (win_x > 0 || win_x + win_w < fbo_width) {
        int left_bytes  = win_x > 0 ? win_x * 2 : 0;
        int right_start = win_x + win_w;
        int right_bytes = right_start < fbo_width
                        ? (fbo_width - right_start) * 2 : 0;

        for (y = y0; y < y1; y++) {
            size_t off = (size_t)y * fbo_line_len;

            if (left_bytes)
                memcpy(back + off, page0 + off, (size_t)left_bytes);
            if (right_bytes)
                memcpy(back + off + (size_t)right_start * 2,
                       page0 + off + (size_t)right_start * 2,
                       (size_t)right_bytes);
        }
    }
}

/* Render one frame straight into the off-screen page, then flip to it. */
static void repaint_directfb(void)
{
    const unsigned char *src = (const unsigned char *)vid.buffer;
    struct fb_var_screeninfo pan;
    unsigned char *back;
    int back_page = 1 - fbo_front_page;
    int sw = vid.width, sh = vid.height, rb = vid.rowbytes;
    int sx, sy, y, x;
    int clip_w, clip_h;

    if (!src)
        return;

    back = (unsigned char *)fbo_mem + (size_t)back_page * fbo_page_bytes;

    /*
     * When we are about to draw into page 0 the desktop there is already
     * live -- X owns it -- so copying over it would be pointless work.
     */
    if (back_page != 0)
        preserve_desktop(back);

    sx = sw > 0 ? win_w / sw : 1;
    sy = sh > 0 ? win_h / sh : 1;
    if (sx < 1) sx = 1;
    if (sy < 1) sy = 1;

    /* Never write outside the screen, whatever the WM did with our window. */
    clip_w = fbo_width  - win_x;
    clip_h = fbo_height - win_y;
    if (clip_w > win_w) clip_w = win_w;
    if (clip_h > win_h) clip_h = win_h;
    if (clip_w <= 0 || clip_h <= 0)
        goto flip;

    if (sw * sx > clip_w) sw = clip_w / sx;
    if (sh * sy > clip_h) sh = clip_h / sy;

    if (sx == 2 && sy == 2) {
        /* Same doubling trick as vid_fb: one 32-bit store per pixel pair. */
        for (y = 0; y < sh; y++) {
            const unsigned char *srow = src + y * rb;
            unsigned int *d0 = (unsigned int *)
                (back + (size_t)(win_y + y * 2)     * fbo_line_len + win_x * 2);
            unsigned int *d1 = (unsigned int *)
                (back + (size_t)(win_y + y * 2 + 1) * fbo_line_len + win_x * 2);

            for (x = 0; x < sw; x++) {
                unsigned int cc = (unsigned int)x_pixel[srow[x]];
                cc |= cc << 16;
                d0[x] = cc;
                d1[x] = cc;
            }
        }
    } else {
        for (y = 0; y < sh; y++) {
            const unsigned char *srow = src + y * rb;
            unsigned short *drow0 = (unsigned short *)
                (back + (size_t)(win_y + y * sy) * fbo_line_len + win_x * 2);
            int i;

            for (x = 0; x < sw; x++) {
                unsigned short cc = (unsigned short)x_pixel[srow[x]];
                for (i = 0; i < sx; i++)
                    drow0[x * sx + i] = cc;
            }
            for (i = 1; i < sy; i++)
                memcpy(back + (size_t)(win_y + y * sy + i) * fbo_line_len
                            + win_x * 2,
                       drow0, (size_t)sw * sx * 2);
        }
    }

flip:
    memset(&pan, 0, sizeof(pan));
    if (ioctl(fbo_fd, FBIOGET_VSCREENINFO, &pan) < 0) {
        fprintf(stderr, "vid_x11: directfb: VSCREENINFO failed, "
                "reverting to XShm\n");
        fbo_active = 0;
        return;
    }
    pan.xoffset = 0;
    pan.yoffset = back_page * fbo_height;
    if (ioctl(fbo_fd, FBIOPAN_DISPLAY, &pan) < 0) {
        fprintf(stderr, "vid_x11: directfb: pan failed (%s), "
                "reverting to XShm\n", strerror(errno));
        fbo_active = 0;
        return;
    }
    fbo_front_page = back_page;
}

void RepaintQtWindow(void)
{
    const unsigned char *src;
    int sw, sh, rb, bpl;
    int sx, sy, y, x;

    if (fbo_active) {
        repaint_directfb();
        return;
    }

    if (!x_dpy)
        return;

    /*
     * The direct-fb path gave up part way through (a failed pan, say). It
     * skipped creating an XImage at startup, so build one now rather than
     * leaving the window blank for the rest of the session.
     */
    if (!x_image) {
        create_image();
        fast16 = (x_image->bits_per_pixel == 16 &&
                  x_image->byte_order == LSBFirst);
    }

    src = (const unsigned char *)vid.buffer;
    if (!src)
        return;

    /*
     * Block here, not after the put: by now the server has had a whole
     * Quake frame's worth of time to finish reading the image, so this
     * usually returns immediately.
     */
    if (shm_put_pending) {
        XEvent done;

        while (XCheckTypedEvent(x_dpy, shm_completion_type, &done) == False) {
            /*
             * Nothing yet. XCheckTypedEvent does not flush, so push the
             * request out and block until something -- anything -- arrives,
             * rather than spinning on the CPU we are trying to save.
             */
            XFlush(x_dpy);
            if (XCheckTypedEvent(x_dpy, shm_completion_type, &done) != False)
                break;
            usleep(500);
        }
        shm_put_pending = 0;
    }

    sw  = vid.width;
    sh  = vid.height;
    rb  = vid.rowbytes;
    bpl = x_image->bytes_per_line;

    /*
     * Integer scale actually achievable for this frame. Normally this is
     * x_scale, but deriving it from the real vid dimensions means a
     * mismatch degrades to a correct smaller image instead of scribbling
     * past the end of the buffer.
     */
    sx = sw > 0 ? win_w / sw : 1;
    sy = sh > 0 ? win_h / sh : 1;
    if (sx < 1) sx = 1;
    if (sy < 1) sy = 1;
    if (sw * sx > win_w) sw = win_w / sx;
    if (sh * sy > win_h) sh = win_h / sy;

    if (fast16 && sx == 2 && sy == 2) {
        /*
         * The common case: 320x240 -> 640x480. Two source pixels are packed
         * into one 32-bit store, and the doubled row is written twice, so
         * each source pixel costs two stores instead of four.
         */
        for (y = 0; y < sh; y++) {
            const unsigned char *srow = src + y * rb;
            unsigned int *d0 = (unsigned int *)(x_image->data + (2 * y)     * bpl);
            unsigned int *d1 = (unsigned int *)(x_image->data + (2 * y + 1) * bpl);

            for (x = 0; x < sw; x++) {
                unsigned int cc = (unsigned int)x_pixel[srow[x]];
                cc |= cc << 16;
                d0[x] = cc;
                d1[x] = cc;
            }
        }
    } else if (fast16) {
        /* Any other integer scale, still 16bpp. */
        for (y = 0; y < sh; y++) {
            const unsigned char *srow = src + y * rb;
            unsigned short *drow =
                (unsigned short *)(x_image->data + (y * sy) * bpl);
            int i;

            for (x = 0; x < sw; x++) {
                unsigned short cc = (unsigned short)x_pixel[srow[x]];
                for (i = 0; i < sx; i++)
                    drow[x * sx + i] = cc;
            }
            /* Replicate the finished row down for the remaining sy-1 lines. */
            for (i = 1; i < sy; i++)
                memcpy(x_image->data + (y * sy + i) * bpl, drow,
                       (size_t)sw * sx * 2);
        }
    } else {
        /* Correct anywhere: unusual depth, or a byte order we don't match. */
        for (y = 0; y < sh; y++) {
            const unsigned char *srow = src + y * rb;
            int i, j;

            for (x = 0; x < sw; x++) {
                unsigned long cc = x_pixel[srow[x]];
                for (j = 0; j < sy; j++)
                    for (i = 0; i < sx; i++)
                        XPutPixel(x_image, x * sx + i, y * sy + j, cc);
            }
        }
    }

    if (x_use_shm) {
        /* True: ask for the completion event the next frame waits on. */
        XShmPutImage(x_dpy, x_win, x_gc, x_image,
                     0, 0, 0, 0, win_w, win_h, True);
        shm_put_pending = 1;
        XFlush(x_dpy);
    } else {
        XPutImage(x_dpy, x_win, x_gc, x_image, 0, 0, 0, 0, win_w, win_h);
        XFlush(x_dpy);
    }
}

/* ── input ──────────────────────────────────────────────────────────────── */

void DoQtEventLoop(void)
{
    /* sys_linux.c drives the loop via Host_Frame -- nothing to do here. */
}

void ProcessOneQtEvent(void)
{
    static int have_last;
    static int last_x, last_y;
    XEvent ev;
    KeySym ks;
    int qk;

    if (!x_dpy)
        return;

    while (XPending(x_dpy)) {
        XNextEvent(x_dpy, &ev);

        /*
         * This drains the whole queue, so it will happily eat the
         * ShmCompletion event that RepaintQtWindow is waiting on. Claim it
         * here instead -- otherwise the next frame blocks forever on an
         * event that was already thrown away.
         */
        if (x_use_shm && ev.type == shm_completion_type) {
            shm_put_pending = 0;
            continue;
        }

        switch (ev.type) {
        case KeyPress:
        case KeyRelease:
            ks = XLookupKeysym(&ev.xkey, 0);
            qk = xkeycode_to_quake(ev.xkey.keycode);
            if (qk < 0)
                qk = xkey_to_quake(ks);
            /*
             * QUAKE_X11_DEBUG_KEYS=1 reports every key the server actually
             * delivers. Worth keeping: it distinguishes "the window manager
             * swallowed that key" (nothing logged at all) from "we got it
             * and did not recognise the keysym" (logged with qk -1), which
             * are otherwise indistinguishable from the game's behaviour.
             */
            if (debug_keys)
                fprintf(stderr, "vid_x11: key %s keycode=%u keysym=0x%lx (%s)"
                        " -> quake %d\n",
                        ev.type == KeyPress ? "down" : "up  ",
                        ev.xkey.keycode, (unsigned long)ks,
                        XKeysymToString(ks) ? XKeysymToString(ks) : "?", qk);
            if (qk >= 0)
                Key_Event(qk, (qboolean)(ev.type == KeyPress));
            break;

        case ButtonPress:
        case ButtonRelease: {
            /*
             * Quake's bit order is left, right, middle -- IN_Commands in
             * vid.c turns bit i into K_MOUSE1+i -- while X numbers them
             * left, middle, right. Hence the swap.
             */
            int bit = -1;

            if (ev.xbutton.button == Button1)      bit = 0;
            else if (ev.xbutton.button == Button3) bit = 1;
            else if (ev.xbutton.button == Button2) bit = 2;

            if (bit >= 0) {
                if (ev.type == ButtonPress)
                    mouse_buttonstate |=  (1 << bit);
                else
                    mouse_buttonstate &= ~(1 << bit);
            }

            /*
             * Pen down and pen up both start a fresh stroke. This is what
             * makes touch look work: the touchscreen reports absolute
             * positions and only while pressed, so without resetting here
             * the first sample of a new stroke would be measured against
             * wherever the previous one ended and fling the view across the
             * map. Resetting means each stroke contributes only its own
             * movement, and you can lift and drag again to keep turning
             * past what one screen-width of travel would allow.
             */
            have_last = 0;
            break;
        }

        case MotionNotify:
            /*
             * Accumulate movement relative to the previous sample of the
             * SAME stroke; see the ButtonPress/Release handler for why
             * have_last is cleared at each stroke boundary. Run with
             * -nomouse to ignore the pointer entirely.
             */
            if (have_last) {
                mouse_x += (float)(ev.xmotion.x - last_x);
                mouse_y += (float)(ev.xmotion.y - last_y);
            }
            last_x = ev.xmotion.x;
            last_y = ev.xmotion.y;
            have_last = 1;
            break;

        case LeaveNotify:
        case FocusOut:
            have_last = 0;
            break;

        case Expose:
            /* Next frame repaints everything anyway. */
            break;

        case ConfigureNotify:
            /*
             * The direct-fb path blits to absolute screen coordinates, so a
             * move by the window manager has to be tracked or we would keep
             * drawing over the old rectangle. Coordinates in the event are
             * relative to the parent, so ask X for the real ones.
             */
            update_window_origin();
            break;

        case ClientMessage:
            if (x_wm_delete != None &&
                (Atom)ev.xclient.data.l[0] == x_wm_delete)
                Sys_Quit();
            break;

        default:
            break;
        }
    }
}

void Vid_ShowError(const char *string)
{
    fprintf(stderr, "Quake error: %s\n", string);
}
