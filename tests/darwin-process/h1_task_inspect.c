/*
 * h1_task_inspect.c - H: use the task port of an own child signed with com.apple.security.get-task-allow.
 *
 *   H1.tfp       task_for_pid on a direct child, under a deadline (an authorization prompt would block)
 *   H1.alt       task_name_for_pid / task_inspect_for_pid / task_read_for_pid on the same child
 *   H1.vmread    mach_vm_read_overwrite, mach_vm_read, mach_vm_remap: content and latency
 *   H1.vmwrite   mach_vm_write on RW, RO and code pages (VM_PROT_COPY), instruction visibility
 *   H1.region    mach_vm_region / mach_vm_region_recurse scan of the child
 *   H1.thread    task_threads, thread_info, thread_get_state (GPR, NEON, exception, debug), latency
 *   H1.redirect  thread_set_state to move a running thread to a new pc with new arguments
 *   H1.stop      task_suspend / task_resume
 */
#include "hi_debuggee.h"
#include <mach/task_info.h>
#include <mach/thread_info.h>

extern kern_return_t task_inspect_for_pid(mach_port_name_t target_tport, int pid, mach_port_name_t *t) __attribute__((weak_import));
extern kern_return_t task_read_for_pid(mach_port_name_t target_tport, int pid, mach_port_name_t *t) __attribute__((weak_import));

static mach_port_t g_task;
static struct hi_dbg_info di;
static struct hi_child ch;

struct tfp_arg { int pid; mach_port_t task; kern_return_t kr; uint64_t ns; };

static void *tfp_worker(void *p)
{
    struct tfp_arg *a = p;
    uint64_t t0 = dp_ns();
    a->kr = task_for_pid(mach_task_self(), a->pid, &a->task);
    a->ns = dp_ns() - t0;
    return NULL;
}

static void fill(uint8_t *p, size_t n, uint8_t seed) { for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(seed + i); }

static int vm_read_exact(uint64_t addr, void *dst, size_t n)
{
    mach_vm_size_t out = 0;
    kern_return_t kr = mach_vm_read_overwrite(g_task, addr, n, (mach_vm_address_t)(uintptr_t)dst, &out);
    return (kr == KERN_SUCCESS && out == n) ? 0 : (kr ? kr : -1);
}

static uint64_t read_counter(void)
{
    uint64_t v = 0;
    vm_read_exact(di.counter_addr, &v, 8);
    return v;
}

/* ---- H1.vmread --------------------------------------------------------------- */
static void bench_read(size_t sz, int n)
{
    uint8_t *dst = malloc(sz);
    uint64_t *v = malloc(sizeof(uint64_t) * n);
    int i;
    kern_return_t kr = KERN_SUCCESS;
    for (i = 0; i < n; i++) {
        mach_vm_size_t out;
        uint64_t t0 = dp_ns();
        kr = mach_vm_read_overwrite(g_task, di.buf_addr, sz, (mach_vm_address_t)(uintptr_t)dst, &out);
        v[i] = dp_ns() - t0;
        if (kr != KERN_SUCCESS) break;
    }
    char label[64];
    snprintf(label, sizeof label, "mach_vm_read_overwrite %7zu B", sz);
    if (kr != KERN_SUCCESS) FAIL("H1.vmread", "%s failed %s", label, hi_kr_str(kr));
    else {
        hi_report_us("H1.vmread", label, v, n);
        dp_stats s = dp_stat_u64(v, n);
        if (sz >= 65536) INFO("H1.vmread", "  => %.2f GB/s at p50 for %zu B", (double)sz / s.p50, sz);
    }
    free(dst); free(v);
}

static void h1_vmread(void)
{
    struct hi_msg r;
    hi_call(&ch, DC_FILL_BUF, 0, 1 << 20, 7, &r, 3000);
    uint8_t *got = malloc(1 << 20), *want = malloc(1 << 20);
    fill(want, 1 << 20, 7);
    int rc = vm_read_exact(di.buf_addr, got, 1 << 20);
    CHECK(rc == 0 && memcmp(got, want, 1 << 20) == 0, "H1.vmread", "1 MiB read from the child equals the pattern the child wrote (rc=%d)", rc);

    bench_read(8, 20000);
    bench_read(64, 20000);
    bench_read(4096, 20000);
    bench_read(65536, 5000);
    bench_read(1 << 20, 500);

    /* mach_vm_read (kernel allocates the copy in this task) */
    {
        enum { N = 10000 };
        uint64_t *v = malloc(sizeof(uint64_t) * N);
        kern_return_t kr = KERN_SUCCESS;
        for (int i = 0; i < N; i++) {
            vm_offset_t data; mach_msg_type_number_t cnt;
            uint64_t t0 = dp_ns();
            kr = mach_vm_read(g_task, di.buf_addr, 4096, &data, &cnt);
            if (kr == KERN_SUCCESS) mach_vm_deallocate(mach_task_self(), data, cnt);
            v[i] = dp_ns() - t0;
            if (kr) break;
        }
        if (kr) FAIL("H1.vmread", "mach_vm_read 4096 B failed %s", hi_kr_str(kr));
        else hi_report_us("H1.vmread", "mach_vm_read+dealloc      4096 B", v, N);
        free(v);
    }

    /* mach_vm_remap: map the child's buffer into this task (zero copy) */
    {
        mach_vm_address_t local = 0;
        vm_prot_t cur = 0, max = 0;
        kern_return_t kr = mach_vm_remap(mach_task_self(), &local, 1 << 20, 0, VM_FLAGS_ANYWHERE, g_task, di.buf_addr, FALSE, &cur, &max, VM_INHERIT_NONE);
        if (kr != KERN_SUCCESS) {
            INFO("H1.remap", "mach_vm_remap(copy=FALSE) of the child's anonymous buffer failed %s", hi_kr_str(kr));
        } else {
            INFO("H1.remap", "mach_vm_remap(copy=FALSE) mapped child buffer at local 0x%llx cur_prot=%d max_prot=%d", (unsigned long long)local, cur, max);
            volatile uint8_t *m = (volatile uint8_t *)(uintptr_t)local;
            CHECK(m[5] == (uint8_t)(7 + 5), "H1.remap", "remapped region shows the child's content");
            hi_call(&ch, DC_FILL_BUF, 0, 4096, 99, &r, 3000);
            CHECK(m[5] == (uint8_t)(99 + 5), "H1.remap", "the child's later write is visible through the mapping (shared, not a snapshot)");
            if (cur & VM_PROT_WRITE) {
                uint8_t *w = (uint8_t *)(uintptr_t)local;
                for (int i = 0; i < 4096; i++) w[i] = (uint8_t)(55 + i);
                hi_call(&ch, DC_VERIFY_BUF, 0, 4096, 55, &r, 3000);
                CHECK(r.rc == 0, "H1.remap", "a write through the mapping is seen by the child (rc=%d)", r.rc);
            } else INFO("H1.remap", "mapping is not writable");
            enum { N = 200000 };
            uint64_t *v = malloc(sizeof(uint64_t) * N);
            uint8_t tmp[64]; uint64_t sink = 0;
            for (int i = 0; i < N; i++) {
                uint64_t t0 = dp_ns();
                memcpy(tmp, (void *)(uintptr_t)(local + (i & 0xff) * 64), 8);
                sink += tmp[0];
                v[i] = dp_ns() - t0;
            }
            hi_report_us("H1.remap", "8 B load through remapped child memory", v, N);
            if (sink == 42424242) INFO("H1.remap", "sink");
            free(v);
            mach_vm_deallocate(mach_task_self(), local, 1 << 20);
        }
    }
    free(got); free(want);
}

/* ---- H1.vmwrite ---------------------------------------------------------------- */
static void h1_vmwrite(void)
{
    struct hi_msg r;
    uint8_t *src = malloc(8192);
    fill(src, 8192, 99);
    kern_return_t kr = mach_vm_write(g_task, di.buf_addr + 4096, (vm_offset_t)(uintptr_t)src, 8192);
    hi_call(&ch, DC_VERIFY_BUF, 4096, 8192, 99, &r, 3000);
    CHECK(kr == KERN_SUCCESS && r.rc == 0, "H1.vmwrite", "mach_vm_write of 8192 B into the child, child verifies (kr=%d rc=%d)", kr, r.rc);

    {
        enum { N = 20000 };
        uint64_t *v = malloc(sizeof(uint64_t) * N);
        uint64_t word = 0x1122334455667788ull;
        for (int i = 0; i < N; i++) {
            uint64_t t0 = dp_ns();
            kr = mach_vm_write(g_task, di.buf_addr + 128, (vm_offset_t)(uintptr_t)&word, 8);
            v[i] = dp_ns() - t0;
            if (kr) break;
        }
        if (kr) FAIL("H1.vmwrite", "8 B mach_vm_write failed %s", hi_kr_str(kr));
        else hi_report_us("H1.vmwrite", "mach_vm_write 8 B", v, N);
        free(v);
    }

    /* read-only page */
    uint64_t newv = 0xfeedfacecafebeefull;
    kr = mach_vm_write(g_task, di.ro_addr, (vm_offset_t)(uintptr_t)&newv, 8);
    INFO("H1.vmwrite", "mach_vm_write onto a PROT_READ page directly -> %s (%s)", hi_kr_str(kr), kr == KERN_SUCCESS ? "succeeded without changing protection" : "refused");
    hi_call(&ch, DC_RO_READ, 0, 0, 0, &r, 3000);
    uint64_t before = r.a[0];
    kr = mach_vm_protect(g_task, di.ro_addr, HI_PAGE, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
    INFO("H1.vmwrite", "mach_vm_protect(RW|COPY) on the RO page -> %s", hi_kr_str(kr));
    if (kr == KERN_SUCCESS) {
        kern_return_t kw = mach_vm_write(g_task, di.ro_addr, (vm_offset_t)(uintptr_t)&newv, 8);
        kern_return_t kp = mach_vm_protect(g_task, di.ro_addr, HI_PAGE, FALSE, VM_PROT_READ);
        hi_call(&ch, DC_RO_READ, 0, 0, 0, &r, 3000);
        CHECK(kw == KERN_SUCCESS && kp == KERN_SUCCESS && r.a[0] == newv, "H1.vmwrite",
              "write through a temporary RW|COPY window then restore R: child reads 0x%llx (was 0x%llx), kw=%d kp=%d",
              (unsigned long long)r.a[0], (unsigned long long)before, kw, kp);
    }

    free(src);
}


/* ---- H1.codepatch: runs against its own debuggee because a code-signing kill is possible ------ */
static void report_status(const char *id, pid_t pid)
{
    int st; char b[128];
    pid_t r = waitpid(pid, &st, WNOHANG);
    if (r == pid) INFO(id, "child %d is gone: %s", (int)pid, dp_status_str(st, b, sizeof b));
    else INFO(id, "child %d still running", (int)pid);
}

static void h1_codepatch(int attach_first)
{
    struct hi_child c2; struct hi_dbg_info d2; struct hi_msg r;
    const char *id = attach_first ? "H1.codepatch+attach" : "H1.codepatch";
    if (hi_spawn_debuggee(&c2, &d2)) { FAIL(id, "debuggee did not start"); return; }
    struct tfp_arg ta2 = {.pid = c2.pid};
    if (!hi_call_with_deadline(tfp_worker, &ta2, 5000, NULL) || ta2.kr) { FAIL(id, "task_for_pid failed"); kill(c2.pid, SIGKILL); return; }
    mach_port_t t2 = ta2.task;
    if (attach_first) {
        errno = 0;
        int pr = ptrace(PT_ATTACHEXC, c2.pid, 0, 0);
        INFO(id, "ptrace(PT_ATTACHEXC) -> %d errno=%d (%s)", pr, errno, errno ? strerror(errno) : "ok");
        int st = 0;
        pid_t w = hi_wait_stop(c2.pid, &st, 2000);
        char b[128];
        INFO(id, "waitpid after attach -> %d %s", (int)w, w > 0 ? dp_status_str(st, b, sizeof b) : "timeout");
    }
    uint32_t insn[2] = {0, 0};
    mach_vm_size_t out = 0;
    mach_vm_read_overwrite(t2, d2.code_addr, 8, (mach_vm_address_t)(uintptr_t)insn, &out);
    INFO(id, "hi_target_fn at 0x%llx holds %08x %08x (52800020 d65f03c0 = mov w0,#1 ; ret)", (unsigned long long)d2.code_addr, insn[0], insn[1]);
    uint64_t page = d2.code_addr & ~(HI_PAGE - 1);
    kern_return_t kr = mach_vm_protect(t2, page, HI_PAGE, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
    uint32_t patched = 0x52800040;   /* mov w0, #2 */
    kern_return_t kw = kr ? kr : mach_vm_write(t2, d2.code_addr, (vm_offset_t)(uintptr_t)&patched, 4);
    kern_return_t kp = kr ? kr : mach_vm_protect(t2, page, HI_PAGE, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
    INFO(id, "mach_vm_protect(RW|COPY) -> %s ; write 4 B -> %s ; restore R-X -> %s", hi_kr_str(kr), hi_kr_str(kw), hi_kr_str(kp));
    if (attach_first) {
        errno = 0;
        int pr = ptrace(PT_CONTINUE, c2.pid, (caddr_t)1, 0);
        INFO(id, "ptrace(PT_CONTINUE) -> %d errno=%d", pr, errno);
    }
    int rc = hi_call(&c2, DC_CALL_TARGET, 0, 0, 0, &r, 3000);
    if (rc == 0) {
        INFO(id, "hi_target_fn() returned %lld after the patch (original returned 1)", (long long)r.a[0]);
        CHECK((int64_t)r.a[0] == 2, id, "patched instruction executed by the child: code patching through the task port works%s", attach_first ? " after PT_ATTACHEXC" : " without any ptrace attach");
    } else {
        usleep(100000);
        report_status(id, c2.pid);
        FAIL(id, "child did not answer after executing the patched page (died or hung)%s", attach_first ? "" : ": code signing probably killed it; try the attach variant");
    }
    kill(c2.pid, SIGKILL);
    int st; dp_wait_timeout(c2.pid, &st, 1000);
    close(c2.wfd); close(c2.rfd);
}

/* ---- H1.region ----------------------------------------------------------------- */
static void h1_regions(void)
{
    mach_vm_address_t a = 0;
    int n = 0, found_buf = 0, found_ro = 0, found_code = 0;
    uint64_t t0 = dp_ns();
    for (;;) {
        mach_vm_size_t sz = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj;
        kern_return_t kr = mach_vm_region(g_task, &a, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj);
        if (kr != KERN_SUCCESS) break;
        n++;
        if (di.buf_addr >= a && di.buf_addr < a + sz) {
            found_buf = 1;
            INFO("H1.region", "buffer region 0x%llx-0x%llx prot=%d max=%d share=%d reserved=%d inherit=%d", (unsigned long long)a, (unsigned long long)(a + sz), info.protection, info.max_protection, info.shared, info.reserved, info.inheritance);
            CHECK(info.protection == (VM_PROT_READ | VM_PROT_WRITE), "H1.region", "buffer region is rw");
        }
        if (di.ro_addr >= a && di.ro_addr < a + sz) {
            found_ro = 1;
            CHECK(info.protection == VM_PROT_READ, "H1.region", "read-only page is r-- (prot=%d max=%d)", info.protection, info.max_protection);
        }
        if (di.code_addr >= a && di.code_addr < a + sz) {
            found_code = 1;
            INFO("H1.region", "code region 0x%llx-0x%llx prot=%d max=%d", (unsigned long long)a, (unsigned long long)(a + sz), info.protection, info.max_protection);
        }
        a += sz;
    }
    double ms = (dp_ns() - t0) / 1e6;
    INFO("H1.region", "mach_vm_region walked %d regions of the child in %.3f ms (%.1f us per call)", n, ms, ms * 1000 / (n + 1));
    CHECK(found_buf && found_ro && found_code, "H1.region", "buffer, RO page and code page all found (buf=%d ro=%d code=%d)", found_buf, found_ro, found_code);

    /* recursive walk with share modes, tags and object ids, like vmmap / /proc/pid/maps */
    a = 0; n = 0;
    natural_t depth = 0;
    int tags[256] = {0};
    t0 = dp_ns();
    for (;;) {
        mach_vm_size_t sz = 0;
        vm_region_submap_info_data_64_t info;
        mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
        depth = 0xffff;
        kern_return_t kr = mach_vm_region_recurse(g_task, &a, &sz, &depth, (vm_region_recurse_info_t)&info, &cnt);
        if (kr != KERN_SUCCESS) break;
        if (info.is_submap) { depth++; continue; }
        n++;
        if (info.user_tag < 256) tags[info.user_tag]++;
        a += sz;
    }
    ms = (dp_ns() - t0) / 1e6;
    INFO("H1.region", "mach_vm_region_recurse walked %d leaf regions in %.3f ms", n, ms);
    char line[512]; int off = 0;
    for (int i = 0; i < 256; i++) if (tags[i]) off += snprintf(line + off, sizeof line - off, " tag%d=%d", i, tags[i]);
    INFO("H1.region", "user_tag histogram:%s", line);

    /* dyld image list: what /proc/pid/maps would need for file names */
    task_dyld_info_data_t di2; mach_msg_type_number_t cnt2 = TASK_DYLD_INFO_COUNT;
    kern_return_t kr = task_info(g_task, TASK_DYLD_INFO, (task_info_t)&di2, &cnt2);
    INFO("H1.region", "task_info(TASK_DYLD_INFO) -> %s all_image_info_addr=0x%llx size=%llu format=%d", hi_kr_str(kr), (unsigned long long)di2.all_image_info_addr, (unsigned long long)di2.all_image_info_size, di2.all_image_info_format);

    task_vm_info_data_t vi; mach_msg_type_number_t vc = TASK_VM_INFO_COUNT;
    kr = task_info(g_task, TASK_VM_INFO, (task_info_t)&vi, &vc);
    if (kr == KERN_SUCCESS)
        INFO("H1.region", "task_info(TASK_VM_INFO): virtual=%llu KiB resident=%llu KiB phys_footprint=%llu KiB page_size=%d", (unsigned long long)vi.virtual_size / 1024, (unsigned long long)vi.resident_size / 1024, (unsigned long long)vi.phys_footprint / 1024, (int)vi.page_size);
}

/* ---- H1.thread --------------------------------------------------------------- */
static thread_act_t g_spin = MACH_PORT_NULL, g_park = MACH_PORT_NULL;

static void h1_threads(void)
{
    thread_act_array_t th = NULL; mach_msg_type_number_t nth = 0;
    kern_return_t kr = task_threads(g_task, &th, &nth);
    if (kr) { FAIL("H1.thread", "task_threads %s", hi_kr_str(kr)); return; }
    INFO("H1.thread", "task_threads returned %u threads", nth);
    for (unsigned i = 0; i < nth; i++) {
        thread_identifier_info_data_t id; mach_msg_type_number_t c = THREAD_IDENTIFIER_INFO_COUNT;
        thread_extended_info_data_t ex; mach_msg_type_number_t c2 = THREAD_EXTENDED_INFO_COUNT;
        thread_basic_info_data_t bi; mach_msg_type_number_t c3 = THREAD_BASIC_INFO_COUNT;
        kern_return_t k1 = thread_info(th[i], THREAD_IDENTIFIER_INFO, (thread_info_t)&id, &c);
        kern_return_t k2 = thread_info(th[i], THREAD_EXTENDED_INFO, (thread_info_t)&ex, &c2);
        kern_return_t k3 = thread_info(th[i], THREAD_BASIC_INFO, (thread_info_t)&bi, &c3);
        INFO("H1.thread", "thread[%u] port=0x%x tid=%llu handle=0x%llx dispatch_qaddr=0x%llx name='%s' run_state=%d flags=0x%x suspend_count=%d cpu_usage=%d prio=%d (k=%d/%d/%d)",
             i, th[i], k1 ? 0ull : (unsigned long long)id.thread_id, k1 ? 0ull : (unsigned long long)id.thread_handle, k1 ? 0ull : (unsigned long long)id.dispatch_qaddr,
             k2 ? "?" : ex.pth_name, k3 ? -1 : bi.run_state, k3 ? -1 : bi.flags, k3 ? -1 : bi.suspend_count, k3 ? -1 : bi.cpu_usage, k2 ? -1 : ex.pth_priority, k1, k2, k3);
        if (!k1 && id.thread_id == di.main_tid) g_spin = th[i];
        if (!k1 && id.thread_id == di.park_tid) g_park = th[i];
    }
    CHECK(g_spin != MACH_PORT_NULL && g_park != MACH_PORT_NULL, "H1.thread", "main (spin) and park threads identified by thread id: pthread_threadid_np in the child equals THREAD_IDENTIFIER_INFO in the tracer");
    if (g_spin == MACH_PORT_NULL) return;

    /* 1. suspended state of the spinner */
    uint64_t c0 = read_counter();
    usleep(2000);
    uint64_t c1 = read_counter();
    CHECK(c1 > c0, "H1.thread", "spin counter advances in the child (%llu -> %llu)", (unsigned long long)c0, (unsigned long long)c1);
    kr = thread_suspend(g_spin);
    arm_thread_state64_t st; mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
    kern_return_t kg = thread_get_state(g_spin, ARM_THREAD_STATE64, (thread_state_t)&st, &cnt);
    uint64_t mem = read_counter();
    if (kr || kg) FAIL("H1.thread", "thread_suspend %s / thread_get_state(ARM_THREAD_STATE64) %s", hi_kr_str(kr), hi_kr_str(kg));
    else {
        uintptr_t pc = (uintptr_t)arm_thread_state64_get_pc(st);
        int bad = 0, checked = 0;
        for (int i = 1; i < 29; i++) if (hi_spin_reg_const(i)) { checked++; if (st.__x[i] != (uint64_t)(0x1000 + i)) { bad++; INFO("H1.thread", "x%d = 0x%llx expected 0x%x", i, (unsigned long long)st.__x[i], 0x1000 + i); } }
        INFO("H1.thread", "ARM_THREAD_STATE64 count=%u pc=0x%llx sp=0x%llx fp=0x%llx lr=0x%llx cpsr=0x%x pad=0x%x x0=0x%llx x27=%llu counter-in-memory=%llu",
             cnt, (unsigned long long)pc, (unsigned long long)arm_thread_state64_get_sp(st), (unsigned long long)arm_thread_state64_get_fp(st),
             (unsigned long long)(uintptr_t)arm_thread_state64_get_lr(st), st.__cpsr, ((const uint32_t *)&st)[sizeof st / 4 - 1], (unsigned long long)st.__x[0], (unsigned long long)st.__x[27], (unsigned long long)mem);
        CHECK(bad == 0, "H1.thread", "%d constant registers x1..x17,x19..x26,x28 read back exactly (bad=%d)", checked, bad);
        CHECK(pc >= di.spin_lo && pc < di.spin_hi, "H1.thread", "pc is inside the spin loop [0x%llx,0x%llx)", (unsigned long long)di.spin_lo, (unsigned long long)di.spin_hi);
        CHECK(st.__x[27] == mem || st.__x[27] == mem + 1, "H1.thread", "x27 (%llu) matches the counter in the child's memory (%llu), consistent snapshot of registers and memory", (unsigned long long)st.__x[27], (unsigned long long)mem);
        CHECK(st.__x[0] == di.counter_addr, "H1.thread", "x0 holds the counter address");

        arm_neon_state64_t ns; cnt = ARM_NEON_STATE64_COUNT;
        kern_return_t kn = thread_get_state(g_spin, ARM_NEON_STATE64, (thread_state_t)&ns, &cnt);
        int vbad = 0;
        if (!kn) for (int i = 0; i < 8; i++) { uint8_t *b = (uint8_t *)&ns.__v[i]; for (int j = 0; j < 16; j++) if (b[j] != 0x10 + i) vbad++; }
        CHECK(!kn && vbad == 0, "H1.thread", "ARM_NEON_STATE64 v0..v7 read back exactly (kr=%d bad_bytes=%d fpsr=0x%x fpcr=0x%x)", kn, vbad, kn ? 0 : ns.__fpsr, kn ? 0 : ns.__fpcr);

        arm_exception_state64_t es; cnt = ARM_EXCEPTION_STATE64_COUNT;
        kern_return_t ke = thread_get_state(g_spin, ARM_EXCEPTION_STATE64, (thread_state_t)&es, &cnt);
        INFO("H1.thread", "ARM_EXCEPTION_STATE64 -> %s far=0x%llx esr=0x%x exception=0x%x", hi_kr_str(ke), ke ? 0ull : (unsigned long long)es.__far, ke ? 0 : es.__esr, ke ? 0 : es.__exception);

        arm_debug_state64_t ds; cnt = ARM_DEBUG_STATE64_COUNT;
        kern_return_t kd = thread_get_state(g_spin, ARM_DEBUG_STATE64, (thread_state_t)&ds, &cnt);
        INFO("H1.thread", "ARM_DEBUG_STATE64 -> %s count=%u mdscr_el1=0x%llx bvr0=0x%llx bcr0=0x%llx wvr0=0x%llx wcr0=0x%llx", hi_kr_str(kd), cnt, kd ? 0ull : (unsigned long long)ds.__mdscr_el1, kd ? 0ull : (unsigned long long)ds.__bvr[0], kd ? 0ull : (unsigned long long)ds.__bcr[0], kd ? 0ull : (unsigned long long)ds.__wvr[0], kd ? 0ull : (unsigned long long)ds.__wcr[0]);
    }
    thread_resume(g_spin);
    usleep(2000);
    uint64_t c2 = read_counter();
    CHECK(c2 > mem, "H1.thread", "spinner runs again after thread_resume (%llu -> %llu)", (unsigned long long)mem, (unsigned long long)c2);

    /* 2. thread_get_state on a RUNNING thread (no explicit suspend) */
    {
        enum { N = 20000 };
        uint64_t *v = malloc(sizeof(uint64_t) * N);
        kern_return_t k = KERN_SUCCESS;
        for (int i = 0; i < N; i++) {
            cnt = ARM_THREAD_STATE64_COUNT;
            uint64_t t0 = dp_ns();
            k = thread_get_state(g_spin, ARM_THREAD_STATE64, (thread_state_t)&st, &cnt);
            v[i] = dp_ns() - t0;
            if (k) break;
        }
        if (k) FAIL("H1.thread", "thread_get_state on a running thread: %s", hi_kr_str(k));
        else { PASS("H1.thread", "thread_get_state works on a running thread without thread_suspend"); hi_report_us("H1.thread", "thread_get_state (running thread)", v, N); }
        free(v);
    }
    /* 3. suspend + get_state + resume cycle */
    {
        enum { N = 20000 };
        uint64_t *vs = malloc(sizeof(uint64_t) * N), *vg = malloc(sizeof(uint64_t) * N), *vr = malloc(sizeof(uint64_t) * N), *vt = malloc(sizeof(uint64_t) * N);
        for (int i = 0; i < N; i++) {
            cnt = ARM_THREAD_STATE64_COUNT;
            uint64_t t0 = dp_ns();
            thread_suspend(g_spin);
            uint64_t t1 = dp_ns();
            thread_get_state(g_spin, ARM_THREAD_STATE64, (thread_state_t)&st, &cnt);
            uint64_t t2 = dp_ns();
            thread_resume(g_spin);
            uint64_t t3 = dp_ns();
            vs[i] = t1 - t0; vg[i] = t2 - t1; vr[i] = t3 - t2; vt[i] = t3 - t0;
        }
        hi_report_us("H1.thread", "thread_suspend (running spinner)", vs, N);
        hi_report_us("H1.thread", "thread_get_state (suspended)   ", vg, N);
        hi_report_us("H1.thread", "thread_resume                  ", vr, N);
        hi_report_us("H1.thread", "suspend+get_state+resume cycle ", vt, N);
        free(vs); free(vg); free(vr); free(vt);
    }

    /* 4. the thread parked in read(2) */
    if (g_park != MACH_PORT_NULL) {
        thread_basic_info_data_t bi; mach_msg_type_number_t c3 = THREAD_BASIC_INFO_COUNT;
        thread_info(g_park, THREAD_BASIC_INFO, (thread_info_t)&bi, &c3);
        thread_suspend(g_park);
        cnt = ARM_THREAD_STATE64_COUNT;
        kg = thread_get_state(g_park, ARM_THREAD_STATE64, (thread_state_t)&st, &cnt);
        if (kg == KERN_SUCCESS) {
            uintptr_t pc = (uintptr_t)arm_thread_state64_get_pc(st);
            Dl_info dl;
            const char *sym = dladdr((void *)pc, &dl) && dl.dli_sname ? dl.dli_sname : "?";
            INFO("H1.thread", "thread blocked in read(2): run_state=%d (TH_STATE_WAITING=3) pc=0x%llx in %s (read() in the child is at 0x%llx) x16=%llu (BSD syscall number: read=3) x0=%llu",
                 bi.run_state, (unsigned long long)pc, sym, (unsigned long long)di.read_addr, (unsigned long long)st.__x[16], (unsigned long long)st.__x[0]);
            CHECK(st.__x[16] == 3, "H1.thread", "a thread inside a system call shows the syscall number in x16 (here read=3): a tracer can read 'syscall-stop' register state of a thread blocked in the kernel");
        } else FAIL("H1.thread", "thread_get_state on the parked thread %s", hi_kr_str(kg));
        thread_resume(g_park);
    }
}

/* ---- H1.redirect --------------------------------------------------------------- */
static void h1_redirect(void)
{
    if (g_spin == MACH_PORT_NULL) return;
    arm_thread_state64_t st; mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
    thread_suspend(g_spin);
    kern_return_t kg = thread_get_state(g_spin, ARM_THREAD_STATE64, (thread_state_t)&st, &cnt);
    st.__x[0] = 0x1111; st.__x[1] = 0x2222; st.__x[2] = 0x3333;
    arm_thread_state64_set_pc_fptr(st, (void *)(uintptr_t)di.escape_fn);
    kern_return_t ks = thread_set_state(g_spin, ARM_THREAD_STATE64, (thread_state_t)&st, ARM_THREAD_STATE64_COUNT);
    thread_resume(g_spin);
    struct hi_msg r;
    int ok = 0;
    for (int i = 0; i < 200; i++) {
        usleep(5000);
        if (hi_call(&ch, DC_GET_FLAG, 0, 0, 0, &r, 2000) == 0 && r.a[0] == 0xE5CA9Eull) { ok = 1; break; }
    }
    CHECK(kg == 0 && ks == 0 && ok && r.a[1] == 0x1111 && r.a[2] == 0x2222 && r.a[3] == 0x3333, "H1.redirect",
          "thread_set_state(pc=escape_fn, x0..x2) on a running thread moved it: flag=0x%llx args=0x%llx 0x%llx 0x%llx (kg=%d ks=%d)",
          (unsigned long long)r.a[0], (unsigned long long)r.a[1], (unsigned long long)r.a[2], (unsigned long long)r.a[3], kg, ks);
}

/* ---- H1.stop ------------------------------------------------------------------ */
static void h1_stop(void)
{
    enum { N = 3000 };
    uint64_t *v = malloc(sizeof(uint64_t) * N);
    kern_return_t k = KERN_SUCCESS;
    for (int i = 0; i < N; i++) {
        uint64_t t0 = dp_ns();
        k = task_suspend(g_task);
        k |= task_resume(g_task);
        v[i] = dp_ns() - t0;
        if (k) break;
    }
    if (k) FAIL("H1.stop", "task_suspend/task_resume failed %s", hi_kr_str(k));
    else hi_report_us("H1.stop", "task_suspend+task_resume (whole process stop/continue)", v, N);
    free(v);
    /* the child must still answer commands after the cycles */
    struct hi_msg r;
    CHECK(hi_call(&ch, DC_PING, 0, 0, 0, &r, 2000) == 0, "H1.stop", "child still alive and answering after %d suspend/resume cycles", N);
    k = task_suspend(g_task);
    struct hi_msg ping; memset(&ping, 0, sizeof ping); ping.cmd = DC_PING;
    hi_write_full(ch.wfd, &ping, sizeof ping);
    int answered = hi_read_timeout(ch.rfd, &r, sizeof r, 300) == 0;
    task_resume(g_task);
    int answered2 = answered || hi_read_timeout(ch.rfd, &r, sizeof r, 2000) == 0;
    CHECK(k == 0 && !answered && answered2, "H1.stop", "while task_suspend is in force the child does not answer (answered=%d), after task_resume it does (%d)", answered, answered2);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "debuggee")) debuggee_main();
    dp_watchdog(170);
    signal(SIGPIPE, SIG_IGN);
    int rc = hi_spawn_debuggee(&ch, &di);
    if (rc) { FAIL("H1.spawn", "debuggee did not start (%d)", rc); return dp_summary("h1_task_inspect"); }
    INFO("H1.spawn", "debuggee pid=%u buf=0x%llx ro=0x%llx code=0x%llx counter=0x%llx spin=[0x%llx,0x%llx) main_tid=%llu srv_tid=%llu park_tid=%llu",
         di.pid, (unsigned long long)di.buf_addr, (unsigned long long)di.ro_addr, (unsigned long long)di.code_addr, (unsigned long long)di.counter_addr,
         (unsigned long long)di.spin_lo, (unsigned long long)di.spin_hi, (unsigned long long)di.main_tid, (unsigned long long)di.srv_tid, (unsigned long long)di.park_tid);

    struct tfp_arg ta = {.pid = ch.pid};
    int done = hi_call_with_deadline(tfp_worker, &ta, 5000, NULL);
    if (!done) {
        FAIL("H1.tfp", "task_for_pid on a direct get-task-allow child did not return within 5 s: probably an authorization prompt. Stopping this line of tests.");
        kill(ch.pid, SIGKILL);
        dp_summary("h1_task_inspect");
        _exit(2);
    }
    if (ta.kr != KERN_SUCCESS) {
        FAIL("H1.tfp", "task_for_pid on a direct get-task-allow child failed: %s after %.1f us. Stopping this line of tests.", hi_kr_str(ta.kr), ta.ns / 1e3);
        INFO("H1.tfp", "euid=%d; kr=5 is KERN_FAILURE, the usual answer when taskgated/AMFI refuses without prompting", geteuid());
        hi_call(&ch, DC_EXIT, 0, 0, 0, NULL, 1000);
        hi_reap(&ch, 1000);
        return dp_summary("h1_task_inspect");
    }
    g_task = ta.task;
    PASS("H1.tfp", "task_for_pid(own child, get-task-allow) returned port 0x%x in %.1f us, no blocking (a user prompt would have blocked)", g_task, ta.ns / 1e3);
    int p = -1;
    kern_return_t kpt = pid_for_task(g_task, &p);
    CHECK(kpt == 0 && p == (int)ch.pid, "H1.tfp", "pid_for_task round trip gives %d (child is %d)", p, ch.pid);
    {
        enum { N = 2000 };
        uint64_t *v = malloc(sizeof(uint64_t) * N);
        for (int i = 0; i < N; i++) {
            mach_port_t t2; uint64_t t0 = dp_ns();
            kern_return_t k = task_for_pid(mach_task_self(), (int)ch.pid, &t2);
            v[i] = dp_ns() - t0;
            if (k == 0) mach_port_deallocate(mach_task_self(), t2);
        }
        hi_report_us("H1.tfp", "task_for_pid repeated on the same child", v, N);
        free(v);
    }
    {
        mach_port_t pn = MACH_PORT_NULL, pi = MACH_PORT_NULL, pr = MACH_PORT_NULL;
        kern_return_t k1 = task_name_for_pid(mach_task_self(), (int)ch.pid, &pn);
        kern_return_t k2 = task_inspect_for_pid ? task_inspect_for_pid(mach_task_self(), (int)ch.pid, &pi) : -1;
        kern_return_t k3 = task_read_for_pid ? task_read_for_pid(mach_task_self(), (int)ch.pid, &pr) : -1;
        INFO("H1.alt", "task_name_for_pid -> %s ; task_inspect_for_pid -> %s ; task_read_for_pid -> %s", hi_kr_str(k1), hi_kr_str(k2), hi_kr_str(k3));
        if (k3 == 0) {
            vm_offset_t data; mach_msg_type_number_t c;
            kern_return_t kr = mach_vm_read(pr, di.buf_addr, 64, &data, &c);
            INFO("H1.alt", "mach_vm_read through the task_read port -> %s", hi_kr_str(kr));
            if (kr == 0) mach_vm_deallocate(mach_task_self(), data, c);
        }
    }

    h1_vmread();
    h1_vmwrite();
    h1_regions();
    h1_threads();
    h1_stop();
    h1_redirect();

    hi_call(&ch, DC_EXIT, 0, 0, 0, NULL, 1000);
    hi_reap(&ch, 2000);
    h1_codepatch(1);
    h1_codepatch(0);
    return dp_summary("h1_task_inspect");
}
