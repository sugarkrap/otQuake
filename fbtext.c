/*
 * fbtext -- unconditionally restore the console to KD_TEXT.
 *
 * vid_fb.c's own SIGINT/SIGTERM handler already does this on a graceful
 * stop (e.g. qstop's pkillx, which sends SIGTERM). But SIGKILL, a crash,
 * or an OOM-kill can't be caught by any process, so the console is left
 * stuck in KD_GRAPHICS with no cursor and no visible shell. This is run
 * by the `quake` wrapper script *after* quake-fb-launcher exits, whatever
 * the reason, as an unconditional safety net.
 */
#include <fcntl.h>
#include <linux/kd.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(void)
{
    int fd = open("/dev/tty0", O_RDWR);
    if (fd < 0)
        fd = open("/dev/console", O_RDWR);
    if (fd < 0)
        return 1;
    ioctl(fd, KDSETMODE, KD_TEXT);
    close(fd);
    return 0;
}
