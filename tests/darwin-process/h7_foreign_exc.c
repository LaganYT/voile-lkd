/*
 * h7_foreign_exc.c - H: the debugger model.  The tracer registers an exception port on the TASK of its own
 * get-task-allow child (task_for_pid), serves the exceptions in a handler thread, edits the faulting thread's
 * state and resumes it.  This is the Darwin counterpart of ptrace stops for faults, breakpoints and single-step.
 *
 *   H7.brk       brk #imm in the child: EXC_BREAKPOINT codes, state rewrite (x0, pc+4), resume, round trip
 *   H7.udf       undefined instruction: EXC_BAD_INSTRUCTION codes
 *   H7.badload   load from address 0x10: EXC_BAD_ACCESS codes
 *   H7.wprot     store to a read-only page: EXC_BAD_ACCESS PROTECTION_FAILURE, handler makes it writable, instruction retried
 *   H7.codepatch execution of a patched code page: exception type and code (why the child died in h1)
 *   H7.step      hardware single-step through ARM_DEBUG_STATE64.mdscr_el1: count, pc progression, rate
 *   H7.hwbp      hardware breakpoint (bvr/bcr) on hi_target_fn
 *   H7.hwwp      hardware watchpoint (wvr/wcr) on the child's buffer
 *   H7.sig       PT_ATTACHEXC and signals as EXC_SOFTWARE exceptions: stop, suppress, deliver (PT_THUPDATE)
 */
#include "hi_debuggee.h"
#include "hi_exc.h"

static mach_port_t g_task, g_exc_port;
static struct hi_dbg_info di;
static struct hi_child ch;

enum { MODE_SKIP = 1, MODE_UNPROTECT, MODE_PASS, MODE_STEP, MODE_CLEAR_HW, MODE_LOG_SUCCEED, MODE_SIG };
static volatile int g_deliver_sig, g_use_thupdate;
static pid_t g_child_pid;
static volatile int g_mode;
static volatile uint64_t g_seen;
static struct {
    int behavior, exc;
    int64_t code[4];
    int ncode;
    uint64_t pc, x0;
    int has_ports, task_is_control;
} g_last;

/* single step bookkeeping */
static volatile uint64_t g_steps, g_step_limit, g_dups, g_prev_pc;
static volatile uint64_t g_step_pc[8];
static uint64_t g_step_t0, g_step_t1, g_counter_end;
static volatile int g_step_done;

static kern_return_t hook(struct hi_exc_event *ev, void *ctx)
{
    (void)ctx;
    g_last.behavior = ev->behavior; g_last.exc = ev->exception; g_last.ncode = ev->ncode;
    for (int i = 0; i < 4; i++) g_last.code[i] = ev->code[i];
    g_last.pc = ev->ts ? (uint64_t)(uintptr_t)arm_thread_state64_get_pc(*ev->ts) : 0;
    g_last.x0 = ev->ts ? ev->ts->__x[0] : 0;
    g_last.has_ports = ev->task != MACH_PORT_NULL;
    if (ev->task != MACH_PORT_NULL && g_seen == 0) {
        vm_offset_t data; mach_msg_type_number_t cnt;
        kern_return_t kr = mach_vm_read(ev->task, di.buf_addr, 8, &data, &cnt);
        g_last.task_is_control = (kr == 0);
        if (kr == 0) mach_vm_deallocate(mach_task_self(), data, cnt);
    }
    g_seen++;
    switch (g_mode) {
    case MODE_SKIP:
        if (ev->ts) {
            ev->ts->__x[0] = 0xBEEF;
            arm_thread_state64_set_pc_fptr(*ev->ts, (void *)(uintptr_t)(g_last.pc + 4));
        } else {
            arm_thread_state64_t st; mach_msg_type_number_t c = ARM_THREAD_STATE64_COUNT;
            thread_get_state(ev->thread, ARM_THREAD_STATE64, (thread_state_t)&st, &c);
            g_last.pc = (uint64_t)(uintptr_t)arm_thread_state64_get_pc(st);
            st.__x[0] = 0xBEEF;
            arm_thread_state64_set_pc_fptr(st, (void *)(uintptr_t)(g_last.pc + 4));
            thread_set_state(ev->thread, ARM_THREAD_STATE64, (thread_state_t)&st, ARM_THREAD_STATE64_COUNT);
        }
        return KERN_SUCCESS;
    case MODE_UNPROTECT:
        mach_vm_protect(g_task, di.ro_addr, HI_PAGE, FALSE, VM_PROT_READ | VM_PROT_WRITE);
        return KERN_SUCCESS;
    case MODE_STEP: {
        uint64_t n = ++g_steps;
        if (n <= 8) g_step_pc[n - 1] = g_last.pc;
        if (g_last.pc == g_prev_pc) g_dups++;
        g_prev_pc = g_last.pc;
        if (n == 1) g_step_t0 = dp_ns();
        arm_debug_state64_t ds; memset(&ds, 0, sizeof ds);
        if (n >= g_step_limit) {
            mach_vm_size_t osz2;
            mach_vm_read_overwrite(g_task, di.counter_addr, 8, (mach_vm_address_t)(uintptr_t)&g_counter_end, &osz2);
            g_step_t1 = dp_ns();
            thread_set_state(ev->thread, ARM_DEBUG_STATE64, (thread_state_t)&ds, ARM_DEBUG_STATE64_COUNT);
            g_step_done = 1;
        } else {
            ds.__mdscr_el1 = 1;      /* the kernel clears SS after each step: re-arm before replying */
            thread_set_state(ev->thread, ARM_DEBUG_STATE64, (thread_state_t)&ds, ARM_DEBUG_STATE64_COUNT);
        }
        return KERN_SUCCESS;
    }
    case MODE_CLEAR_HW: {
        arm_debug_state64_t ds; memset(&ds, 0, sizeof ds);
        kern_return_t kr = thread_set_state(ev->thread, ARM_DEBUG_STATE64, (thread_state_t)&ds, ARM_DEBUG_STATE64_COUNT);
        (void)kr;
        return KERN_SUCCESS;
    }
    case MODE_LOG_SUCCEED:
        return KERN_SUCCESS;
    case MODE_SIG:
        if (ev->exception == EXC_SOFTWARE && ev->code[0] == 0x10003 && g_use_thupdate) {
            int sig = ev->code[1] == SIGSTOP ? 0 : g_deliver_sig;
            errno = 0;
            int pr = ptrace(PT_THUPDATE, g_child_pid, (caddr_t)(uintptr_t)ev->thread, sig);
            g_last.x0 = (uint64_t)(pr == 0 ? 0 : errno);
        }
        return KERN_SUCCESS;
    case MODE_PASS:
    default:
        return KERN_FAILURE;
    }
}

static const char *exc_name(int e)
{
    switch (e) {
    case EXC_BAD_ACCESS: return "EXC_BAD_ACCESS"; case EXC_BAD_INSTRUCTION: return "EXC_BAD_INSTRUCTION"; case EXC_ARITHMETIC: return "EXC_ARITHMETIC";
    case EXC_EMULATION: return "EXC_EMULATION"; case EXC_SOFTWARE: return "EXC_SOFTWARE"; case EXC_BREAKPOINT: return "EXC_BREAKPOINT";
    case EXC_SYSCALL: return "EXC_SYSCALL"; case EXC_MACH_SYSCALL: return "EXC_MACH_SYSCALL"; case EXC_RPC_ALERT: return "EXC_RPC_ALERT";
    case EXC_CRASH: return "EXC_CRASH"; case EXC_RESOURCE: return "EXC_RESOURCE"; case EXC_GUARD: return "EXC_GUARD";
    default: return "?";
    }
}

static void show(const char *id, const char *what)
{
    INFO(id, "%s: %s behavior=%d codes=[0x%llx, 0x%llx] pc=0x%llx x0=0x%llx task_port_in_message=%d (control port usable: %d)", what, exc_name(g_last.exc), g_last.behavior,
         (unsigned long long)g_last.code[0], (unsigned long long)g_last.code[1], (unsigned long long)g_last.pc, (unsigned long long)g_last.x0, g_last.has_ports, g_last.task_is_control);
}

struct tfp_arg { int pid; mach_port_t task; kern_return_t kr; };
static void *tfp_worker(void *p) { struct tfp_arg *a = p; a->kr = task_for_pid(mach_task_self(), a->pid, &a->task); return NULL; }

static int start_child(struct hi_child *c, struct hi_dbg_info *d, mach_port_t *task, const char *id)
{
    if (hi_spawn_debuggee(c, d)) { FAIL(id, "debuggee did not start"); return -1; }
    struct tfp_arg ta = {.pid = c->pid};
    if (!hi_call_with_deadline(tfp_worker, &ta, 5000, NULL) || ta.kr) { FAIL(id, "task_for_pid failed (%d)", ta.kr); kill(c->pid, SIGKILL); return -1; }
    *task = ta.task;
    return 0;
}

static void finish_child(struct hi_child *c)
{
    int st; kill(c->pid, SIGKILL); dp_wait_timeout(c->pid, &st, 2000); close(c->wfd); close(c->rfd);
}

static void set_ports(mach_port_t task, int behavior)
{
    exception_mask_t mask = EXC_MASK_BAD_ACCESS | EXC_MASK_BAD_INSTRUCTION | EXC_MASK_ARITHMETIC | EXC_MASK_SOFTWARE | EXC_MASK_BREAKPOINT | EXC_MASK_SYSCALL | EXC_MASK_MACH_SYSCALL;
    kern_return_t kr = task_set_exception_ports(task, mask, g_exc_port, behavior | MACH_EXCEPTION_CODES, ARM_THREAD_STATE64);
    if (kr) INFO("H7.setup", "task_set_exception_ports(foreign task, behavior %d) -> %s", behavior, hi_kr_str(kr));
}

/* ---- brk / udf / badload / wprot, with the three behaviours ------------------------- */
static void t_basic(void)
{
    struct { int behavior; const char *name; } bs[] = {{EXCEPTION_STATE_IDENTITY, "STATE_IDENTITY"}, {EXCEPTION_STATE, "STATE"}, {EXCEPTION_DEFAULT, "DEFAULT"}};
    for (int bi = 0; bi < 3; bi++) {
        struct hi_msg r;
        char id[40]; snprintf(id, sizeof id, "H7.%s", bs[bi].name);
        set_ports(g_task, bs[bi].behavior);
        g_mode = MODE_SKIP; g_seen = 0;
        int rc = hi_call(&ch, DC_BRK, 0, 0, 0, &r, 3000);
        show(id, "brk #0x77");
        CHECK(rc == 0 && r.a[0] == 0xBEEF && g_last.exc == EXC_BREAKPOINT, id, "brk: handler thread in ANOTHER process saw EXC_BREAKPOINT, rewrote x0 and pc+4, the child resumed with x0=0x%llx", (unsigned long long)r.a[0]);
        g_seen = 0;
        rc = hi_call(&ch, DC_UDF, 0, 0, 0, &r, 3000);
        show(id, "udf #0x1234");
        CHECK(rc == 0 && r.a[0] == 0xBEEF && g_last.exc == EXC_BAD_INSTRUCTION, id, "udf: EXC_BAD_INSTRUCTION handled and resumed");
        g_seen = 0;
        rc = hi_call(&ch, DC_BADLOAD, 0, 0, 0, &r, 3000);
        show(id, "ldr x0,[0x10]");
        CHECK(rc == 0 && r.a[0] == 0xBEEF && g_last.exc == EXC_BAD_ACCESS, id, "bad load: EXC_BAD_ACCESS handled, load emulated by the tracer (x0=0xBEEF) and skipped");

        /* round trip: DC_BRK cycle minus DC_PING cycle */
        enum { N = 5000 };
        uint64_t *vb = malloc(sizeof(uint64_t) * N), *vp = malloc(sizeof(uint64_t) * N);
        for (int i = 0; i < N; i++) {
            uint64_t t0 = dp_ns(); hi_call(&ch, DC_PING, 0, 0, 0, &r, 3000); vp[i] = dp_ns() - t0;
            t0 = dp_ns(); hi_call(&ch, DC_BRK, 0, 0, 0, &r, 3000); vb[i] = dp_ns() - t0;
        }
        hi_report_us(id, "ping command (pipe round trip only)   ", vp, N);
        hi_report_us(id, "brk command (pipe + exception + reply)", vb, N);
        free(vb); free(vp);
        task_set_exception_ports(g_task, EXC_MASK_ALL, MACH_PORT_NULL, EXCEPTION_DEFAULT, THREAD_STATE_NONE);
    }
}

static void t_wprot(void)
{
    struct hi_msg r;
    set_ports(g_task, EXCEPTION_STATE_IDENTITY);
    g_mode = MODE_UNPROTECT; g_seen = 0;
    int rc = hi_call(&ch, DC_STORE_RO, 0xA5A5A5A5ull, 0, 0, &r, 3000);
    show("H7.wprot", "store to a read-only page");
    CHECK(rc == 0 && g_seen >= 1 && r.a[0] == 0xA5A5A5A5ull && g_last.exc == EXC_BAD_ACCESS, "H7.wprot",
          "a store fault was fixed by the tracer (mach_vm_protect RW in the child) and the same instruction retried: write-protect emulation from a foreign handler works (code[0]=%lld: 2 = KERN_PROTECTION_FAILURE)", (long long)g_last.code[0]);
    /* repeated: re-protect from the tracer then fault again */
    enum { N = 3000 };
    uint64_t *v = malloc(sizeof(uint64_t) * N);
    for (int i = 0; i < N; i++) {
        mach_vm_protect(g_task, di.ro_addr, HI_PAGE, FALSE, VM_PROT_READ);
        uint64_t t0 = dp_ns();
        hi_call(&ch, DC_STORE_RO, 1 + i, 0, 0, &r, 3000);
        v[i] = dp_ns() - t0;
    }
    hi_report_us("H7.wprot", "store command with a write-protect fault fixed by mach_vm_protect from the tracer", v, N);
    free(v);
    task_set_exception_ports(g_task, EXC_MASK_ALL, MACH_PORT_NULL, EXCEPTION_DEFAULT, THREAD_STATE_NONE);
}

/* ---- single step and hardware breakpoints ------------------------------------------- */
static thread_act_t find_thread(uint64_t tid)
{
    thread_act_array_t th; mach_msg_type_number_t n;
    if (task_threads(g_task, &th, &n)) return MACH_PORT_NULL;
    for (unsigned i = 0; i < n; i++) {
        thread_identifier_info_data_t id; mach_msg_type_number_t c = THREAD_IDENTIFIER_INFO_COUNT;
        if (thread_info(th[i], THREAD_IDENTIFIER_INFO, (thread_info_t)&id, &c) == 0 && id.thread_id == tid) return th[i];
    }
    return MACH_PORT_NULL;
}

static void t_step(void)
{
    struct hi_child c; struct hi_dbg_info d; mach_port_t task;
    if (start_child(&c, &d, &task, "H7.step")) return;
    mach_port_t save_task = g_task; struct hi_dbg_info save_di = di;
    g_task = task; di = d;
    set_ports(g_task, EXCEPTION_STATE_IDENTITY);
    thread_act_t spin = find_thread(d.main_tid);
    if (!spin) { FAIL("H7.step", "spinner thread not found"); goto out; }
    g_mode = MODE_STEP; g_steps = 0; g_step_limit = 20000; g_step_done = 0; g_seen = 0;
    thread_suspend(spin);
    uint64_t counter0 = 0; mach_vm_size_t osz;
    mach_vm_read_overwrite(g_task, d.counter_addr, 8, (mach_vm_address_t)(uintptr_t)&counter0, &osz);
    arm_debug_state64_t ds; mach_msg_type_number_t cnt = ARM_DEBUG_STATE64_COUNT;
    thread_get_state(spin, ARM_DEBUG_STATE64, (thread_state_t)&ds, &cnt);
    ds.__mdscr_el1 |= 1;     /* SS */
    kern_return_t ks = thread_set_state(spin, ARM_DEBUG_STATE64, (thread_state_t)&ds, ARM_DEBUG_STATE64_COUNT);
    INFO("H7.step", "thread_set_state(ARM_DEBUG_STATE64, mdscr_el1.SS=1) -> %s", hi_kr_str(ks));
    thread_resume(spin);
    for (int i = 0; i < 400 && !g_step_done; i++) usleep(5000);
    show("H7.step", "last single-step exception");
    INFO("H7.step", "%llu single-step exceptions received; first pcs: %llx %llx %llx %llx %llx %llx (spin loop is %llx..%llx, 3 instructions)", (unsigned long long)g_steps,
         (unsigned long long)g_step_pc[0], (unsigned long long)g_step_pc[1], (unsigned long long)g_step_pc[2], (unsigned long long)g_step_pc[3], (unsigned long long)g_step_pc[4], (unsigned long long)g_step_pc[5],
         (unsigned long long)d.spin_lo, (unsigned long long)d.spin_hi);
    if (g_step_done) {
        double us = (g_step_t1 - g_step_t0) / 1e3;
        INFO("H7.step", "%llu single steps in %.1f us = %.2f us per step (exception to handler thread, reply, resume)", (unsigned long long)g_steps, us, us / (double)(g_steps - 1));
        CHECK(g_last.exc == EXC_BREAKPOINT, "H7.step", "hardware single-step is delivered as EXC_BREAKPOINT (code[0]=0x%llx; EXC_ARM_DA_DEBUG is 0x102)", (unsigned long long)g_last.code[0]);
        int64_t delta = (int64_t)(g_counter_end - counter0);
        int64_t expect = (int64_t)((g_steps - g_dups) / 3);
        INFO("H7.step", "%llu of %llu step exceptions (%.2f %%) were reported at the same pc as the previous one (no architectural progress): a stepping client must tolerate repeats", (unsigned long long)g_dups, (unsigned long long)g_steps, 100.0 * g_dups / g_steps);
        CHECK(llabs(delta - expect) <= 3, "H7.step", "all other exceptions advanced exactly one instruction: loop counter advanced %lld, expected (steps - repeats)/3 = %lld", (long long)delta, (long long)expect);
    } else FAIL("H7.step", "single-step did not complete (steps=%llu)", (unsigned long long)g_steps);
out:
    task_set_exception_ports(g_task, EXC_MASK_ALL, MACH_PORT_NULL, EXCEPTION_DEFAULT, THREAD_STATE_NONE);
    finish_child(&c);
    g_task = save_task; di = save_di;
}

static void t_hw(void)
{
    struct hi_child c; struct hi_dbg_info d; mach_port_t task;
    if (start_child(&c, &d, &task, "H7.hwbp")) return;
    mach_port_t save_task = g_task; struct hi_dbg_info save_di = di;
    g_task = task; di = d;
    set_ports(g_task, EXCEPTION_STATE_IDENTITY);
    struct hi_msg r;
    thread_act_t srv = find_thread(d.srv_tid);
    /* hardware breakpoint on hi_target_fn for the server thread */
    arm_debug_state64_t ds; mach_msg_type_number_t cnt = ARM_DEBUG_STATE64_COUNT;
    thread_suspend(srv);
    thread_get_state(srv, ARM_DEBUG_STATE64, (thread_state_t)&ds, &cnt);
    ds.__bvr[0] = d.code_addr;
    ds.__bcr[0] = (0xfull << 5) | (2ull << 1) | 1ull;     /* BAS=0b1111, PMC=EL0, E=1 */
    kern_return_t ks = thread_set_state(srv, ARM_DEBUG_STATE64, (thread_state_t)&ds, ARM_DEBUG_STATE64_COUNT);
    thread_resume(srv);
    INFO("H7.hwbp", "thread_set_state(ARM_DEBUG_STATE64, bvr0=hi_target_fn, bcr0=0x%llx) -> %s", (unsigned long long)ds.__bcr[0], hi_kr_str(ks));
    g_mode = MODE_CLEAR_HW; g_seen = 0;
    int rc = hi_call(&c, DC_CALL_TARGET, 0, 0, 0, &r, 3000);
    show("H7.hwbp", "call into hi_target_fn with a hardware breakpoint on it");
    CHECK(rc == 0 && g_seen >= 1 && g_last.exc == EXC_BREAKPOINT && g_last.pc == d.code_addr, "H7.hwbp", "hardware instruction breakpoint hit at hi_target_fn (pc=0x%llx), handler cleared it, call completed with %lld", (unsigned long long)g_last.pc, rc ? -1LL : (long long)r.a[0]);

    /* hardware watchpoint on the buffer */
    cnt = ARM_DEBUG_STATE64_COUNT;
    memset(&ds, 0, sizeof ds);
    thread_suspend(srv);
    thread_get_state(srv, ARM_DEBUG_STATE64, (thread_state_t)&ds, &cnt);
    ds.__wvr[0] = d.buf_addr;
    ds.__wcr[0] = (0xffull << 5) | (2ull << 3) | (2ull << 1) | 1ull;    /* BAS=8 bytes, LSC=store, PAC=EL0, E=1 */
    ks = thread_set_state(srv, ARM_DEBUG_STATE64, (thread_state_t)&ds, ARM_DEBUG_STATE64_COUNT);
    thread_resume(srv);
    INFO("H7.hwwp", "thread_set_state(wvr0=buffer, wcr0=0x%llx) -> %s", (unsigned long long)ds.__wcr[0], hi_kr_str(ks));
    g_mode = MODE_CLEAR_HW; g_seen = 0;
    rc = hi_call(&c, DC_FILL_BUF, 0, 64, 5, &r, 3000);
    show("H7.hwwp", "store into the watched buffer");
    CHECK(rc == 0 && g_seen >= 1 && g_last.exc == EXC_BREAKPOINT, "H7.hwwp", "hardware watchpoint on a store fired (EXC_BREAKPOINT code[0]=0x%llx code[1]=0x%llx = data address); %d watchpoint register(s) available on this CPU", (unsigned long long)g_last.code[0], (unsigned long long)g_last.code[1], 4);

    task_set_exception_ports(g_task, EXC_MASK_ALL, MACH_PORT_NULL, EXCEPTION_DEFAULT, THREAD_STATE_NONE);
    finish_child(&c);
    g_task = save_task; di = save_di;
}

/* ---- why the patched code page kills the child -------------------------------------- */
static void t_codepatch(void)
{
    struct hi_child c; struct hi_dbg_info d; mach_port_t task;
    if (start_child(&c, &d, &task, "H7.codepatch")) return;
    mach_port_t save_task = g_task; struct hi_dbg_info save_di = di;
    g_task = task; di = d;
    set_ports(g_task, EXCEPTION_STATE_IDENTITY);
    g_mode = MODE_PASS; g_seen = 0;
    uint64_t page = d.code_addr & ~(HI_PAGE - 1);
    kern_return_t kr = mach_vm_protect(task, page, HI_PAGE, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
    uint32_t patched = 0x52800040;
    kern_return_t kw = mach_vm_write(task, d.code_addr, (vm_offset_t)(uintptr_t)&patched, 4);
    kern_return_t kp = mach_vm_protect(task, page, HI_PAGE, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
    INFO("H7.codepatch", "protect RW|COPY %s ; write %s ; protect R-X %s", hi_kr_str(kr), hi_kr_str(kw), hi_kr_str(kp));
    struct hi_msg r;
    int rc = hi_call(&c, DC_CALL_TARGET, 0, 0, 0, &r, 3000);
    usleep(100000);
    show("H7.codepatch", "executing the patched page");
    int st; char b[128];
    pid_t w = waitpid(c.pid, &st, WNOHANG);
    INFO("H7.codepatch", "call rc=%d ; child status: %s ; EXC_BAD_ACCESS code[0]=%lld (KERN_CODESIGN_ERROR is 50), code[1]=0x%llx", rc, w == c.pid ? dp_status_str(st, b, sizeof b) : "alive", (long long)g_last.code[0], (unsigned long long)g_last.code[1]);
    task_set_exception_ports(g_task, EXC_MASK_ALL, MACH_PORT_NULL, EXCEPTION_DEFAULT, THREAD_STATE_NONE);
    finish_child(&c);
    g_task = save_task; di = save_di;
}

/* ---- PT_ATTACHEXC and signals as exceptions ------------------------------------------- */
static void sig_case(const char *id, int use_thupdate, int deliver)
{
    struct hi_child c; struct hi_dbg_info d; mach_port_t task;
    if (start_child(&c, &d, &task, id)) return;
    mach_port_t save_task = g_task; struct hi_dbg_info save_di = di;
    g_task = task; di = d; g_child_pid = c.pid;
    set_ports(g_task, EXCEPTION_STATE_IDENTITY);
    g_mode = MODE_SIG; g_seen = 0; g_use_thupdate = use_thupdate; g_deliver_sig = deliver;
    errno = 0;
    int rc = ptrace(PT_ATTACHEXC, c.pid, 0, 0);
    INFO(id, "ptrace(PT_ATTACHEXC) with an exception port on the task -> %d errno=%d", rc, errno);
    usleep(100000);
    show(id, "exception caused by the attach SIGSTOP");
    int st; char b[128];
    pid_t w = hi_wait_stop(c.pid, &st, 200);
    INFO(id, "waitpid(WUNTRACED) -> %d %s ; exceptions seen so far: %llu", (int)w, w > 0 ? dp_status_str(st, b, sizeof b) : "nothing", (unsigned long long)g_seen);
    g_seen = 0;
    kill(c.pid, SIGUSR1);
    usleep(150000);
    show(id, "exception caused by kill(SIGUSR1)");
    INFO(id, "EXC_SOFTWARE codes: code[0]=0x%llx (EXC_SOFT_SIGNAL is 0x10003) code[1]=%lld (signal number, SIGUSR1=%d) ; PT_THUPDATE(thread, sig=%d) %s ; exceptions seen: %llu",
         (unsigned long long)g_last.code[0], (long long)g_last.code[1], SIGUSR1, deliver, use_thupdate ? (g_last.x0 ? "FAILED" : "accepted") : "not used", (unsigned long long)g_seen);
    struct hi_msg rr, ping; memset(&ping, 0, sizeof ping); ping.cmd = DC_PING;
    hi_write_full(c.wfd, &ping, sizeof ping);
    int a = hi_read_timeout(c.rfd, &rr, sizeof rr, 800) == 0;
    pid_t ww = waitpid(c.pid, &st, WNOHANG);
    INFO(id, "after the handler replied KERN_SUCCESS: child %s ; wait status: %s", a ? "answers a PING (signal suppressed)" : "does not answer", ww == c.pid ? dp_status_str(st, b, sizeof b) : "alive");
    if (a) ptrace(PT_DETACH, c.pid, 0, 0);
    task_set_exception_ports(g_task, EXC_MASK_ALL, MACH_PORT_NULL, EXCEPTION_DEFAULT, THREAD_STATE_NONE);
    finish_child(&c);
    g_task = save_task; di = save_di;
}

static void t_sig(void)
{
    sig_case("H7.sig.plain", 0, 0);            /* reply only */
    sig_case("H7.sig.suppress", 1, 0);         /* PT_THUPDATE(thread, 0) then reply */
    sig_case("H7.sig.deliver", 1, SIGUSR1);    /* PT_THUPDATE(thread, SIGUSR1) then reply */
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "debuggee")) debuggee_main();
    dp_watchdog(150);
    signal(SIGPIPE, SIG_IGN);
    if (start_child(&ch, &di, &g_task, "H7.setup")) return dp_summary("h7_foreign_exc");
    g_exc_port = hi_exc_start(hook, NULL);
    INFO("H7.setup", "debuggee %d, tracer exception server on port 0x%x", (int)ch.pid, g_exc_port);
    t_basic();
    t_wprot();
    t_codepatch();
    t_step();
    t_hw();
    t_sig();
    finish_child(&ch);
    return dp_summary("h7_foreign_exc");
}
