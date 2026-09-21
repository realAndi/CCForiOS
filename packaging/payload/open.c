/* open: iOS stand-in for macOS's open(1), forwarding to uiopen.
 *
 * Claude Code opens URLs -- the sign-in page, docs, a bug report -- through the
 * `open` npm package, which on darwin spawns `open` and looks it up on PATH.
 * iOS has no /usr/bin/open; uikittools' uiopen is the equivalent, but the
 * library never consults BROWSER on darwin, so the only way in is a program
 * called `open`. This is it: parse the flags macOS open(1) accepts, keep the
 * target, hand it to uiopen.
 *
 * A #! script would not do: Bun on iOS cannot posix_spawn one (EPERM), which is
 * how a PATH shim for `security` was missed the first time round. Compiled, and
 * signed like everything else in this package.
 *
 * CCIOS_OPEN_DRYRUN=1 prints what would be opened instead of opening it.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define UIOPEN "/var/jb/usr/bin/uiopen"

/* open(1) flags that take a value; everything else starting with - is a
 * boolean we can drop (-g, -n, -W, --background, ...). Anything after --args
 * belongs to the opened application, which uiopen cannot pass on. */
static int takes_value(const char *flag) {
    static const char *with_value[] = { "-a", "-b", "-u", "-i", "--env", NULL };
    for (int i = 0; with_value[i] != NULL; i++)
        if (strcmp(flag, with_value[i]) == 0) return 1;
    return 0;
}

int main(int argc, char **argv) {
    const char *target = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--args") == 0) break;
        if (a[0] == '-' && a[1] != '\0') {
            if (takes_value(a)) {
                const char *value = (i + 1 < argc) ? argv[++i] : NULL;
                if (value != NULL && strcmp(a, "-u") == 0 && target == NULL)
                    target = value;             /* -u <url> names the target */
            }
            continue;
        }
        if (target == NULL) target = a;
    }

    if (target == NULL) {
        fprintf(stderr, "open: nothing to open\n");
        return 1;
    }

    if (getenv("CCIOS_OPEN_DRYRUN") != NULL) {
        printf("would open: %s\n", target);
        return 0;
    }

    char *args[] = { (char *)"uiopen", (char *)target, NULL };
    execv(UIOPEN, args);
    if (errno == ENOENT) {
        execvp("uiopen", args);                 /* a bootstrap that puts it elsewhere */
        fprintf(stderr, "open: uiopen not found -- install uikittools. URL: %s\n", target);
        return 127;
    }
    fprintf(stderr, "open: could not run %s: %s. URL: %s\n", UIOPEN, strerror(errno), target);
    return 127;
}
