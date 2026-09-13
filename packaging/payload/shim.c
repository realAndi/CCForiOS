/*  ccios shim — lets the macOS arm64 Claude Code (Bun/JSC) binary run on iOS.
 *
 *  The binary reaches these functions through patched LC_DYLD_CHAINED_FIXUPS
 *  import ordinals (machfix.py), not DYLD_INSERT_LIBRARIES: dyld ignores DYLD_*
 *  for a binary carrying entitlements, and the import table is two-level so an
 *  inserted library would never win the lookup anyway.
 *
 *  Two jobs:
 *
 *  1. Supply the four symbols the macOS build imports from libSystem that iOS's
 *     libSystem does not export (found with symcheck: 4 missing out of 840).
 *
 *  2. Make JSC's JIT work. macOS arm64 JSC maps its executable pool with MAP_JIT
 *     and flips per-thread W^X via pthread_jit_write_protect_np(). iOS rejects
 *     MAP_JIT (EINVAL) and refuses to execute a page while it is writable
 *     (SIGBUS) -- but mprotect() may flip a page between RW and RX. So: strip
 *     MAP_JIT, remember the region, and implement the toggle as an mprotect().
 *     This is process-wide where the real API is per-thread, so the binary must
 *     run with BUN_JSC_useConcurrentJIT=0 -- otherwise a background compiler
 *     thread flips the pool to RW while the main thread is executing from it.
 *     Without working JIT, JSC sets useJIT=false and then forces
 *     useSharedArrayBuffer=false, and Claude Code dies at startup on
 *     `new SharedArrayBuffer(4)`.
 *
 *  (The matching macOS-only framework paths Bun dlopen()s are fixed in the
 *  binary's string data by machfix.py, not here.)
 */

#include <sys/mman.h>
#include <libkern/OSCacheControl.h>
#include <spawn.h>
#include <errno.h>
#include <dlfcn.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#ifndef MAP_JIT
#define MAP_JIT 0x0800
#endif

#define MAX_JIT_REGIONS 16

static struct { void *addr; size_t len; } g_jit[MAX_JIT_REGIONS];
static int             g_jit_count = 0;
static pthread_mutex_t g_jit_lock  = PTHREAD_MUTEX_INITIALIZER;
static int             g_debug     = -1;

static int debug_on(void) {
    if (g_debug < 0) g_debug = getenv("CCIOS_SHIM_DEBUG") != NULL;
    return g_debug;
}

/* Resolve the libSystem original of a symbol this file also defines. dlsym on
   an explicit libSystem handle, not RTLD_DEFAULT: the default search walks the
   global list and finds this shim's own definition first. */
static void *libsystem_handle(void) {
    static void *h = NULL;
    if (h == NULL) {
        h = dlopen("/usr/lib/libSystem.B.dylib", RTLD_LAZY | RTLD_NOLOAD);
        if (h == NULL) h = dlopen("/usr/lib/libSystem.B.dylib", RTLD_LAZY);
    }
    return h;
}

static void *real_sym(const char *name, void *self) {
    void *h = libsystem_handle();
    void *p = h ? dlsym(h, name) : NULL;
    if (p == self) {          /* would recurse -- should never happen */
        fprintf(stderr, "[ccios] fatal: %s resolved to the shim itself\n", name);
        abort();
    }
    return p;
}

/* ---- 2. JIT memory ---------------------------------------------------- */

typedef void *(*mmap_fn)(void *, size_t, int, int, int, off_t);

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset) {
    static mmap_fn real = NULL;
    if (real == NULL) real = (mmap_fn)real_sym("mmap", (void *)mmap);

    if ((flags & MAP_JIT) == 0)
        return real(addr, len, prot, flags, fd, offset);

    /* JSC's executable pool. Map it RW now; the toggle below swings it to RX
       when JSC is ready to run what it wrote. */
    flags &= ~MAP_JIT;
    int initial = prot;
    if ((prot & PROT_WRITE) && (prot & PROT_EXEC)) initial = prot & ~PROT_EXEC;

    void *p = real(addr, len, initial, flags, fd, offset);
    if (p == MAP_FAILED) return p;

    pthread_mutex_lock(&g_jit_lock);
    if (g_jit_count < MAX_JIT_REGIONS) {
        g_jit[g_jit_count].addr = p;
        g_jit[g_jit_count].len  = len;
        g_jit_count++;
    }
    pthread_mutex_unlock(&g_jit_lock);

    if (debug_on())
        fprintf(stderr, "[ccios] JIT pool %p len=%zu prot=0x%x->0x%x\n",
                p, len, prot, initial);
    return p;
}

/* enabled=0: "about to write"; enabled=1: "done, must be executable again". */
void pthread_jit_write_protect_np(int enabled) {
    int prot = enabled ? (PROT_READ | PROT_EXEC) : (PROT_READ | PROT_WRITE);
    pthread_mutex_lock(&g_jit_lock);
    for (int i = 0; i < g_jit_count; i++) {
        if (mprotect(g_jit[i].addr, g_jit[i].len, prot) != 0 && debug_on())
            fprintf(stderr, "[ccios] mprotect(%p, %zu, 0x%x) failed: %d\n",
                    g_jit[i].addr, g_jit[i].len, prot, errno);
    }
    pthread_mutex_unlock(&g_jit_lock);
}

int pthread_jit_write_protect_supported_np(void) { return 1; }

/* ---- 1. missing libSystem symbols -------------------------------------- */

/* compiler-rt builtin the JIT calls after emitting code; iOS spells the same
   operation sys_icache_invalidate. */
void __clear_cache(void *begin, void *end) {
    sys_icache_invalidate(begin, (char *)end - (char *)begin);
}

/* The un-suffixed spelling postdates iOS 17; the _np one is present there. */
int posix_spawn_file_actions_addfchdir(posix_spawn_file_actions_t *acts, int fd) {
    static int (*np)(posix_spawn_file_actions_t *, int) = NULL;
    if (np == NULL) np = dlsym(RTLD_DEFAULT, "posix_spawn_file_actions_addfchdir_np");
    if (np != NULL) return np(acts, fd);
    return ENOSYS;
}
