/*
 * f1_fork_mt.c - F: fork() from a multi-threaded process.
 *
 * Questions answered here (one line of evidence each, see the PASS/FAIL/INFO lines):
 *   - what exists in the child (threads, Mach ports, mach_task_self, pthread state, locks held by other threads)
 *   - is MAP_SHARED|MAP_ANON memory really shared after fork, is private memory copy-on-write (anon and file)
 *   - minherit(VM_INHERIT_SHARE / NONE) as a way to change what fork does with a region
 *   - cost of a copy-on-write fault after fork
 *   - MAP_JIT regions in the child (execution and modification)
 *   - stability of fork() against concurrent malloc in other threads (storm of 300 forks)
 *   - vfork semantics
 */
#include "fg_util.h"
#include <dispatch/dispatch.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach_vm.h>
#include <os/lock.h>
#include <servers/bootstrap.h>

/* ---- state shared by the helper threads ------------------------------------ */
static pthread_mutex_t held_mutex = PTHREAD_MUTEX_INITIALIZER;
static os_unfair_lock held_unfair = OS_UNFAIR_LOCK_INIT;
static pthread_key_t tsd_key;
static __thread int tls_var;
static FILE *held_file;
static _Atomic int stop_workers;
static _Atomic uint64_t spin_counter, malloc_counter;
static int pipe_a[2], pipe_b[2], pipe_c[2];
static mach_port_t parent_port = MACH_PORT_NULL;
static pthread_t workers[5];
static int nworkers;
static uint64_t parent_tid;
static pthread_t parent_forker;
static int parent_pid;
static mach_port_t parent_mach_task, parent_mach_thread;
static int atfork_prepare_n, atfork_parent_n, atfork_child_n;
static volatile sig_atomic_t usr1_seen, usr2_seen;
static int gcd_warm;            /* the parent has already used libdispatch before this fork */
static int crash_tests;         /* FG_CRASH_TESTS=1 enables tests that crash a child (the OS writes a crash report for each) */

#define TSD_MAGIC ((void *)0x5a5a1234)

static void at_prepare(void) { atfork_prepare_n++; }
static void at_parent(void) { atfork_parent_n++; }
static void at_child(void) { atfork_child_n++; }
static void usr1_handler(int s) { usr1_seen++; }
static void usr2_handler(int s) { usr2_seen++; }

static void *w_spin(void *a)
{
    while (!atomic_load(&stop_workers)) atomic_fetch_add_explicit(&spin_counter, 1, memory_order_relaxed);
    return NULL;
}

static void *w_malloc(void *a)
{
    unsigned seed = 1;
    while (!atomic_load(&stop_workers)) {
        size_t sz = 16 + (rand_r(&seed) % 8192);
        void *p = malloc(sz);
        if (p) { memset(p, 0xa5, sz < 64 ? sz : 64); free(p); }
        atomic_fetch_add_explicit(&malloc_counter, 1, memory_order_relaxed);
    }
    return NULL;
}

/* holds a pthread mutex and an os_unfair_lock, then blocks in read() */
static void *w_holder(void *a)
{
    char c;
    pthread_mutex_lock(&held_mutex);
    os_unfair_lock_lock(&held_unfair);
    (void)!read(pipe_a[0], &c, 1);
    os_unfair_lock_unlock(&held_unfair);
    pthread_mutex_unlock(&held_mutex);
    return NULL;
}

/* holds the stdio lock of held_file */
static void *w_stdio(void *a)
{
    char c;
    flockfile(held_file);
    (void)!read(pipe_b[0], &c, 1);
    funlockfile(held_file);
    return NULL;
}

/* plain thread blocked in a system call */
static void *w_blocked(void *a)
{
    char c;
    (void)!read(pipe_c[0], &c, 1);
    return NULL;
}

/* ---- nested probes executed in a grandchild with a deadline ----------------- */
static int n_mutex(void *a) { pthread_mutex_lock(&held_mutex); return 0; }
static int n_unfair(void *a) { os_unfair_lock_lock(&held_unfair); return 0; }
static int n_file(void *a) { fputs("x", held_file); fflush(held_file); return 0; }
static int n_dispatch(void *a)
{
    dispatch_semaphore_t s = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{ dispatch_semaphore_signal(s); });
    long r = dispatch_semaphore_wait(s, dispatch_time(DISPATCH_TIME_NOW, 1500ll * NSEC_PER_MSEC));
    return r == 0 ? 0 : 3;
}
static int n_pthread_kill(void *a)
{
    int r = pthread_kill(workers[0], 0);
    dp_emit("INFO", "F1.child.nested", "pthread_kill(worker thread of the parent, 0) returned %s", ERRS(r));
    return 0;
}
static void *noop_thread(void *a) { return a; }
static int n_pthread_create(void *a)
{
    pthread_t t;
    int r = pthread_create(&t, NULL, noop_thread, (void *)7);
    if (r) return 2;
    void *rv = NULL;
    pthread_join(t, &rv);
    return rv == (void *)7 ? 0 : 3;
}

static void nested(const char *label, fg_child_fn fn, unsigned ms)
{
    int st = 0;
    char b[160];
    int rc = fg_run_in_child(fn, NULL, ms, &st);
    INFO("F1.child.nested", "%s: grandchild %s", label, fg_outcome_str(rc, st, b, sizeof b));
}

/* ---- child probe: what exists after fork() ---------------------------------- */
struct probe_arg { int from_secondary; };

static int child_probe(void *argp)
{
    struct probe_arg *pa = argp;
    int bad = 0;
    const char *w = pa->from_secondary ? "fork from a secondary thread" : "fork from the main thread";
    uint64_t tid = 0;
    pthread_threadid_np(NULL, &tid);
    INFO("F1.child.ident", "%s: pid=%d (parent %d) ppid=%d pthread_self==forker pthread_t: %s, pthread_main_np=%d, thread id=%llu (forker had %llu)",
         w, getpid(), parent_pid, getppid(), pthread_self() == parent_forker ? "yes" : "no", pthread_main_np(),
         (unsigned long long)tid, (unsigned long long)parent_tid);
    INFO("F1.child.ident", "atfork handlers run: prepare=%d parent=%d child=%d (counters as seen in the child)",
         atfork_prepare_n, atfork_parent_n, atfork_child_n);

    /* threads */
    thread_act_array_t th = NULL;
    mach_msg_type_number_t n = 0;
    kern_return_t kr = task_threads(mach_task_self(), &th, &n);
    CHECK(kr == KERN_SUCCESS && n == 1, "F1.child.threads", "child task has exactly 1 thread (task_threads kr=%d count=%u)", kr, n);
    if (!(kr == KERN_SUCCESS && n == 1)) bad++;
    uint64_t s0 = atomic_load(&spin_counter), m0 = atomic_load(&malloc_counter);
    usleep(40000);
    uint64_t s1 = atomic_load(&spin_counter), m1 = atomic_load(&malloc_counter);
    CHECK(s0 == s1 && m0 == m1, "F1.child.threads", "the parent's spinning and malloc threads do not run in the child (spin %llu->%llu, malloc %llu->%llu)",
          (unsigned long long)s0, (unsigned long long)s1, (unsigned long long)m0, (unsigned long long)m1);

    /* pthread state */
    void *tv = pthread_getspecific(tsd_key);
    CHECK(tv == TSD_MAGIC, "F1.child.pthread", "pthread_getspecific value of the forking thread survives (%p)", tv);
    CHECK(tls_var == 77, "F1.child.pthread", "__thread variable of the forking thread survives (%d)", tls_var);
    int rc = pthread_mutex_trylock(&held_mutex);
    CHECK(rc == EBUSY, "F1.child.locks", "pthread mutex held by a thread that does not exist in the child stays locked (trylock=%s)", ERRS(rc));
    bool got = os_unfair_lock_trylock(&held_unfair);
    CHECK(!got, "F1.child.locks", "os_unfair_lock held by a vanished thread stays locked (trylock got=%d)", (int)got);

    /* Mach */
    mach_port_type_t pt = 0;
    kr = mach_port_type(mach_task_self(), parent_port, &pt);
    mach_port_context_t pctx = 0;
    int same_port = 0;
    if (kr == KERN_SUCCESS && (pt & MACH_PORT_TYPE_RECEIVE) && mach_port_get_context(mach_task_self(), parent_port, &pctx) == KERN_SUCCESS) same_port = (pctx == 0x5a17c0de5a17c0deull);
    CHECK(!same_port, "F1.child.mach", "the receive+send right created by the parent is NOT inherited (name 0x%x in the child: kr=%d %s, type=0x%x, %s)", parent_port, kr, mach_error_string(kr),
          kr == KERN_SUCCESS ? pt : 0, same_port ? "its context is the parent's marker" : "no port with the parent's context marker");
    int pidt = -1;
    kr = pid_for_task(mach_task_self(), &pidt);
    INFO("F1.child.mach", "mach_task_self() parent=0x%x child=0x%x ; mach_thread_self() parent=0x%x child=0x%x ; pid_for_task kr=%d pid=%d (getpid=%d)",
         parent_mach_task, mach_task_self(), parent_mach_thread, mach_thread_self(), kr, pidt, getpid());
    CHECK(kr == KERN_SUCCESS && pidt == getpid(), "F1.child.mach", "mach_task_self() in the child designates the child task, not the parent");
    mach_port_t svc = MACH_PORT_NULL;
    kr = bootstrap_look_up(bootstrap_port, "com.apple.system.notification_center", &svc);
    INFO("F1.child.mach", "bootstrap_look_up of a system service from the child: kr=%d (%s) port=0x%x (bootstrap_port=0x%x)", kr, mach_error_string(kr), svc, bootstrap_port);
    {
        exception_mask_t masks[EXC_TYPES_COUNT];
        mach_msg_type_number_t cnt = EXC_TYPES_COUNT;
        mach_port_t ports[EXC_TYPES_COUNT];
        exception_behavior_t beh[EXC_TYPES_COUNT];
        thread_state_flavor_t fl[EXC_TYPES_COUNT];
        kr = task_get_exception_ports(mach_task_self(), EXC_MASK_ALL, masks, &cnt, ports, beh, fl);
        INFO("F1.child.mach", "task exception ports in the child: kr=%d entries=%u", kr, cnt);
    }

    /* signals and timers */
    struct sigaction sa;
    sigaction(SIGUSR1, NULL, &sa);
    CHECK(sa.sa_handler == usr1_handler, "F1.child.signals", "signal dispositions are inherited (SIGUSR1 handler kept)");
    sigset_t cur, pend;
    pthread_sigmask(SIG_BLOCK, NULL, &cur);
    CHECK(sigismember(&cur, SIGUSR2) == 1, "F1.child.signals", "signal mask of the forking thread is inherited (SIGUSR2 blocked)");
    sigpending(&pend);
    CHECK(sigismember(&pend, SIGUSR2) == 0, "F1.child.signals", "signals pending in the parent are not pending in the child");
    unsigned left = alarm(0);
    CHECK(left == 0, "F1.child.signals", "an armed alarm (the parent's watchdog) is not inherited (alarm(0) returned %u)", left);
    dp_watchdog(30);

    /* malloc and thread creation in the child */
    uint64_t t0 = dp_ns();
    for (int i = 0; i < 20000; i++) {
        void *p = malloc(16 + (i % 4096));
        memset(p, i, 16);
        __asm__ volatile("" : : "r"(p) : "memory");   /* keep the allocation, the optimizer would drop malloc/free otherwise */
        free(p);
    }
    INFO("F1.child.malloc", "20000 malloc/free pairs in the child took %.2f ms", (double)(dp_ns() - t0) / 1e6);

    /* things that may hang or crash are tried in a grandchild with a deadline */
    nested("pthread_create + join in a child of the multi-threaded parent", n_pthread_create, 3000);
    nested("pthread_mutex_lock on a mutex held by a vanished thread", n_mutex, 700);
    if (crash_tests)
        nested("os_unfair_lock_lock on a lock held by a vanished thread", n_unfair, 3000);
    else
        SKIP("F1.child.nested", "os_unfair_lock_lock on a lock held by a vanished thread not run (it aborts the child and the OS writes a crash report); see evidence/darwin-process/os-crash-reports and the run1 log; set FG_CRASH_TESTS=1 to run it");
    nested("fputs on a FILE whose stdio lock is held by a vanished thread", n_file, 700);
    if (!gcd_warm || crash_tests) {
        char lbl[128];
        snprintf(lbl, sizeof lbl, "libdispatch (parent %s used GCD before fork): dispatch_async to a global queue, wait 1.5 s", gcd_warm ? "had already" : "had not");
        nested(lbl, n_dispatch, 4000);
    } else {
        SKIP("F1.child.nested", "libdispatch when the parent already used GCD: not run by default (the child dies with SIGSEGV and the OS writes a crash report; result kept in evidence f1_fork_mt.run2-gcd-warm-crash.log); FG_CRASH_TESTS=1 runs it");
    }
    nested("pthread_kill(parent's worker pthread_t, 0)", n_pthread_kill, 1500);
    return bad ? 1 : 0;
}

static void run_probe(int from_secondary)
{
    struct probe_arg pa = { from_secondary };
    char b[160];
    int st = 0;
    parent_forker = pthread_self();
    pthread_threadid_np(NULL, &parent_tid);
    parent_mach_task = mach_task_self();
    parent_mach_thread = mach_thread_self();
    pthread_setspecific(tsd_key, TSD_MAGIC);
    tls_var = 77;
    sigset_t blk, old;
    sigemptyset(&blk);
    sigaddset(&blk, SIGUSR2);
    pthread_sigmask(SIG_BLOCK, &blk, &old);
    pthread_kill(pthread_self(), SIGUSR2);   /* pending on this thread only */
    int p0 = atfork_prepare_n, q0 = atfork_parent_n;
    int rc = fg_run_in_child(child_probe, &pa, 25000, &st);
    INFO("F1.fork", "%s: child probe %s; parent saw atfork prepare +%d, parent +%d",
         from_secondary ? "secondary thread" : "main thread", fg_outcome_str(rc, st, b, sizeof b),
         atfork_prepare_n - p0, atfork_parent_n - q0);
    sigset_t pend;
    sigpending(&pend);
    INFO("F1.fork", "SIGUSR2 still pending in the parent thread after fork: %d", sigismember(&pend, SIGUSR2));
    pthread_sigmask(SIG_SETMASK, &old, NULL);   /* delivers the pending signal to usr2_handler */
}

static void *forker_thread(void *a) { run_probe(1); return NULL; }

/* ---- memory sharing and copy-on-write ------------------------------------------ */
typedef struct {
    _Atomic uint64_t ctr;
    _Atomic int child_ready, parent_go, child_done, parent_done;
    _Atomic uint64_t child_ns, parent_ns;
} ctl_t;
static ctl_t *ctl;

struct region { const char *name; unsigned char *p; size_t sz; int shared_expected; int in_child; };
#define NREG 6
static struct region regs[NREG];

#define P1 0x1111aaaa11110000ull
#define P2 0x2222bbbb22220000ull
#define P3 0x3333cccc33330000ull

static void wait_flag(_Atomic int *f)
{
    for (int i = 0; i < 20000 && !atomic_load(f); i++) usleep(500);
}

static int mem_child(void *argp)
{
    char id[] = "F1.mem";
    for (int i = 0; i < NREG; i++) {
        if (!regs[i].in_child) continue;
        size_t bad = fg_stride_check(regs[i].p, regs[i].sz, FG_PAGE, P1);
        CHECK(bad == 0, id, "child sees the parent's pre-fork contents of %s (%zu mismatching words)", regs[i].name, bad);
        fg_stride_fill(regs[i].p, regs[i].sz, FG_PAGE, P2);
    }
    atomic_store(&ctl->child_ready, 1);
    wait_flag(&ctl->parent_go);
    for (int i = 0; i < NREG; i++) {
        if (!regs[i].in_child) continue;
        size_t bad3 = fg_stride_check(regs[i].p, regs[i].sz, FG_PAGE, P3);
        size_t bad2 = fg_stride_check(regs[i].p, regs[i].sz, FG_PAGE, P2);
        if (regs[i].shared_expected)
            CHECK(bad3 == 0, id, "child sees the parent's post-fork write in %s (shared, %zu mismatches)", regs[i].name, bad3);
        else
            CHECK(bad2 == 0 && bad3 != 0, id, "child keeps its own copy of %s and does not see the parent's later write (private)", regs[i].name);
    }
    /* cross-process atomics on the shared region */
    uint64_t t0 = dp_ns();
    for (int i = 0; i < 2000000; i++) atomic_fetch_add(&ctl->ctr, 1);
    atomic_store(&ctl->child_ns, dp_ns() - t0);
    /* the child unmaps/protects its own view only */
    munmap(regs[0].p, regs[0].sz);
    mprotect(regs[1].p, regs[1].sz, PROT_NONE);
    atomic_store(&ctl->child_done, 1);
    return 0;
}

static int none_child(void *argp)
{
    unsigned char *p = argp;
    volatile unsigned char c = *p;
    (void)c;
    return 0;
}

/* 1 if addr lies inside a mapped region of this task (no fault taken) */
static int region_mapped(const void *addr)
{
    mach_vm_address_t a = (mach_vm_address_t)addr;
    mach_vm_size_t sz = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj = MACH_PORT_NULL;
    kern_return_t kr = mach_vm_region(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj);
    return kr == KERN_SUCCESS && a <= (mach_vm_address_t)addr;
}

static int mapped_child(void *argp)
{
    unsigned char **v = argp;
    int none_mapped = region_mapped(v[0]);
    int priv_mapped = region_mapped(v[1]);
    int share_mapped = region_mapped(v[2]);
    dp_emit("INFO", "F1.minherit", "region present in the child: VM_INHERIT_NONE region=%d, default private region=%d, VM_INHERIT_SHARE region=%d", none_mapped, priv_mapped, share_mapped);
    return (!none_mapped && priv_mapped && share_mapped) ? 0 : 1;
}

static void mem_tests(void)
{
    const size_t SZ = 64u << 20;
    ctl = mmap(NULL, FG_PAGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    if (ctl == MAP_FAILED) { FAIL("F1.mem", "cannot map the control page"); return; }
    memset(ctl, 0, sizeof *ctl);

    unsigned char *shared = mmap(NULL, SZ, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    unsigned char *priv = mmap(NULL, SZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    unsigned char *inh_share = mmap(NULL, SZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    unsigned char *inh_none = mmap(NULL, SZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    char *fa = fg_tmpname("f1a"), *fb = fg_tmpname("f1b");
    int fda = mkstemp(fa), fdb = mkstemp(fb);
    if (fda < 0 || fdb < 0) { FAIL("F1.mem", "mkstemp failed: %s", ERRS(errno)); return; }
    unlink(fa);
    unlink(fb);
    if (ftruncate(fda, SZ) || ftruncate(fdb, SZ)) { FAIL("F1.mem", "ftruncate failed: %s", ERRS(errno)); return; }
    unsigned char *fpriv = mmap(NULL, SZ, PROT_READ | PROT_WRITE, MAP_PRIVATE, fda, 0);
    unsigned char *fshared = mmap(NULL, SZ, PROT_READ | PROT_WRITE, MAP_SHARED, fdb, 0);
    if (shared == MAP_FAILED || priv == MAP_FAILED || inh_share == MAP_FAILED || inh_none == MAP_FAILED || fpriv == MAP_FAILED || fshared == MAP_FAILED) {
        FAIL("F1.mem", "mmap failed: %s", ERRS(errno));
        return;
    }
    int r1 = minherit(inh_share, SZ, VM_INHERIT_SHARE);
    int e1 = errno;
    int r2 = minherit(inh_none, SZ, VM_INHERIT_NONE);
    int e2 = errno;
    CHECK(r1 == 0, "F1.minherit", "minherit(VM_INHERIT_SHARE) on a private anonymous region: %s", r1 ? ERRS(e1) : "ok");
    CHECK(r2 == 0, "F1.minherit", "minherit(VM_INHERIT_NONE) on a private anonymous region: %s", r2 ? ERRS(e2) : "ok");

    regs[0] = (struct region){ "MAP_SHARED|MAP_ANON", shared, SZ, 1, 1 };
    regs[1] = (struct region){ "MAP_PRIVATE|MAP_ANON", priv, SZ, 0, 1 };
    regs[2] = (struct region){ "private anon + minherit(SHARE)", inh_share, SZ, 1, 1 };
    regs[3] = (struct region){ "private anon + minherit(NONE)", inh_none, SZ, 0, 0 };
    regs[4] = (struct region){ "MAP_PRIVATE file mapping", fpriv, SZ, 0, 1 };
    regs[5] = (struct region){ "MAP_SHARED file mapping", fshared, SZ, 1, 1 };
    for (int i = 0; i < NREG; i++) fg_stride_fill(regs[i].p, regs[i].sz, FG_PAGE, P1);

    pid_t pid = fork();
    if (pid < 0) { FAIL("F1.mem", "fork failed %s", ERRS(errno)); return; }
    if (pid == 0) {
        dp_role = "child";
        dp_watchdog(60);
        _exit(mem_child(NULL));
    }
    dp_track(pid);
    wait_flag(&ctl->child_ready);
    for (int i = 0; i < NREG; i++) {
        if (!regs[i].in_child) continue;
        size_t b2 = fg_stride_check(regs[i].p, regs[i].sz, FG_PAGE, P2);
        if (regs[i].shared_expected)
            CHECK(b2 == 0, "F1.mem", "parent sees the child's write in %s (%zu mismatches)", regs[i].name, b2);
        else
            CHECK(b2 != 0 && fg_stride_check(regs[i].p, regs[i].sz, FG_PAGE, P1) == 0, "F1.mem",
                  "parent does NOT see the child's write in %s (copy-on-write isolation)", regs[i].name);
    }
    {
        uint64_t w = 0;
        pread(fdb, &w, 8, 0);
        CHECK(w == (P2 ^ 0), "F1.mem", "MAP_SHARED file mapping: the file itself holds the child's write (word=0x%llx)", (unsigned long long)w);
        pread(fda, &w, 8, 0);
        CHECK(w == 0, "F1.mem", "MAP_PRIVATE file mapping: the file is untouched by either process (word=0x%llx)", (unsigned long long)w);
    }
    for (int i = 0; i < NREG; i++) fg_stride_fill(regs[i].p, regs[i].sz, FG_PAGE, P3);
    atomic_store(&ctl->parent_go, 1);
    uint64_t t0 = dp_ns();
    for (int i = 0; i < 2000000; i++) atomic_fetch_add(&ctl->ctr, 1);
    uint64_t pns = dp_ns() - t0;
    wait_flag(&ctl->child_done);
    int st = 0;
    dp_wait_timeout(pid, &st, 5000);
    uint64_t cv = atomic_load(&ctl->ctr);
    CHECK(cv == 4000000ull, "F1.mem", "2 x 2,000,000 atomic increments from two processes on MAP_SHARED|MAP_ANON memory give %llu (parent %.1f ms, child %.1f ms)",
          (unsigned long long)cv, (double)pns / 1e6, (double)atomic_load(&ctl->child_ns) / 1e6);
    {
        size_t b3 = fg_stride_check(shared, SZ, FG_PAGE, P3);
        CHECK(b3 == 0, "F1.mem", "child's munmap of its view of the shared region leaves the parent's view intact");
        priv[0] = 1;
        CHECK(1, "F1.mem", "child's mprotect(PROT_NONE) on its copy of a private region leaves the parent's region writable");
    }

    /* VM_INHERIT_NONE: the region does not exist in the child (checked with mach_vm_region, no fault) */
    {
        char b[160];
        unsigned char *v[3] = { inh_none, priv, inh_share };
        int s2 = 0;
        int rc = fg_run_in_child(mapped_child, v, 10000, &s2);
        CHECK(rc == FG_OK, "F1.minherit", "VM_INHERIT_NONE removes the region from the child, default regions stay, VM_INHERIT_SHARE regions stay (%s)", fg_outcome_str(rc, s2, b, sizeof b));
        if (crash_tests) {
            pid_t p = fork();
            if (p == 0) { dp_role = "child"; dp_watchdog(10); _exit(none_child(inh_none)); }
            dp_track(p);
            dp_wait_timeout(p, &s2, 5000);
            INFO("F1.minherit", "child touching a VM_INHERIT_NONE region: %s", dp_status_str(s2, b, sizeof b));
        }
    }

    /* unmap what we do not need any more */
    munmap(shared, SZ); munmap(priv, SZ); munmap(inh_share, SZ); munmap(inh_none, SZ); munmap(fpriv, SZ); munmap(fshared, SZ);
    close(fda); close(fdb);
    free(fa); free(fb);
}

/* ---- cost of a copy-on-write fault after fork ----------------------------------- */
static int cow_child_touch(void *argp)
{
    unsigned char *buf = argp;
    size_t pages = (256u << 20) / FG_PAGE;
    uint64_t t0 = dp_ns();
    for (size_t i = 0; i < pages; i++) buf[i * FG_PAGE] = 2;
    atomic_store(&ctl->child_ns, dp_ns() - t0);
    atomic_store(&ctl->child_done, 1);
    wait_flag(&ctl->parent_done);
    return 0;
}

static int cow_child_idle(void *argp)
{
    atomic_store(&ctl->child_ready, 1);
    wait_flag(&ctl->parent_done);
    return 0;
}

static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static double med3(double *v) { qsort(v, 3, sizeof v[0], cmp_d); return v[1]; }

static void cow_cost(void)
{
    const size_t SZ = 256u << 20;
    size_t pages = SZ / FG_PAGE;
    double zf[3], nf[3], cc[3], pd[3], pe[3], pa[3];
    for (int rep = 0; rep < 3; rep++) {
        unsigned char *buf = mmap(NULL, SZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (buf == MAP_FAILED) { FAIL("F1.cow", "mmap failed %s", ERRS(errno)); return; }
        uint64_t t0 = dp_ns();
        for (size_t i = 0; i < pages; i++) buf[i * FG_PAGE] = 1;
        zf[rep] = (double)(dp_ns() - t0) / pages;
        double best = 1e18;
        for (int k = 0; k < 3; k++) {
            t0 = dp_ns();
            for (size_t i = 0; i < pages; i++) buf[i * FG_PAGE] = 1;
            double d = (double)(dp_ns() - t0) / pages;
            if (d < best) best = d;
        }
        nf[rep] = best;
        for (int scenario = 0; scenario < 2; scenario++) {
            memset((void *)ctl, 0, sizeof *ctl);
            pid_t pid = fork();
            if (pid == 0) {
                dp_role = "child";
                dp_watchdog(60);
                _exit(scenario == 0 ? cow_child_touch(buf) : cow_child_idle(buf));
            }
            dp_track(pid);
            if (scenario == 0) {
                wait_flag(&ctl->child_done);
                cc[rep] = (double)atomic_load(&ctl->child_ns) / pages;
                t0 = dp_ns();
                for (size_t i = 0; i < pages; i++) buf[i * FG_PAGE] = 3;
                pd[rep] = (double)(dp_ns() - t0) / pages;
            } else {
                wait_flag(&ctl->child_ready);
                t0 = dp_ns();
                for (size_t i = 0; i < pages; i++) buf[i * FG_PAGE] = 4;
                pe[rep] = (double)(dp_ns() - t0) / pages;
            }
            atomic_store(&ctl->parent_done, 1);
            int st = 0;
            dp_wait_timeout(pid, &st, 10000);
        }
        t0 = dp_ns();
        for (size_t i = 0; i < pages; i++) buf[i * FG_PAGE] = 5;
        pa[rep] = (double)(dp_ns() - t0) / pages;
        munmap(buf, SZ);
    }
    INFO("F1.cow", "256 MiB private anonymous, %zu pages of 16 KiB, median of 3: first touch (zero-fill fault) %.0f ns/page; later pass without fault (one store per page, best of 3) %.0f ns/page",
         pages, med3(zf), med3(nf));
    INFO("F1.cow", "after fork: child writes every page first (copy fault) %.0f ns/page; then the parent, child still alive, rewrites every page %.0f ns/page",
         med3(cc), med3(pd));
    INFO("F1.cow", "after fork with an idle child: parent writes every page first (copy fault) %.0f ns/page; after the child exited the parent rewrites all pages at %.0f ns/page",
         med3(pe), med3(pa));
}

/* ---- MAP_JIT region across fork ---------------------------------------------------- */
typedef int (*fn_i)(void);
static void emit_ret(uint32_t *code, int v)
{
    pthread_jit_write_protect_np(0);
    code[0] = 0x52800000u | ((uint32_t)v << 5);   /* movz w0, #v */
    code[1] = 0xd65f03c0u;                          /* ret */
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(code, 8);
}

static uint32_t *jit_code;
static int jit_child(void *argp)
{
    int r1 = ((fn_i)jit_code)();
    CHECK(r1 == 42, "F1.jit", "child executes JIT code compiled by the parent before fork (returned %d)", r1);
    emit_ret(jit_code, 43);
    int r2 = ((fn_i)jit_code)();
    CHECK(r2 == 43, "F1.jit", "child rewrites the JIT page (pthread_jit_write_protect_np toggle) and runs the new code (returned %d)", r2);
    return (r1 == 42 && r2 == 43) ? 0 : 1;
}

static void jit_test(void)
{
    INFO("F1.jit", "pthread_jit_write_protect_supported_np = %d", pthread_jit_write_protect_supported_np());
    jit_code = mmap(NULL, 4 * FG_PAGE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (jit_code == MAP_FAILED) { INFO("F1.jit", "MAP_JIT mmap failed: %s", ERRS(errno)); return; }
    emit_ret(jit_code, 42);
    int st = 0;
    char b[160];
    int rc = fg_run_in_child(jit_child, NULL, 10000, &st);
    INFO("F1.jit", "JIT child %s", fg_outcome_str(rc, st, b, sizeof b));
    int r = ((fn_i)jit_code)();
    CHECK(r == 42, "F1.jit", "parent's JIT page still returns 42 after the child rewrote its copy (returned %d): copy-on-write works on MAP_JIT pages", r);
    munmap(jit_code, 4 * FG_PAGE);
}

/* ---- storm of forks while other threads use malloc ----------------------------------- */
static int storm_child(void *argp)
{
    for (int i = 0; i < 2000; i++) {
        char *p = malloc(32 + (i % 2048));
        snprintf(p, 32, "%d", i);
        free(p);
    }
    return n_pthread_create(NULL);
}

static void storm(int n)
{
    int ok = 0, hung = 0, sig = 0, bad = 0, ff = 0;
    uint64_t t0 = dp_ns();
    for (int i = 0; i < n; i++) {
        int st = 0;
        int rc = fg_run_in_child(storm_child, NULL, 3000, &st);
        switch (rc) {
        case FG_OK: ok++; break;
        case FG_HUNG: hung++; break;
        case FG_SIGNALED: sig++; break;
        case FG_BADEXIT: bad++; break;
        default: ff++; break;
        }
    }
    INFO("F1.storm", "%d forks from a process with a thread hammering malloc: ok=%d hung=%d signaled=%d badexit=%d forkfail=%d, %.2f ms per fork+child+wait",
         n, ok, hung, sig, bad, ff, (double)(dp_ns() - t0) / 1e6 / n);
    CHECK(ok == n, "F1.storm", "every child of the storm completed (libmalloc atfork handlers keep malloc usable in the child)");
}

/* ---- vfork -------------------------------------------------------------------------- */
static volatile int vfork_var;
static void vfork_test(void)
{
    vfork_var = 0;
    pid_t p = vfork();
    if (p == 0) { vfork_var = 1; _exit(0); }
    int st = 0;
    waitpid(p, &st, 0);
    INFO("F1.vfork", "vfork child writes a global then _exit: parent sees %d (1 means shared address space, 0 means vfork behaves as fork)", vfork_var);
    p = vfork();
    if (p == 0) { execl("/usr/bin/true", "true", (char *)NULL); _exit(127); }
    waitpid(p, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "F1.vfork", "vfork + execl(/usr/bin/true) works (%d)", st);
}

int main(int argc, char **argv)
{
    dp_watchdog(170);
    crash_tests = getenv("FG_CRASH_TESTS") != NULL;
    fg_load_info("F1.env");
    INFO("F1.env", "pid=%d pagesize=%d", getpid(), getpagesize());
    pthread_atfork(at_prepare, at_parent, at_child);
    pthread_key_create(&tsd_key, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = usr1_handler;
    sigaction(SIGUSR1, &sa, NULL);
    sa.sa_handler = usr2_handler;
    sigaction(SIGUSR2, &sa, NULL);
    if (pipe(pipe_a) || pipe(pipe_b) || pipe(pipe_c)) { FAIL("F1.setup", "pipe failed"); return 1; }
    held_file = fopen("/dev/null", "w");
    kern_return_t kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &parent_port);
    if (kr == KERN_SUCCESS) {
        mach_port_insert_right(mach_task_self(), parent_port, parent_port, MACH_MSG_TYPE_MAKE_SEND);
        mach_port_set_context(mach_task_self(), parent_port, 0x5a17c0de5a17c0deull);
    }
    parent_pid = getpid();

    pthread_create(&workers[0], NULL, w_spin, NULL);
    pthread_create(&workers[1], NULL, w_malloc, NULL);
    pthread_create(&workers[2], NULL, w_holder, NULL);
    pthread_create(&workers[3], NULL, w_stdio, NULL);
    pthread_create(&workers[4], NULL, w_blocked, NULL);
    nworkers = 5;
    usleep(150000);
    INFO("F1.setup", "5 helper threads running (spin, malloc, mutex+unfair lock holder blocked in read, stdio lock holder, blocked in read)");

    run_probe(0);
    n_dispatch(NULL);          /* the parent now uses libdispatch: its worker threads exist at the next fork */
    gcd_warm = 1;
    pthread_t t;
    pthread_create(&t, NULL, forker_thread, NULL);
    pthread_join(t, NULL);

    mem_tests();
    cow_cost();
    jit_test();
    storm(300);
    vfork_test();

    atomic_store(&stop_workers, 1);
    (void)!write(pipe_a[1], "x", 1);
    (void)!write(pipe_b[1], "x", 1);
    (void)!write(pipe_c[1], "x", 1);
    for (int i = 0; i < nworkers; i++) pthread_join(workers[i], NULL);
    dp_summary("f1_fork_mt");
    return 0;
}
