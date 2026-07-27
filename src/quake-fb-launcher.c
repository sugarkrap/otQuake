/*
 * quake-fb-launcher  —  launch framebuffer Quake
 *
 * This wrapper only starts quake-fb with a configurable basedir and
 * preserves any extra command-line options.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#define DEFAULT_QUAKE_FB  "/usr/sbin/quake-fb"
#define DEFAULT_BASEDIR   "/mnt/card"

int main(int argc, char **argv)
{
    const char *basedir = getenv("QUAKE_BASEDIR");
    const char *quake_bin = getenv("QUAKE_BIN");
    char *child_argv[64];
    int i, out = 0;

    if (!quake_bin || !*quake_bin)
        quake_bin = DEFAULT_QUAKE_FB;

    if (!basedir || !*basedir)
        basedir = DEFAULT_BASEDIR;

    child_argv[out++] = (char *)quake_bin;
    child_argv[out++] = "-nosound";
    child_argv[out++] = "-basedir";
    child_argv[out++] = (char *)basedir;

    for (i = 1; i < argc && out < (int)(sizeof(child_argv) / sizeof(child_argv[0])) - 1; i++)
        child_argv[out++] = argv[i];
    child_argv[out] = NULL;

    execv(quake_bin, child_argv);
    perror("execv quake-fb");
    return 1;
}
