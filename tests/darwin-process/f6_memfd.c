/*
 * f6_memfd.c - F: what stands in for Linux memfd_create on macOS.
 *
 * Backends compared:
 *   B1 shm_open + shm_unlink          (POSIX shared memory object, an fd)
 *   B2 mkstemp + unlink               (unlinked file on APFS, an fd)
 *   B3 mach_make_memory_entry_64      (a Mach port, not an fd)
 *   B4 MAP_SHARED|MAP_ANON            (no handle at all, shared only through fork/minherit)
 * Per backend: name limits, ftruncate semantics (grow, shrink, repeat), fstat size, read/write/pread/pwrite/lseek,
 * mmap rules (beyond the size, PROT_EXEC, MAP_PRIVATE view, MAP_FIXED over a reservation), huge sizes, sharing with a forked
 * child, page-fault throughput, hole punching, madvise, memory footprint; and a cross-process Mach rendezvous to move a
 * memory entry and a fileport to a spawned child without launchd.
 */
#include "fg_util.h"
#include <mach/mach_vm.h>
#include <mach/task_info.h>
#include <servers/bootstrap.h>
#include <sys/fileport.h>

typedef int (*mk_fn)(void);

static int mk_shm(void)
{
    static int seq;
    char name[32];
    snprintf(name, sizeof name, "/fg6-%d-%d", (int)getpid(), seq++);
    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd >= 0) shm_unlink(name);
    return fd;
}

static int mk_file(void)
{
    char *t = fg_tmpname("f6");
    int fd = mkstemp(t);
    if (fd >= 0) unlink(t);
    free(t);
    return fd;
}

static const char *szs(long long v)
{
    static char b[8][32];
    static int i;
    char *s = b[i++ & 7];
    if (v >= (1ll << 40) && v % (1ll << 40) == 0) snprintf(s, 32, "%lld TiB", v >> 40);
    else if (v >= (1ll << 30) && v % (1ll << 30) == 0) snprintf(s, 32, "%lld GiB", v >> 30);
    else if (v >= (1ll << 20) && v % (1ll << 20) == 0) snprintf(s, 32, "%lld MiB", v >> 20);
    else snprintf(s, 32, "%lld B", v);
    return s;
}

/* ---- name rules (shm only) ------------------------------------------------------------------ */
static void t_names(void)
{
    int lens[] = { 10, 30, 31, 32, 33, 64, 250 };
    for (size_t i = 0; i < sizeof lens / sizeof lens[0]; i++) {
        char name[300];
        name[0] = '/';
        for (int k = 1; k < lens[i]; k++) name[k] = 'a' + (k % 26);
        name[lens[i]] = 0;
        int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
        int e = errno;
        INFO("F6.name", "shm_open with a %d-character name (leading slash included): %s", lens[i], fd >= 0 ? "ok" : ERRS(e));
        if (fd >= 0) { close(fd); shm_unlink(name); }
    }
    char name[32];
    snprintf(name, sizeof name, "/fg6n-%d", (int)getpid());
    int a = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    int b = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    int eb = errno;
    INFO("F6.name", "second shm_open(O_EXCL) of the same name: %s", b >= 0 ? "ok (unexpected)" : ERRS(eb));
    int u1 = shm_unlink(name);
    int u2 = shm_unlink(name);
    INFO("F6.name", "shm_unlink twice: %d then %d errno %s", u1, u2, ERRS(errno));
    int c = shm_open("noslash", O_CREAT | O_EXCL | O_RDWR, 0600);
    INFO("F6.name", "shm_open without leading slash: %s", c >= 0 ? "ok" : ERRS(errno));
    if (c >= 0) { close(c); shm_unlink("noslash"); }
    /* after unlink the fd keeps working */
    int ok = (a >= 0 && ftruncate(a, FG_PAGE) == 0);
    INFO("F6.name", "ftruncate on an fd whose name was already unlinked: %s", ok ? "ok" : ERRS(errno));
    if (a >= 0) close(a);
    if (b >= 0) close(b);
}

/* ---- ftruncate and fstat --------------------------------------------------------------------- */
static void t_ftruncate(const char *be, mk_fn mk)
{
    int fd = mk();
    if (fd < 0) { INFO("F6.ftruncate", "%s: creation failed %s", be, ERRS(errno)); return; }
    long long sizes[] = { 100, 16384, 65536, 4096, 0, 1 << 20, 3 << 20, 5000 };
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        int r = ftruncate(fd, sizes[i]);
        int e = errno;
        struct stat st;
        fstat(fd, &st);
        INFO("F6.ftruncate", "%s: ftruncate(%lld) -> %s ; fstat st_size=%lld st_blocks=%lld st_blksize=%d", be, sizes[i], r == 0 ? "ok" : ERRS(e), (long long)st.st_size, (long long)st.st_blocks, st.st_blksize);
    }
    close(fd);
}

/* ---- read / write / seek -------------------------------------------------------------------------- */
static void t_rw(const char *be, mk_fn mk)
{
    int fd = mk();
    if (fd < 0) return;
    ftruncate(fd, 1 << 20);
    char buf[64] = "hello memfd";
    ssize_t w = pwrite(fd, buf, 12, 100);
    int ew = errno;
    char rb[64] = { 0 };
    ssize_t r = pread(fd, rb, 12, 100);
    int er = errno;
    off_t o = lseek(fd, 50, SEEK_SET);
    int eo = errno;
    ssize_t w2 = write(fd, buf, 12);
    int ew2 = errno;
    ssize_t r2 = read(fd, rb, 4);
    int er2 = errno;
    off_t end = lseek(fd, 0, SEEK_END);
    INFO("F6.rw", "%s: pwrite=%zd(%s) pread=%zd(%s) lseek(50)=%lld(%s) write=%zd(%s) read=%zd(%s) lseek(END)=%lld", be, w, w < 0 ? ERRS(ew) : "ok", r, r < 0 ? ERRS(er) : "ok",
         (long long)o, o < 0 ? ERRS(eo) : "ok", w2, w2 < 0 ? ERRS(ew2) : "ok", r2, r2 < 0 ? ERRS(er2) : "ok", (long long)end);
    close(fd);
}

/* ---- mmap rules ------------------------------------------------------------------------------------- */
static void t_mmap(const char *be, mk_fn mk)
{
    int fd = mk();
    if (fd < 0) return;
    size_t sz = 1 << 20;
    ftruncate(fd, sz);
    unsigned char *m = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(m != MAP_FAILED, "F6.mmap", "%s: mmap(MAP_SHARED, RW, whole size) %s", be, m == MAP_FAILED ? ERRS(errno) : "ok");
    unsigned char *big = mmap(NULL, 2 * sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    int eb = errno;
    int beyond_ok = 0;
    if (big != MAP_FAILED) beyond_ok = fg_probe(big + sz + FG_PAGE, 0);
    INFO("F6.mmap", "%s: mmap of 2x the file size: %s ; reading past the end of the object: %s", be, big == MAP_FAILED ? ERRS(eb) : "accepted", big == MAP_FAILED ? "n/a" : beyond_ok ? "reads zero or data" : "SIGBUS/SIGSEGV");
    if (big != MAP_FAILED) munmap(big, 2 * sz);
    unsigned char *x = mmap(NULL, FG_PAGE, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
    INFO("F6.mmap", "%s: mmap(PROT_READ|PROT_EXEC, MAP_SHARED) %s", be, x == MAP_FAILED ? ERRS(errno) : "accepted (not executed)");
    if (x != MAP_FAILED) munmap(x, FG_PAGE);
    unsigned char *rw = mmap(NULL, FG_PAGE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_SHARED, fd, 0);
    INFO("F6.mmap", "%s: mmap(PROT_READ|PROT_WRITE|PROT_EXEC, MAP_SHARED) %s", be, rw == MAP_FAILED ? ERRS(errno) : "accepted");
    if (rw != MAP_FAILED) munmap(rw, FG_PAGE);
    if (m != MAP_FAILED) {
        int mp = mprotect(m, FG_PAGE, PROT_READ | PROT_EXEC);
        INFO("F6.mmap", "%s: mprotect(RW view -> R+X) %s", be, mp ? ERRS(errno) : "accepted");
        int mp2 = mprotect(m, FG_PAGE, PROT_READ | PROT_WRITE);
        if (mp2) INFO("F6.mmap", "%s: mprotect back to RW failed %s", be, ERRS(errno));
    }
    /* MAP_PRIVATE view = copy-on-write view of the object */
    if (m != MAP_FAILED) {
        m[0] = 0x31;
        unsigned char *pv = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
        if (pv == MAP_FAILED) INFO("F6.mmap", "%s: mmap(MAP_PRIVATE, RW) %s", be, ERRS(errno));
        else {
            int sees = (pv[0] == 0x31);
            pv[0] = 0x32;
            pv[FG_PAGE] = 0x33;
            CHECK(sees && m[0] == 0x31 && m[FG_PAGE] == 0, "F6.mmap", "%s: MAP_PRIVATE view sees the object's data (%d) and its own writes do not reach the shared view", be, sees);
            munmap(pv, sz);
        }
    }
    /* MAP_FIXED over a PROT_NONE reservation */
    {
        size_t rsz = 64 << 20;
        unsigned char *res = mmap(NULL, rsz, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
        unsigned char *f = mmap(res + (8 << 20), sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
        INFO("F6.mmap", "%s: MAP_FIXED view placed inside a 64 MiB PROT_NONE reservation: %s", be, f == MAP_FAILED ? ERRS(errno) : (f == res + (8 << 20) ? "ok, at the requested address" : "ok but moved"));
        if (f != MAP_FAILED && m != MAP_FAILED) {
            f[2] = 0x44;
            CHECK(m[2] == 0x44, "F6.mmap", "%s: two views of one object at different addresses in one process alias each other", be);
        }
        munmap(res, rsz);
    }
    /* mapping offsets */
    {
        unsigned char *o4 = mmap(NULL, FG_PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 4096);
        INFO("F6.mmap", "%s: mmap at file offset 4096 (not a multiple of 16 KiB): %s", be, o4 == MAP_FAILED ? ERRS(errno) : "accepted");
        if (o4 != MAP_FAILED) munmap(o4, FG_PAGE);
        unsigned char *o16 = mmap(NULL, FG_PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, FG_PAGE);
        INFO("F6.mmap", "%s: mmap at file offset 16384: %s", be, o16 == MAP_FAILED ? ERRS(errno) : "accepted");
        if (o16 != MAP_FAILED) munmap(o16, FG_PAGE);
    }
    if (m != MAP_FAILED) munmap(m, sz);
    close(fd);
}

/* ---- huge sizes ---------------------------------------------------------------------------------------- */
static void t_huge(const char *be, mk_fn mk)
{
    long long sizes[] = { 1ll << 30, 16ll << 30, 64ll << 30, 256ll << 30, 1ll << 40 };
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        int fd = mk();
        if (fd < 0) { INFO("F6.huge", "%s: creation failed", be); return; }
        uint64_t t0 = dp_ns();
        int r = ftruncate(fd, sizes[i]);
        int e = errno;
        double tt = (double)(dp_ns() - t0) / 1e3;
        if (r) { INFO("F6.huge", "%s: ftruncate(%s) -> %s (%.0f us)", be, szs(sizes[i]), ERRS(e), tt); close(fd); continue; }
        t0 = dp_ns();
        unsigned char *m = mmap(NULL, sizes[i], PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        int em = errno;
        double tm = (double)(dp_ns() - t0) / 1e3;
        if (m == MAP_FAILED) { INFO("F6.huge", "%s: ftruncate(%s) ok (%.0f us), mmap of the whole object -> %s (%.0f us)", be, szs(sizes[i]), tt, ERRS(em), tm); close(fd); continue; }
        m[0] = 1;
        m[sizes[i] - 1] = 2;
        struct stat st;
        fstat(fd, &st);
        INFO("F6.huge", "%s: ftruncate(%s) ok (%.0f us), mmap whole object ok (%.0f us), first and last byte written, fstat st_size=%s st_blocks=%lld", be, szs(sizes[i]), tt, tm, szs(st.st_size), (long long)st.st_blocks);
        munmap(m, sizes[i]);
        close(fd);
    }
}

/* ---- sharing with a forked child ------------------------------------------------------------------------------ */
typedef struct { _Atomic uint32_t ready, go, done; uint64_t seen_by_child; } syncw_t;

static int share_child(void *arg)
{
    int fd = *(int *)arg;
    unsigned char *m = mmap(NULL, 2 * FG_PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) return 2;
    syncw_t *s = (syncw_t *)(m + FG_PAGE);
    uint64_t v = *(volatile uint64_t *)m;
    s->seen_by_child = v;
    *(volatile uint64_t *)(m + 8) = 0xc0ffee;
    atomic_store(&s->ready, 1);
    for (int i = 0; i < 20000 && !atomic_load(&s->go); i++) usleep(250);
    s->seen_by_child = *(volatile uint64_t *)(m + 16);
    atomic_store(&s->done, 1);
    return 0;
}

static void t_fork_share(const char *be, mk_fn mk)
{
    int fd = mk();
    if (fd < 0) return;
    ftruncate(fd, 2 * FG_PAGE);
    unsigned char *m = mmap(NULL, 2 * FG_PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    syncw_t *s = (syncw_t *)(m + FG_PAGE);
    memset(s, 0, sizeof *s);
    *(volatile uint64_t *)m = 0x1234abcd;
    pid_t pid = fork();
    if (pid == 0) {
        dp_role = "child";
        dp_watchdog(20);
        _exit(share_child(&fd));
    }
    dp_track(pid);
    for (int i = 0; i < 20000 && !atomic_load(&s->ready); i++) usleep(250);
    int a = (s->seen_by_child == 0x1234abcd);
    int b = (*(volatile uint64_t *)(m + 8) == 0xc0ffee);
    *(volatile uint64_t *)(m + 16) = 0xdecafbad;
    atomic_store(&s->go, 1);
    for (int i = 0; i < 20000 && !atomic_load(&s->done); i++) usleep(250);
    int c = (s->seen_by_child == 0xdecafbad);
    int st;
    dp_wait_timeout(pid, &st, 5000);
    CHECK(a && b && c, "F6.fork", "%s: forked child maps the inherited fd; parent->child data seen (%d), child->parent data seen (%d), later parent write seen by child (%d)", be, a, b, c);
    munmap(m, 2 * FG_PAGE);
    close(fd);
}

/* ---- throughput of page faults -------------------------------------------------------------------------------------------- */
static double touch_pass(unsigned char *p, size_t len)
{
    uint64_t t0 = dp_ns();
    for (size_t o = 0; o < len; o += FG_PAGE) p[o] = (unsigned char)o;
    return (double)(dp_ns() - t0) / (len / FG_PAGE);
}

static void footprint(const char *what, uint64_t *phys)
{
    task_vm_info_data_t vi;
    mach_msg_type_number_t cnt = TASK_VM_INFO_COUNT;
    memset(&vi, 0, sizeof vi);
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &cnt) == KERN_SUCCESS) {
        INFO("F6.foot", "%s: phys_footprint=%llu MiB internal=%llu MiB external=%llu MiB", what, (unsigned long long)(vi.phys_footprint >> 20), (unsigned long long)(vi.internal >> 20), (unsigned long long)(vi.external >> 20));
        if (phys) *phys = vi.phys_footprint;
    }
}

static void t_throughput(void)
{
    const size_t SZ = 256u << 20;
    struct { const char *name; int kind; } cases[] = { { "MAP_PRIVATE|MAP_ANON", 0 }, { "MAP_SHARED|MAP_ANON", 1 }, { "shm_open object", 2 }, { "unlinked APFS file", 3 } };
    for (size_t i = 0; i < 4; i++) {
        unsigned char *m = MAP_FAILED;
        int fd = -1;
        uint64_t f0 = 0, f1 = 0;
        footprint("before", &f0);
        switch (cases[i].kind) {
        case 0: m = mmap(NULL, SZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0); break;
        case 1: m = mmap(NULL, SZ, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0); break;
        case 2: fd = mk_shm(); ftruncate(fd, SZ); m = mmap(NULL, SZ, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); break;
        case 3: fd = mk_file(); ftruncate(fd, SZ); m = mmap(NULL, SZ, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); break;
        }
        if (m == MAP_FAILED) { INFO("F6.perf", "%s: mmap failed %s", cases[i].name, ERRS(errno)); continue; }
        double first = touch_pass(m, SZ);
        double best = 1e18;
        for (int k = 0; k < 3; k++) { double d = touch_pass(m, SZ); if (d < best) best = d; }
        uint64_t t0 = dp_ns();
        memset(m, 0x5a, SZ);
        double bw = (double)SZ / ((double)(dp_ns() - t0) / 1e9) / 1e9;
        footprint(cases[i].name, &f1);
        INFO("F6.perf", "%-22s 256 MiB: first touch %.0f ns/page (16 KiB), later pass %.0f ns/page, memset %.1f GB/s, footprint +%llu MiB", cases[i].name, first, best, bw, (unsigned long long)((f1 - f0) >> 20));
        munmap(m, SZ);
        if (fd >= 0) close(fd);
    }
}

/* ---- holes, madvise, F_GETPATH ------------------------------------------------------------------------------------------------ */
static void t_holes_madvise(void)
{
    int fd = mk_file();
    size_t sz = 1 << 20;
    unsigned char *buf = malloc(sz);
    memset(buf, 0xaa, sz);
    ssize_t w = pwrite(fd, buf, sz, 0);
    (void)w;
    fsync(fd);
    struct stat s0, s1;
    fstat(fd, &s0);
    fpunchhole_t ph;
    memset(&ph, 0, sizeof ph);
    ph.fp_offset = 256 << 10;
    ph.fp_length = 256 << 10;
    int r = fcntl(fd, F_PUNCHHOLE, &ph);
    int e = errno;
    fstat(fd, &s1);
    unsigned char rb[16];
    ssize_t n = pread(fd, rb, 16, 300 << 10);
    INFO("F6.hole", "APFS file: F_PUNCHHOLE(256 KiB, 256 KiB) -> %s ; st_blocks %lld -> %lld ; hole reads %s (first byte 0x%02x, n=%zd)", r == 0 ? "ok" : ERRS(e), (long long)s0.st_blocks, (long long)s1.st_blocks,
         n == 16 && rb[0] == 0 ? "zero" : "not zero", n > 0 ? rb[0] : 0, n);
    char path[1024];
    int gp = fcntl(fd, F_GETPATH, path);
    INFO("F6.hole", "F_GETPATH on the unlinked file fd: %s %s", gp == 0 ? "ok" : ERRS(errno), gp == 0 ? path : "");
    free(buf);
    close(fd);

    int sfd = mk_shm();
    ftruncate(sfd, sz);
    int gps = fcntl(sfd, F_GETPATH, path);
    INFO("F6.hole", "F_GETPATH on a shm fd: %s", gps == 0 ? path : ERRS(errno));
    unsigned char *m = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, sfd, 0);
    int advs[] = { MADV_ZERO, MADV_FREE, MADV_DONTNEED, MADV_FREE_REUSABLE, MADV_FREE_REUSE };
    const char *names[] = { "MADV_ZERO", "MADV_FREE", "MADV_DONTNEED", "MADV_FREE_REUSABLE", "MADV_FREE_REUSE" };
    for (int i = 0; i < 5; i++) {
        memset(m, 0x77, sz);
        int ra = madvise(m, sz, advs[i]);
        int ea = errno;
        INFO("F6.madv", "shm mapping after memset(0x77): madvise(%s) -> %s ; first byte now 0x%02x, last byte 0x%02x", names[i], ra ? ERRS(ea) : "ok", m[0], m[sz - 1]);
    }
    int fl = open("/dev/zero", O_RDWR);
    unsigned char *z = mmap(NULL, FG_PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fl, 0);
    INFO("F6.devzero", "mmap(MAP_SHARED) of /dev/zero: %s", z == MAP_FAILED ? ERRS(errno) : "accepted");
    if (z != MAP_FAILED) {
        z[0] = 9;
        pid_t pid = fork();
        if (pid == 0) { z[1] = 7; _exit(0); }
        int st;
        waitpid(pid, &st, 0);
        INFO("F6.devzero", "  /dev/zero MAP_SHARED mapping is %s after fork (child wrote byte 1: parent sees %d)", z[1] == 7 ? "shared" : "private", z[1]);
    }
    munmap(m, sz);
    close(sfd);
    close(fl);
}

/* ---- Mach memory entry (no fd) ------------------------------------------------------------------------------------------------------- */
static int mach_entry_child(void *arg)
{
    unsigned char **v = arg;
    int a = fg_probe(v[0], 0) ? v[0][0] : -1;
    int b = fg_probe(v[1], 0) ? v[1][0] : -1;
    dp_emit("INFO", "F6.mach", "in the forked child: view inherited with VM_INHERIT_SHARE reads 0x%02x, view inherited with VM_INHERIT_COPY reads 0x%02x", a, b);
    if (a >= 0) v[0][1] = 0x91;
    if (b >= 0) v[1][1] = 0x92;
    return 0;
}

static void t_mach_entry(void)
{
    mach_vm_size_t sz = 4 << 20;
    memory_object_size_t msz = sz;
    mach_port_t mem = MACH_PORT_NULL;
    kern_return_t kr = mach_make_memory_entry_64(mach_task_self(), &msz, 0, MAP_MEM_NAMED_CREATE | VM_PROT_READ | VM_PROT_WRITE, &mem, MACH_PORT_NULL);
    INFO("F6.mach", "mach_make_memory_entry_64(NAMED_CREATE, %s) kr=%d (%s) size=%llu port=0x%x", szs(sz), kr, mach_error_string(kr), (unsigned long long)msz, mem);
    if (kr != KERN_SUCCESS) return;
    mach_vm_address_t a1 = 0, a2 = 0, a3 = 0;
    kern_return_t k1 = mach_vm_map(mach_task_self(), &a1, sz, 0, VM_FLAGS_ANYWHERE, mem, 0, FALSE, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_SHARE);
    kern_return_t k2 = mach_vm_map(mach_task_self(), &a2, sz, 0, VM_FLAGS_ANYWHERE, mem, 0, FALSE, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_COPY);
    kern_return_t k3 = KERN_FAILURE;  /* the copy=TRUE view is mapped after the first write: copy=TRUE snapshots the entry when it is mapped */
    INFO("F6.mach", "mach_vm_map of the entry twice (shared views): inherit-share kr=%d, inherit-copy kr=%d", k1, k2);
    unsigned char *p1 = (unsigned char *)(uintptr_t)a1, *p2 = (unsigned char *)(uintptr_t)a2, *p3 = (unsigned char *)(uintptr_t)a3;
    if (k1 || k2) {
        INFO("F6.mach", "kr 17 is KERN_INVALID_RIGHT: the first run asked max_protection VM_PROT_ALL for an entry created read/write only; the mappings now use max_protection read|write");
        FAIL("F6.mach", "a mapping of the memory entry failed (k1=%d k2=%d), the aliasing checks are skipped", k1, k2);
        return;
    }
    p1[0] = 0x5c;
    CHECK(p2[0] == 0x5c, "F6.mach", "two mappings of one memory entry alias each other (write via the first, read 0x%02x via the second)", p2[0]);
    k3 = mach_vm_map(mach_task_self(), &a3, sz, 0, VM_FLAGS_ANYWHERE, mem, 0, TRUE, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_COPY);
    p3 = (unsigned char *)(uintptr_t)a3;
    INFO("F6.mach", "mach_vm_map with copy=TRUE after the write: kr=%d", k3);
    CHECK(k3 == 0 && p3[0] == 0x5c, "F6.mach", "a copy=TRUE mapping made after the write starts with the entry's content (0x%02x)", k3 == 0 ? p3[0] : 0xff);
    p1[0] = 0x5d; /* later writes to the shared view must not reach the snapshot */
    CHECK(k3 == 0 && p3[0] == 0x5c, "F6.mach", "a later write through the shared view does not reach the copy=TRUE snapshot (reads 0x%02x)", k3 == 0 ? p3[0] : 0xff);
    p1[0] = 0x5c;
    if (k3 == 0) p3[0] = 0x6d;
    CHECK(p1[0] == 0x5c, "F6.mach", "writes to the copy=TRUE view stay private (the shared view still reads 0x%02x)", p1[0]);
    unsigned char *views[2] = { p1, p2 };
    char b[96];
    int st;
    int rc = fg_run_in_child(mach_entry_child, views, 5000, &st);
    INFO("F6.mach", "forked child: %s ; parent now reads view1[1]=0x%02x (child wrote 0x91 through its VM_INHERIT_SHARE mapping) view2[1]=0x%02x (child wrote 0x92 through its VM_INHERIT_COPY mapping)", fg_outcome_str(rc, st, b, sizeof b), p1[1], p2[1]);
    mach_port_type_t pt;
    kern_return_t kp = mach_port_type(mach_task_self(), mem, &pt);
    INFO("F6.mach", "memory entry port: type=0x%x kr=%d (a send right to the entry, not a file descriptor; fd-style APIs do not apply)", pt, kp);
    /* sizes that are not a multiple of 16 KiB */
    memory_object_size_t odd = 5000;
    mach_port_t m2 = MACH_PORT_NULL;
    kr = mach_make_memory_entry_64(mach_task_self(), &odd, 0, MAP_MEM_NAMED_CREATE | VM_PROT_READ | VM_PROT_WRITE, &m2, MACH_PORT_NULL);
    INFO("F6.mach", "named entry of 5000 bytes: kr=%d, size rounded to %llu", kr, (unsigned long long)odd);
    /* very large named entry */
    memory_object_size_t big = 256ull << 30;
    mach_port_t m3 = MACH_PORT_NULL;
    kr = mach_make_memory_entry_64(mach_task_self(), &big, 0, MAP_MEM_NAMED_CREATE | VM_PROT_READ | VM_PROT_WRITE, &m3, MACH_PORT_NULL);
    INFO("F6.mach", "named entry of 256 GiB: kr=%d (%s) size=%llu", kr, mach_error_string(kr), (unsigned long long)big);
    /* resizing: there is no ftruncate on an entry; a second entry must be created (no growth in place) */
}

/* ---- cross-process rendezvous through the bootstrap special port ------------------------------------------------------------------------- */
typedef struct {
    mach_msg_header_t h;
    mach_msg_body_t body;
    mach_msg_port_descriptor_t port;
    uint32_t payload;
} port_msg_t;
typedef struct { port_msg_t m; mach_msg_trailer_t trailer; } port_msg_recv_t;

static kern_return_t send_port(mach_port_t dest, mach_port_t what, mach_msg_type_name_t how, uint32_t payload)
{
    port_msg_t m;
    memset(&m, 0, sizeof m);
    m.h.msgh_bits = MACH_MSGH_BITS_SET(MACH_MSG_TYPE_COPY_SEND, 0, 0, MACH_MSGH_BITS_COMPLEX);
    m.h.msgh_remote_port = dest;
    m.h.msgh_size = sizeof m;
    m.body.msgh_descriptor_count = 1;
    m.port.name = what;
    m.port.disposition = how;
    m.port.type = MACH_MSG_PORT_DESCRIPTOR;
    m.payload = payload;
    return mach_msg(&m.h, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof m, 0, MACH_PORT_NULL, 5000, MACH_PORT_NULL);
}

static kern_return_t recv_port(mach_port_t from, mach_port_t *got, uint32_t *payload, unsigned ms)
{
    port_msg_recv_t r;
    memset(&r, 0, sizeof r);
    kern_return_t kr = mach_msg(&r.m.h, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof r, from, ms, MACH_PORT_NULL);
    if (kr == KERN_SUCCESS) { *got = r.m.port.name; *payload = r.m.payload; }
    return kr;
}

#define MEMSZ (4u << 20)

static int role_machchild(void)
{
    dp_role = "machchild";
    dp_watchdog(30);
    mach_port_t bp = bootstrap_port;
    mach_port_t myrecv = MACH_PORT_NULL;
    mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &myrecv);
    kern_return_t kr = send_port(bp, myrecv, MACH_MSG_TYPE_MAKE_SEND, 1);
    if (kr) { dp_emit("FAIL", "F6.rdv", "child: send of its port through the replaced bootstrap port failed kr=%d", kr); return 2; }
    mach_port_t ent = MACH_PORT_NULL;
    uint32_t pl = 0;
    kr = recv_port(myrecv, &ent, &pl, 8000);
    if (kr) { dp_emit("FAIL", "F6.rdv", "child: receive of the memory entry failed kr=%d", kr); return 3; }
    mach_vm_address_t a = 0;
    kr = mach_vm_map(mach_task_self(), &a, MEMSZ, 0, VM_FLAGS_ANYWHERE, ent, 0, FALSE, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE);
    if (kr) { dp_emit("FAIL", "F6.rdv", "child: mach_vm_map of the received entry failed kr=%d", kr); return 4; }
    unsigned char *p = (unsigned char *)(uintptr_t)a;
    int seen = (p[0] == 0xb1);
    p[1] = 0xb2;
    dp_emit("INFO", "F6.rdv", "child: received a memory entry, mapped at %p, parent's byte %s", (void *)p, seen ? "seen" : "NOT seen");
    /* now a fileport carrying a shm fd */
    mach_port_t fp = MACH_PORT_NULL;
    kr = recv_port(myrecv, &fp, &pl, 8000);
    if (kr) { dp_emit("FAIL", "F6.rdv", "child: receive of the fileport failed kr=%d", kr); return 5; }
    int fd = fileport_makefd(fp);
    unsigned char *q = fd >= 0 ? mmap(NULL, FG_PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
    if (q != MAP_FAILED) { int ok = (q[0] == 0xc1); q[1] = 0xc2; dp_emit("INFO", "F6.rdv", "child: fileport -> fd %d -> mapping, parent's byte %s", fd, ok ? "seen" : "NOT seen"); }
    else dp_emit("FAIL", "F6.rdv", "child: fileport_makefd/mmap failed fd=%d errno=%s", fd, ERRS(errno));
    return seen ? 0 : 6;
}

static void t_rendezvous(void)
{
    mach_port_t R = MACH_PORT_NULL;
    mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &R);
    mach_port_insert_right(mach_task_self(), R, R, MACH_MSG_TYPE_MAKE_SEND);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    int e = posix_spawnattr_setspecialport_np(&at, R, TASK_BOOTSTRAP_PORT);
    INFO("F6.rdv", "posix_spawnattr_setspecialport_np(TASK_BOOTSTRAP_PORT = our rendezvous port): %s", ERRS(e));
    char *argv[] = { "f6child", "--role=machchild", NULL };
    int se;
    pid_t pid = fg_spawn_self(argv, NULL, &at, &se);
    if (pid < 0) { INFO("F6.rdv", "spawn failed %s", ERRS(se)); return; }
    mach_port_t childport = MACH_PORT_NULL;
    uint32_t pl;
    kern_return_t kr = recv_port(R, &childport, &pl, 8000);
    CHECK(kr == KERN_SUCCESS, "F6.rdv", "the spawned child reached us through the bootstrap port we substituted: it sent a port, kr=%d (%s)", kr, mach_error_string(kr));
    if (kr != KERN_SUCCESS) { kill(pid, SIGKILL); return; }
    /* memory entry */
    memory_object_size_t msz = MEMSZ;
    mach_port_t ent;
    mach_make_memory_entry_64(mach_task_self(), &msz, 0, MAP_MEM_NAMED_CREATE | VM_PROT_READ | VM_PROT_WRITE, &ent, MACH_PORT_NULL);
    mach_vm_address_t a = 0;
    mach_vm_map(mach_task_self(), &a, MEMSZ, 0, VM_FLAGS_ANYWHERE, ent, 0, FALSE, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE);
    unsigned char *p = (unsigned char *)(uintptr_t)a;
    p[0] = 0xb1;
    kr = send_port(childport, ent, MACH_MSG_TYPE_COPY_SEND, 2);
    INFO("F6.rdv", "sent the memory entry to the child in a Mach message with a port descriptor: kr=%d", kr);
    /* shm fd through a fileport */
    int sfd = mk_shm();
    ftruncate(sfd, FG_PAGE);
    unsigned char *q = mmap(NULL, FG_PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, sfd, 0);
    q[0] = 0xc1;
    fileport_t fp;
    fileport_makeport(sfd, &fp);
    kr = send_port(childport, fp, MACH_MSG_TYPE_COPY_SEND, 3);
    INFO("F6.rdv", "sent a fileport of a shm fd to the child: kr=%d", kr);
    int st;
    dp_wait_timeout(pid, &st, 10000);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "F6.rdv", "child exited 0 after mapping the entry and the fileport");
    CHECK(p[1] == 0xb2, "F6.rdv", "the parent sees the spawned child's write into the shared memory entry (0x%02x): memory entry sharing between two unrelated processes works without launchd", p[1]);
    CHECK(q[1] == 0xc2, "F6.rdv", "the parent sees the child's write into the shm object received as a fileport (0x%02x)", q[1]);
}

static void t_anon_minherit(void)
{
    unsigned char *m = mmap(NULL, 2 * FG_PAGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    m[0] = 1;
    pid_t pid = fork();
    if (pid == 0) { m[1] = 2; _exit(0); }
    int st;
    waitpid(pid, &st, 0);
    INFO("F6.anon", "MAP_SHARED|MAP_ANON: no handle exists, so a spawned (exec'd) process cannot get it; fork child shares it (child's write seen: %d)", m[1] == 2);
    munmap(m, 2 * FG_PAGE);
    INFO("F6.anon", "dlsym(RTLD_DEFAULT, \"memfd_create\") = %p", dlsym(RTLD_DEFAULT, "memfd_create"));
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--role=machchild")) return role_machchild();
    dp_watchdog(280);
    fg_load_info("F6.env");
    t_names();
    t_ftruncate("B1 shm_open", mk_shm);
    t_ftruncate("B2 unlinked file", mk_file);
    t_rw("B1 shm_open", mk_shm);
    t_rw("B2 unlinked file", mk_file);
    t_mmap("B1 shm_open", mk_shm);
    t_mmap("B2 unlinked file", mk_file);
    t_huge("B1 shm_open", mk_shm);
    t_huge("B2 unlinked file", mk_file);
    t_fork_share("B1 shm_open", mk_shm);
    t_fork_share("B2 unlinked file", mk_file);
    t_throughput();
    t_holes_madvise();
    t_mach_entry();
    t_rendezvous();
    t_anon_minherit();
    dp_summary("f6_memfd");
    return 0;
}
