/*  ccios shim — lets the macOS arm64 Claude Code (Bun/JSC) binary run on iOS.
 *
 *  The binary reaches these functions through patched LC_DYLD_CHAINED_FIXUPS
 *  import ordinals (ccios_patch.py), not DYLD_INSERT_LIBRARIES: dyld ignores
 *  DYLD_* for a binary carrying entitlements, and the import table is two-level
 *  so an inserted library would never win the lookup anyway.
 *
 *  Three jobs:
 *
 *  1. Supply the four symbols the macOS build imports from libSystem that iOS's
 *     libSystem does not export (found with symcheck: 4 missing out of 840).
 *
 *  2. Make JSC's JIT work, from any number of threads. See section 2. Without
 *     a working JIT, JSC sets useJIT=false and then forces
 *     useSharedArrayBuffer=false, and Claude Code dies at startup on
 *     `new SharedArrayBuffer(4)`.
 *
 *  3. Run on A12-class CPUs. The build targets M1-class ones, and an A12 traps
 *     on the ARMv8.4+ instructions it uses; the SIGILL handler performs each
 *     one and steps over it. See section 3.
 *
 *  (The matching macOS-only framework paths Bun dlopen()s are fixed in the
 *  binary's string data by ccios_patch.py, not here.)
 */

#include <sys/mman.h>
#include <sys/ucontext.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <spawn.h>
#include <errno.h>
#include <dlfcn.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#ifndef MAP_JIT
#define MAP_JIT 0x0800
#endif

static int g_debug = -1;

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
    if (p == self || p == NULL) {   /* would recurse, or cannot continue */
        fprintf(stderr, "[ccios] fatal: cannot resolve libSystem's %s\n", name);
        abort();
    }
    return p;
}

/* ---- 2. JIT memory ---------------------------------------------------- */

/* macOS arm64 JSC maps its executable pool with MAP_JIT and flips W^X through
   pthread_jit_write_protect_np(), which is per thread: a thread that is writing
   code sees the pool read-write while every other thread keeps executing from
   it read-execute. iOS has neither. It rejects MAP_JIT (EINVAL), refuses to
   execute a page that is writable (SIGBUS) and refuses RWX even for a process
   jbctl has marked debugged -- but it lets mprotect() move a page between RW
   and RX, and a fault on the wrong one can be repaired and the instruction
   retried.

   The first version of this shim flipped the whole pool on every call, which
   is process-wide. Any second thread that writes code -- JSC's concurrent
   compiler, or a worker's own VM -- swung the pool to RW under the main thread
   and killed it with "panic: Bus error". The same shim in the Pi port did so
   in 8 of 9 runs with one busy worker, and Pi extensions start workers.

   So now nothing is flipped eagerly. Each page is RW or RX, and faults decide:

     write fault, thread inside a write scope   make that page RW, and count
                                                this thread as its writer
     execute fault                              wait until no thread is still
                                                writing the page, make it RX
     leaving the write scope                    drop this thread's writer
                                                counts; pages stay as they are

   The faulting instruction then simply runs again. Per thread, that is what
   the real API gives JSC: a writer can write, everybody else can execute, and
   an executor never runs a page mid-write. Pages that are only ever executed,
   the common case, stop faulting after the first time.

   The fault handler has to see these faults before Bun's crash handler does,
   so the shim installs it first and interposes sigaction() and signal() for
   SIGBUS and SIGSEGV (and SIGILL, section 3): Bun's handler is recorded and
   chained to for every fault that is not one of ours, instead of replacing ours. ccios_patch.py requires
   the sigaction import for exactly this reason. mprotect() is interposed too,
   so a protection change made by anyone else to a pool page is never
   contradicted by the state kept here. */

#define MAX_JIT_REGIONS 16
#define MAX_TOUCHED     512      /* pages one thread writes per scope, counted */

enum { PAGE_RW = 0, PAGE_RX = 1, PAGE_FOREIGN = 2 };

typedef struct {
    uintptr_t start, end;
    size_t    npages;
    uint8_t  *state;             /* PAGE_*, authoritative while g_lock is held */
    uint16_t *writers;           /* threads in a write scope that wrote the page */
} jit_region_t;

typedef struct {
    int writing;
    int ntouched;
    struct { jit_region_t *r; size_t page; } touched[MAX_TOUCHED];
} writer_t;

static jit_region_t   g_regions[MAX_JIT_REGIONS];
static atomic_int     g_nregions;          /* only ever grows: readers need no lock */
static atomic_flag    g_lock = ATOMIC_FLAG_INIT;
static pthread_mutex_t g_register_lock = PTHREAD_MUTEX_INITIALIZER;
static size_t         g_page;
static pthread_key_t  g_writer_key;

static atomic_ulong   g_write_faults, g_exec_faults, g_waits, g_chained;

typedef void *(*mmap_fn)(void *, size_t, int, int, int, off_t);
typedef int   (*mprotect_fn)(void *, size_t, int);
typedef int   (*sigaction_fn)(int, const struct sigaction *, struct sigaction *);

static mmap_fn      real_mmap;
static mprotect_fn  real_mprotect;
static sigaction_fn real_sigaction;

/* Spin lock: taken inside a signal handler, where a mutex is not safe. Every
   critical section is a few assignments and at most one mprotect. */
static void jit_lock(void) {
    while (atomic_flag_test_and_set_explicit(&g_lock, memory_order_acquire))
        sched_yield();
}
static void jit_unlock(void) { atomic_flag_clear_explicit(&g_lock, memory_order_release); }

static jit_region_t *find_region(uintptr_t a, size_t *page) {
    int n = atomic_load(&g_nregions);
    for (int i = 0; i < n; i++) {
        jit_region_t *r = &g_regions[i];
        if (a >= r->start && a < r->end) {
            *page = (a - r->start) / g_page;
            return r;
        }
    }
    return NULL;
}

static uint8_t state_for_prot(int prot) {
    int wx = prot & (PROT_WRITE | PROT_EXEC);
    if ((prot & PROT_READ) && wx == PROT_WRITE) return PAGE_RW;
    if ((prot & PROT_READ) && wx == PROT_EXEC)  return PAGE_RX;
    return PAGE_FOREIGN;                         /* PROT_NONE guard pages and the like */
}

/* Somebody (JSC, or the region being created) set [a, a+len) to prot. */
static void note_protection(uintptr_t a, size_t len, int prot) {
    int n = atomic_load(&g_nregions);
    for (int i = 0; i < n; i++) {
        jit_region_t *r = &g_regions[i];
        uintptr_t lo = a > r->start ? a : r->start;
        uintptr_t hi = a + len < r->end ? a + len : r->end;
        if (lo >= hi) continue;
        size_t first = (lo - r->start) / g_page;
        size_t last  = (hi - r->start + g_page - 1) / g_page;
        uint8_t st = state_for_prot(prot);
        jit_lock();
        for (size_t p = first; p < last; p++) r->state[p] = st;
        jit_unlock();
    }
}

static void register_region(void *p, size_t len, int prot) {
    size_t dummy;
    if (find_region((uintptr_t)p, &dummy) != NULL) {   /* MAP_FIXED inside the pool */
        note_protection((uintptr_t)p, len, prot);
        return;
    }
    pthread_mutex_lock(&g_register_lock);
    int n = atomic_load(&g_nregions);
    if (n >= MAX_JIT_REGIONS) {
        pthread_mutex_unlock(&g_register_lock);
        fprintf(stderr, "[ccios] fatal: more than %d JIT regions\n", MAX_JIT_REGIONS);
        abort();
    }
    jit_region_t *r = &g_regions[n];
    r->start  = (uintptr_t)p;
    r->npages = (len + g_page - 1) / g_page;
    r->end    = r->start + r->npages * g_page;
    size_t bytes = r->npages * (sizeof *r->state + sizeof *r->writers);
    void *mem = real_mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mem == MAP_FAILED) {
        pthread_mutex_unlock(&g_register_lock);
        fprintf(stderr, "[ccios] fatal: cannot allocate JIT page state\n");
        abort();
    }
    r->state   = mem;
    r->writers = (uint16_t *)((uint8_t *)mem + r->npages);
    memset(r->state, state_for_prot(prot), r->npages);
    atomic_store(&g_nregions, n + 1);              /* publish only once complete */
    pthread_mutex_unlock(&g_register_lock);

    if (debug_on())
        fprintf(stderr, "[ccios] JIT pool %p len=%zu, %zu pages of %zu bytes\n",
                p, len, r->npages, g_page);
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset) {
    if ((flags & MAP_JIT) == 0) {
        void *p = real_mmap(addr, len, prot, flags, fd, offset);
        if (p != MAP_FAILED && (flags & MAP_FIXED)) note_protection((uintptr_t)p, len, prot);
        return p;
    }
    /* JSC's executable pool asks for RWX. Start it RW: the first execution of
       each page faults and makes it RX. */
    flags &= ~MAP_JIT;
    int initial = prot;
    if ((prot & PROT_WRITE) && (prot & PROT_EXEC)) initial = prot & ~PROT_EXEC;
    void *p = real_mmap(addr, len, initial, flags, fd, offset);
    if (p != MAP_FAILED) register_region(p, len, initial);
    return p;
}

int mprotect(void *addr, size_t len, int prot) {
    int rc = real_mprotect(addr, len, prot);
    if (rc == 0) note_protection((uintptr_t)addr, len, prot);
    return rc;
}

static writer_t *current_writer(void) { return pthread_getspecific(g_writer_key); }

static void check_handlers(void);

/* enabled=0: "about to write"; enabled=1: "done, must be executable again". */
void pthread_jit_write_protect_np(int enabled) {
    writer_t *w = current_writer();
    if (!enabled) {
        if (w == NULL) {
            /* Allocated here, in normal context, so the fault handler only ever
               reads it. */
            w = calloc(1, sizeof *w);
            if (w == NULL) abort();
            pthread_setspecific(g_writer_key, w);
        }
        w->writing = 1;
        check_handlers();
        return;
    }
    if (w == NULL || !w->writing) return;
    if (w->ntouched > 0) {
        jit_lock();
        for (int i = 0; i < w->ntouched; i++)
            w->touched[i].r->writers[w->touched[i].page]--;
        jit_unlock();
        w->ntouched = 0;
    }
    w->writing = 0;
}

int pthread_jit_write_protect_supported_np(void) { return 1; }

/* A write to page `page`. Only a thread inside a write scope may make one;
   anything else is a real bug and goes to Bun's handler. */
static int repair_write(jit_region_t *r, size_t page) {
    writer_t *w = current_writer();
    if (w == NULL || !w->writing) return 0;
    atomic_fetch_add(&g_write_faults, 1);
    int ok = 1;
    jit_lock();
    if (r->state[page] == PAGE_FOREIGN) {
        ok = 0;
    } else {
        int seen = 0;
        for (int i = 0; i < w->ntouched; i++)
            if (w->touched[i].r == r && w->touched[i].page == page) { seen = 1; break; }
        /* Past MAX_TOUCHED the page goes uncounted. Still correct -- every access
           faults and is repaired -- just liable to bounce between a writer and an
           executor until the scope ends. */
        if (!seen && w->ntouched < MAX_TOUCHED) {
            w->touched[w->ntouched].r    = r;
            w->touched[w->ntouched].page = page;
            w->ntouched++;
            r->writers[page]++;
        }
        if (r->state[page] == PAGE_RX) {
            if (real_mprotect((void *)(r->start + page * g_page), g_page,
                              PROT_READ | PROT_WRITE) == 0)
                r->state[page] = PAGE_RW;
            else
                ok = 0;
        }
    }
    jit_unlock();
    return ok;
}

/* An execution of page `page`. Waits out any thread still writing it. */
static int repair_exec(jit_region_t *r, size_t page) {
    writer_t *w = current_writer();
    /* On macOS a thread inside its own write scope cannot execute JIT code
       either. Crash the same way rather than wait on ourselves forever. */
    if (w != NULL && w->writing) return 0;
    atomic_fetch_add(&g_exec_faults, 1);
    for (unsigned spins = 0;; spins++) {
        jit_lock();
        if (r->state[page] == PAGE_FOREIGN) { jit_unlock(); return 0; }
        if (r->writers[page] == 0) {
            int ok = 1;
            if (r->state[page] == PAGE_RW) {
                if (real_mprotect((void *)(r->start + page * g_page), g_page,
                                  PROT_READ | PROT_EXEC) == 0)
                    r->state[page] = PAGE_RX;
                else
                    ok = 0;
            }
            jit_unlock();
            return ok;
        }
        jit_unlock();
        if (spins == 0) atomic_fetch_add(&g_waits, 1);
        if (spins < 64) {
            sched_yield();
        } else {
            struct timespec ts = { 0, 50000 };   /* 50 us */
            nanosleep(&ts, NULL);
        }
    }
}

/* ---- signal handler ordering ------------------------------------------ */

static struct sigaction g_app[3];          /* SIGBUS, SIGSEGV, SIGILL as the program set them */
static atomic_flag      g_sig_lock = ATOMIC_FLAG_INIT;

static struct sigaction *app_slot(int sig) {
    return sig == SIGBUS ? &g_app[0] : sig == SIGSEGV ? &g_app[1]
         : sig == SIGILL ? &g_app[2] : NULL;
}
static void sig_lock(void)   { while (atomic_flag_test_and_set(&g_sig_lock)) sched_yield(); }
static void sig_unlock(void) { atomic_flag_clear(&g_sig_lock); }

static void chain(int sig, siginfo_t *si, void *ctx) {
    atomic_fetch_add(&g_chained, 1);
    sig_lock();
    struct sigaction app = *app_slot(sig);
    if (app.sa_flags & SA_RESETHAND) {
        memset(app_slot(sig), 0, sizeof app);
        app_slot(sig)->sa_handler = SIG_DFL;
    }
    sig_unlock();

    int has_info = (app.sa_flags & SA_SIGINFO) != 0;
    if ((has_info && app.sa_sigaction != NULL) ||
        (!has_info && app.sa_handler != SIG_DFL && app.sa_handler != SIG_IGN)) {
        sigset_t saved;
        pthread_sigmask(SIG_BLOCK, &app.sa_mask, &saved);
        if (has_info) app.sa_sigaction(sig, si, ctx);
        else          app.sa_handler(sig);
        pthread_sigmask(SIG_SETMASK, &saved, NULL);
        return;
    }
    /* Default action: restore it for real and raise the signal again. It stays
       pending until this handler returns, then kills the process the way it
       would have without the shim. Returning alone is not enough: a signal
       that was sent rather than faulted -- kill(), or Node resetting its
       handler and calling raise() -- would never arrive a second time. */
    struct sigaction dfl;
    memset(&dfl, 0, sizeof dfl);
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    real_sigaction(sig, &dfl, NULL);
    raise(sig);
}

static void jit_fault(int sig, siginfo_t *si, void *ctx) {
    uintptr_t far = (uintptr_t)si->si_addr;
    size_t page;
    jit_region_t *r = find_region(far, &page);
    if (r != NULL) {
        ucontext_t *uc = ctx;
        uint32_t esr = uc->uc_mcontext->__es.__esr;
        uintptr_t pc = (uintptr_t)uc->uc_mcontext->__ss.__pc;
        unsigned ec = esr >> 26;
        int exec  = ec == 0x20 || ec == 0x21 || pc == far;          /* instruction abort */
        int write = !exec && (ec == 0x24 || ec == 0x25) && (esr & (1u << 6));  /* data abort, WnR */
        if (exec  && repair_exec(r, page))  return;
        if (write && repair_write(r, page)) return;
    }
    chain(sig, si, ctx);
}

/* ---- 3. ARMv8.4 load-acquire/store-release on older CPUs ---------------- */

/* The macOS build targets M1-class CPUs, and its atomics include LDAPUR and
   STLUR (FEAT_LRCPC2, ARMv8.4): a load-acquire or store-release with an
   immediate offset. A12-class chips are ARMv8.3 and trap on them with SIGILL,
   in the binary's own static initializers. Each is LDAR/STLR plus an address
   offset, so perform exactly that here and step over the instruction. LDAR is
   RCsc where LDAPUR is RCpc -- stronger, never weaker. */

static atomic_ulong g_emulated;

static uint64_t *gpr(arm_thread_state64_t *ss, unsigned r) {
    if (r < 29)  return &ss->__x[r];
    if (r == 29) return &ss->__fp;
    if (r == 30) return &ss->__lr;
    return NULL;                               /* 31: sp as a base, zr as data */
}

static int emulate_lrcpc2(ucontext_t *uc) {
    arm_thread_state64_t *ss = &uc->uc_mcontext->__ss;
    uint32_t insn = *(const uint32_t *)(uintptr_t)ss->__pc;
    /* size:2 011001 opc:2 0 imm9 00 Rn Rt */
    if ((insn & 0x3f200c00) != 0x19000000) return 0;
    unsigned size = insn >> 30, opc = (insn >> 22) & 3;
    unsigned rn = (insn >> 5) & 31, rt = insn & 31;
    if ((opc == 2 && size == 3) || (opc == 3 && size >= 2)) return 0;   /* unallocated */
    int64_t imm = (insn >> 12) & 0x1ff;
    if (imm & 0x100) imm -= 0x200;
    uintptr_t a = (uintptr_t)((rn == 31 ? ss->__sp : *gpr(ss, rn)) + imm);
    uint64_t *reg = gpr(ss, rt);

    if (opc == 0) {                            /* STLUR{B,H,} */
        uint64_t v = reg ? *reg : 0;
        switch (size) {
        case 0: __atomic_store_n((uint8_t  *)a, (uint8_t)v,  __ATOMIC_RELEASE); break;
        case 1: __atomic_store_n((uint16_t *)a, (uint16_t)v, __ATOMIC_RELEASE); break;
        case 2: __atomic_store_n((uint32_t *)a, (uint32_t)v, __ATOMIC_RELEASE); break;
        case 3: __atomic_store_n((uint64_t *)a, v,           __ATOMIC_RELEASE); break;
        }
    } else {                                   /* LDAPUR{B,H,,SB,SH,SW} */
        uint64_t v = 0;
        switch (size) {
        case 0: v = __atomic_load_n((uint8_t  *)a, __ATOMIC_ACQUIRE); break;
        case 1: v = __atomic_load_n((uint16_t *)a, __ATOMIC_ACQUIRE); break;
        case 2: v = __atomic_load_n((uint32_t *)a, __ATOMIC_ACQUIRE); break;
        case 3: v = __atomic_load_n((uint64_t *)a, __ATOMIC_ACQUIRE); break;
        }
        unsigned bits = 8u << size;
        if (opc == 2)                          /* sign-extend into Xt */
            v = (uint64_t)((int64_t)(v << (64 - bits)) >> (64 - bits));
        else if (opc == 3)                     /* sign-extend into Wt */
            v = (uint32_t)((int32_t)((uint32_t)v << (32 - bits)) >> (32 - bits));
        if (reg) *reg = v;
    }
    ss->__pc += 4;
    atomic_fetch_add_explicit(&g_emulated, 1, memory_order_relaxed);
    return 1;
}

/* The same build also uses SHA3's three-way XORs and DotProd's byte dot
   products wherever the compiler found the pattern -- constant-time table
   selects in the TLS code, for one -- SHA512's four round instructions, which
   BoringSSL built for an M1 uses without asking the CPU, and, in Claude Code,
   a DIT/SB pair around a constant-time compare. All are plain arithmetic on
   the vector registers the signal context carries. */

typedef __uint128_t v128;

static inline uint64_t ror64(uint64_t x, unsigned r) { return (x >> r) | (x << (64 - r)); }

/* SHA512H, SHA512H2, SHA512SU0, SHA512SU1 -- the ARM ARM pseudocode, with
   [0] the low doubleword and [1] the high one. */
static int emulate_sha512(v128 *v, uint32_t insn) {
    unsigned rd = insn & 31, rn = (insn >> 5) & 31, rm = (insn >> 16) & 31;
    uint64_t x[2], y[2], w[2], t[2];
    memcpy(x, &v[rn], 16); memcpy(y, &v[rm], 16); memcpy(w, &v[rd], 16);

    if ((insn & 0xfffffc00) == 0xcec08000) {                     /* SHA512SU0 Vd, Vn */
        t[0] = w[0] + (ror64(w[1], 1) ^ ror64(w[1], 8) ^ (w[1] >> 7));
        t[1] = w[1] + (ror64(x[0], 1) ^ ror64(x[0], 8) ^ (x[0] >> 7));
    } else if ((insn & 0xffe0fc00) == 0xce608800) {              /* SHA512SU1 Vd, Vn, Vm */
        t[1] = w[1] + (ror64(x[1], 19) ^ ror64(x[1], 61) ^ (x[1] >> 6)) + y[1];
        t[0] = w[0] + (ror64(x[0], 19) ^ ror64(x[0], 61) ^ (x[0] >> 6)) + y[0];
    } else if ((insn & 0xffe0fc00) == 0xce608000) {              /* SHA512H Qd, Qn, Vm */
        uint64_t s1 = ror64(y[1], 14) ^ ror64(y[1], 18) ^ ror64(y[1], 41);
        t[1] = ((y[1] & x[0]) ^ (~y[1] & x[1])) + s1 + w[1];
        uint64_t tmp = t[1] + y[0];
        s1 = ror64(tmp, 14) ^ ror64(tmp, 18) ^ ror64(tmp, 41);
        t[0] = ((tmp & y[1]) ^ (~tmp & x[0])) + s1 + w[0];
    } else if ((insn & 0xffe0fc00) == 0xce608400) {              /* SHA512H2 Qd, Qn, Vm */
        uint64_t s0 = ror64(y[0], 28) ^ ror64(y[0], 34) ^ ror64(y[0], 39);
        t[1] = ((x[0] & y[1]) ^ (x[0] & y[0]) ^ (y[1] & y[0])) + s0 + w[1];
        s0 = ror64(t[1], 28) ^ ror64(t[1], 34) ^ ror64(t[1], 39);
        t[0] = ((t[1] & y[0]) ^ (t[1] & y[1]) ^ (y[1] & y[0])) + s0 + w[0];
    } else {
        return 0;
    }
    memcpy(&v[rd], t, 16);
    return 1;
}

static int emulate_simd(ucontext_t *uc, uint32_t insn) {
    v128 *v = uc->uc_mcontext->__ns.__v;
    unsigned rd = insn & 31, rn = (insn >> 5) & 31, rm = (insn >> 16) & 31, ra = (insn >> 10) & 31;

    if (emulate_sha512(v, insn)) return 1;
    if ((insn & 0xffe08000) == 0xce000000) { v[rd] = v[rn] ^ v[rm] ^ v[ra];    return 1; }  /* EOR3 */
    if ((insn & 0xffe08000) == 0xce200000) { v[rd] = v[rn] ^ (v[rm] & ~v[ra]); return 1; }  /* BCAX */
    if ((insn & 0xffe0fc00) == 0xce608c00 ||                                                 /* RAX1 */
        (insn & 0xffe00000) == 0xce800000) {                                                 /* XAR  */
        int xar = (insn >> 23) & 1;
        unsigned rot = xar ? (insn >> 10) & 63 : 0;
        uint64_t n[2], m[2], d[2];
        memcpy(n, &v[rn], 16); memcpy(m, &v[rm], 16);
        for (int i = 0; i < 2; i++) {
            if (xar) { uint64_t x = n[i] ^ m[i]; d[i] = rot ? (x >> rot) | (x << (64 - rot)) : x; }
            else     d[i] = n[i] ^ ((m[i] << 1) | (m[i] >> 63));
        }
        memcpy(&v[rd], d, 16);
        return 1;
    }

    /* Q (bit 30) and U (bit 29) are operands, not part of the opcode. */
    int vec  = (insn & 0x9fe0fc00) == 0x0e809400;                                            /* SDOT/UDOT */
    int elem = (insn & 0x9fc0f400) == 0x0f80e000;                                            /* ...[index] */
    if (vec || elem) {
        int q = (insn >> 30) & 1, u = (insn >> 29) & 1;
        unsigned idx = 0;
        if (elem) {
            idx = ((insn >> 11) & 1) << 1 | ((insn >> 21) & 1);   /* H:L */
            rm  = (insn >> 16) & 31;                               /* M:Rm */
        }
        uint8_t n[16], m[16];
        uint32_t d[4];
        memcpy(n, &v[rn], 16); memcpy(m, &v[rm], 16); memcpy(d, &v[rd], 16);
        for (int i = 0; i < (q ? 4 : 2); i++) {
            int32_t s = 0;
            for (int j = 0; j < 4; j++) {
                unsigned a = 4 * i + j, b = elem ? 4 * idx + j : a;
                s += u ? (int32_t)n[a] * (int32_t)m[b]
                       : (int32_t)(int8_t)n[a] * (int32_t)(int8_t)m[b];
            }
            d[i] += (uint32_t)s;
        }
        if (!q) d[2] = d[3] = 0;
        memcpy(&v[rd], d, 16);
        return 1;
    }
    return 0;
}

static int emulate_system(ucontext_t *uc, uint32_t insn) {
    if (insn == 0xd50330ff) return 1;                           /* SB: the trap already serialised */
    if ((insn & 0xfffff0ff) == 0xd503405f) return 1;            /* MSR DIT, #imm: no DIT here */
    if ((insn & 0xffffffe0) == 0xd51b42a0) return 1;            /* MSR DIT, Xt */
    if ((insn & 0xffffffe0) == 0xd53b42a0) {                    /* MRS Xt, DIT: reads as off */
        uint64_t *reg = gpr(&uc->uc_mcontext->__ss, insn & 31);
        if (reg) *reg = 0;
        return 1;
    }
    return 0;
}

static void ill_fault(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    if (emulate_lrcpc2(uc)) return;
    uint32_t insn = *(const uint32_t *)(uintptr_t)uc->uc_mcontext->__ss.__pc;
    if (emulate_simd(uc, insn) || emulate_system(uc, insn)) {
        uc->uc_mcontext->__ss.__pc += 4;
        atomic_fetch_add_explicit(&g_emulated, 1, memory_order_relaxed);
        return;
    }
    chain(sig, si, ctx);
}

static void install_handler(int sig) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = sig == SIGILL ? ill_fault : jit_fault;
    /* SA_ONSTACK so that a chained crash handler still gets the alternate stack
       it asked for, which is what makes a stack overflow reportable. */
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    real_sigaction(sig, &sa, NULL);
}

/* Belt and braces for the interposition: if a handler was installed past it
   (a libSystem-internal call, say), adopt that one as the chained handler and
   put ours back in front. Cheap enough to run on the first write scopes and
   then every few thousand. */
static void check_handlers(void) {
    static atomic_ulong calls;
    unsigned long c = atomic_fetch_add(&calls, 1);
    if (c > 16 && (c & 4095) != 0) return;
    static const int sigs[3] = { SIGBUS, SIGSEGV, SIGILL };
    for (int i = 0; i < 3; i++) {
        struct sigaction cur;
        if (real_sigaction(sigs[i], NULL, &cur) != 0) continue;
        if ((cur.sa_flags & SA_SIGINFO) &&
            cur.sa_sigaction == (sigs[i] == SIGILL ? ill_fault : jit_fault)) continue;
        sig_lock();
        *app_slot(sigs[i]) = cur;
        sig_unlock();
        install_handler(sigs[i]);
        if (debug_on()) fprintf(stderr, "[ccios] reinstalled fault handler for %d\n", sigs[i]);
    }
}

int sigaction(int sig, const struct sigaction *act, struct sigaction *oact) {
    struct sigaction *slot = app_slot(sig);
    if (slot == NULL) return real_sigaction(sig, act, oact);
    sig_lock();
    if (oact != NULL) *oact = *slot;
    if (act != NULL)  *slot = *act;
    sig_unlock();
    return 0;
}

void (*signal(int sig, void (*func)(int)))(int) {
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = func;
    sa.sa_flags = SA_RESTART;                    /* BSD signal() semantics */
    sigemptyset(&sa.sa_mask);
    if (sigaction(sig, &sa, &old) != 0) return SIG_ERR;
    return (old.sa_flags & SA_SIGINFO) ? (void (*)(int))(uintptr_t)old.sa_sigaction
                                       : old.sa_handler;
}

static void report_counters(void) {
    fprintf(stderr, "[ccios] JIT faults: write %lu, exec %lu, waited %lu, chained %lu; emulated %lu\n",
            atomic_load(&g_write_faults), atomic_load(&g_exec_faults),
            atomic_load(&g_waits), atomic_load(&g_chained), atomic_load(&g_emulated));
}

__attribute__((constructor))
static void ccios_init(void) {
    real_mmap      = (mmap_fn)real_sym("mmap", (void *)mmap);
    real_mprotect  = (mprotect_fn)real_sym("mprotect", (void *)mprotect);
    real_sigaction = (sigaction_fn)real_sym("sigaction", (void *)sigaction);
    g_page = (size_t)sysconf(_SC_PAGESIZE);
    if (pthread_key_create(&g_writer_key, free) != 0) abort();

    real_sigaction(SIGBUS,  NULL, &g_app[0]);
    real_sigaction(SIGSEGV, NULL, &g_app[1]);
    real_sigaction(SIGILL,  NULL, &g_app[2]);
    install_handler(SIGBUS);
    install_handler(SIGSEGV);
    install_handler(SIGILL);

    if (debug_on()) atexit(report_counters);
}

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
