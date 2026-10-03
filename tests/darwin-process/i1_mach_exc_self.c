/*
 * i1_mach_exc_self.c - I: exceptions and traps handled by the process ITSELF.
 *
 *   I1.register   task_set_exception_ports for EXC_BAD_ACCESS, EXC_BAD_INSTRUCTION, EXC_BREAKPOINT, EXC_SYSCALL (+MACH_SYSCALL)
 *                 with the three behaviours, and read them back; thread level against task level
 *   I1.trap       brk / udf / bad load / store to a read-only page: codes, state rewrite, resume (Mach handler thread)
 *   I1.sig        the same traps through BSD signals (sigaction, ucontext rewrite): signal numbers, si_code, si_addr
 *   I1.rtt        round trip of a trapping instruction: BSD signal against Mach handler thread (three behaviours)
 *   I1.wprot      write-protect fault repaired by the handler (mprotect), instruction retried
 *   I1.fall       a Mach handler that answers KERN_FAILURE: does the exception fall through to the BSD signal
 *   I1.thread     faults raised by a second thread reach the task-level handler; thread port beats task port
 */
#include "hi_util.h"
#include "hi_exc.h"
#include <sys/ucontext.h>

/* ---- trap instructions ---------------------------------------------------------- */
static inline uint64_t trap_brk(void)
{
    register uint64_t x0 __asm__("x0") = 1;
    __asm__ volatile("brk #0x77" : "+r"(x0) : : "memory");
    return x0;
}
static inline uint64_t trap_udf(void)
{
    register uint64_t x0 __asm__("x0") = 1;
    __asm__ volatile(".inst 0x00001234" : "+r"(x0) : : "memory");
    return x0;
}
static inline uint64_t trap_load(volatile void *p)
{
    register uint64_t x0 __asm__("x0") = 1;
    register uint64_t x1 __asm__("x1") = (uint64_t)(uintptr_t)p;
    __asm__ volatile("ldr x0, [x1]" : "+r"(x0) : "r"(x1) : "memory");
    return x0;
}
static inline void trap_store(volatile void *p)
{
    __asm__ volatile("str xzr, [%0]" : : "r"(p) : "memory");
}

/* ---- Mach side ------------------------------------------------------------------ */
enum { MODE_SKIP = 1, MODE_UNPROTECT, MODE_PASS };
static volatile int g_mode;
static volatile uint64_t g_seen;
static struct { int behavior, exc; int64_t code[2]; uint64_t pc; pthread_t handler_thread_is; } g_last;
static unsigned char *g_ro;
static mach_port_t g_exc_port;

static kern_return_t hook(struct hi_exc_event *ev, void *ctx)
{
    (void)ctx;
    g_last.behavior = ev->behavior; g_last.exc = ev->exception;
    g_last.code[0] = ev->code[0]; g_last.code[1] = ev->ncode > 1 ? ev->code[1] : 0;
    g_seen++;
    switch (g_mode) {
    case MODE_SKIP:
        if (ev->ts) {
            uintptr_t pc = (uintptr_t)arm_thread_state64_get_pc(*ev->ts);
            g_last.pc = pc;
            ev->ts->__x[0] = 0xBEEF;
            arm_thread_state64_set_pc_fptr(*ev->ts, (void *)(pc + 4));
        } else {
            arm_thread_state64_t st; mach_msg_type_number_t c = ARM_THREAD_STATE64_COUNT;
            thread_get_state(ev->thread, ARM_THREAD_STATE64, (thread_state_t)&st, &c);
            uintptr_t pc = (uintptr_t)arm_thread_state64_get_pc(st);
            g_last.pc = pc;
            st.__x[0] = 0xBEEF;
            arm_thread_state64_set_pc_fptr(st, (void *)(pc + 4));
            thread_set_state(ev->thread, ARM_THREAD_STATE64, (thread_state_t)&st, ARM_THREAD_STATE64_COUNT);
        }
        return KERN_SUCCESS;
    case MODE_UNPROTECT:
        mprotect(g_ro, HI_PAGE, PROT_READ | PROT_WRITE);
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
    case EXC_SOFTWARE: return "EXC_SOFTWARE"; case EXC_BREAKPOINT: return "EXC_BREAKPOINT"; case EXC_SYSCALL: return "EXC_SYSCALL"; case EXC_MACH_SYSCALL: return "EXC_MACH_SYSCALL";
    default: return "?";
    }
}

#define MASK_FOUR (EXC_MASK_BAD_ACCESS | EXC_MASK_BAD_INSTRUCTION | EXC_MASK_BREAKPOINT | EXC_MASK_SYSCALL)

static void install_task(int behavior)
{
    kern_return_t kr = task_set_exception_ports(mach_task_self(), MASK_FOUR | EXC_MASK_MACH_SYSCALL, g_exc_port, behavior | MACH_EXCEPTION_CODES, ARM_THREAD_STATE64);
    if (kr) INFO("I1.register", "task_set_exception_ports(behavior=%d) -> %s", behavior, hi_kr_str(kr));
}
static void remove_task(void)
{
    task_set_exception_ports(mach_task_self(), EXC_MASK_ALL, MACH_PORT_NULL, EXCEPTION_DEFAULT, THREAD_STATE_NONE);
}

/* ---- signal side ----------------------------------------------------------------- */
static volatile int g_sig_seen, g_sig_no, g_sig_code;
static volatile uint64_t g_sig_addr;
static void sig_handler(int sig, siginfo_t *si, void *uctx)
{
    ucontext_t *uc = uctx;
    g_sig_seen++; g_sig_no = sig; g_sig_code = si->si_code; g_sig_addr = (uint64_t)(uintptr_t)si->si_addr;
    uintptr_t pc = (uintptr_t)arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
    uc->uc_mcontext->__ss.__x[0] = 0xBEEF;
    arm_thread_state64_set_pc_fptr(uc->uc_mcontext->__ss, (void *)(pc + 4));
}
static void sig_unprotect(int sig, siginfo_t *si, void *uctx)
{
    (void)uctx;
    g_sig_seen++; g_sig_no = sig; g_sig_code = si->si_code; g_sig_addr = (uint64_t)(uintptr_t)si->si_addr;
    mprotect(g_ro, HI_PAGE, PROT_READ | PROT_WRITE);
}
static void install_signals(void (*h)(int, siginfo_t *, void *))
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = h; sa.sa_flags = SA_SIGINFO; sigemptyset(&sa.sa_mask);
    sigaction(SIGTRAP, &sa, NULL); sigaction(SIGILL, &sa, NULL); sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
}
static void default_signals(void)
{
    signal(SIGTRAP, SIG_DFL); signal(SIGILL, SIG_DFL); signal(SIGSEGV, SIG_DFL); signal(SIGBUS, SIG_DFL);
}

/* ---- tests -------------------------------------------------------------------------- */
static void t_register(void)
{
    struct { int b; const char *n; } bs[] = {{EXCEPTION_DEFAULT, "DEFAULT"}, {EXCEPTION_STATE, "STATE"}, {EXCEPTION_STATE_IDENTITY, "STATE_IDENTITY"}};
    for (int i = 0; i < 3; i++) {
        kern_return_t kr = task_set_exception_ports(mach_task_self(), MASK_FOUR | EXC_MASK_MACH_SYSCALL, g_exc_port, bs[i].b | MACH_EXCEPTION_CODES, ARM_THREAD_STATE64);
        exception_mask_t masks[EXC_TYPES_COUNT]; mach_msg_type_number_t cnt = EXC_TYPES_COUNT;
        mach_port_t ports[EXC_TYPES_COUNT]; exception_behavior_t beh[EXC_TYPES_COUNT]; thread_state_flavor_t fl[EXC_TYPES_COUNT];
        kern_return_t kg = task_get_exception_ports(mach_task_self(), MASK_FOUR | EXC_MASK_MACH_SYSCALL, masks, &cnt, ports, beh, fl);
        char list[400]; int off = 0;
        for (unsigned j = 0; j < cnt; j++) off += snprintf(list + off, sizeof list - off, " [mask=0x%x port=0x%x behavior=0x%x flavor=%d]", masks[j], ports[j], beh[j], fl[j]);
        INFO("I1.register", "task_set_exception_ports(BAD_ACCESS|BAD_INSTRUCTION|BREAKPOINT|SYSCALL|MACH_SYSCALL, behavior %s|MACH_EXCEPTION_CODES) -> %s ; read back (%s):%s", bs[i].n, hi_kr_str(kr), hi_kr_str(kg), list);
        CHECK(kr == 0, "I1.register", "EXC_SYSCALL and EXC_MACH_SYSCALL are accepted in the mask together with the three others (%s)", bs[i].n);
    }
    remove_task();
    /* thread level for the current thread only */
    kern_return_t kr = thread_set_exception_ports(mach_thread_self(), MASK_FOUR, g_exc_port, EXCEPTION_STATE | MACH_EXCEPTION_CODES, ARM_THREAD_STATE64);
    INFO("I1.register", "thread_set_exception_ports on the calling thread -> %s", hi_kr_str(kr));
    thread_set_exception_ports(mach_thread_self(), EXC_MASK_ALL, MACH_PORT_NULL, EXCEPTION_DEFAULT, THREAD_STATE_NONE);
}

static void t_trap(void)
{
    struct { int b; const char *n; } bs[] = {{EXCEPTION_STATE_IDENTITY, "STATE_IDENTITY"}, {EXCEPTION_STATE, "STATE"}, {EXCEPTION_DEFAULT, "DEFAULT"}};
    for (int i = 0; i < 3; i++) {
        install_task(bs[i].b);
        char id[40]; snprintf(id, sizeof id, "I1.trap.%s", bs[i].n);
        g_mode = MODE_SKIP;
        g_seen = 0; uint64_t r = trap_brk();
        INFO(id, "brk #0x77 -> %s codes=[0x%llx, 0x%llx] pc=0x%llx behavior=%d, x0 after resume=0x%llx", exc_name(g_last.exc), (unsigned long long)g_last.code[0], (unsigned long long)g_last.code[1], (unsigned long long)g_last.pc, g_last.behavior, (unsigned long long)r);
        CHECK(r == 0xBEEF && g_seen == 1 && g_last.exc == EXC_BREAKPOINT, id, "brk: handler thread rewrote x0 and pc, the faulting thread resumed with the new state");
        g_seen = 0; r = trap_udf();
        INFO(id, "udf #0x1234 -> %s codes=[0x%llx, 0x%llx] pc=0x%llx, x0=0x%llx", exc_name(g_last.exc), (unsigned long long)g_last.code[0], (unsigned long long)g_last.code[1], (unsigned long long)g_last.pc, (unsigned long long)r);
        CHECK(r == 0xBEEF && g_last.exc == EXC_BAD_INSTRUCTION, id, "undefined instruction handled");
        g_seen = 0; r = trap_load((void *)0x10);
        INFO(id, "ldr x0,[0x10] -> %s codes=[0x%llx, 0x%llx] pc=0x%llx, x0=0x%llx", exc_name(g_last.exc), (unsigned long long)g_last.code[0], (unsigned long long)g_last.code[1], (unsigned long long)g_last.pc, (unsigned long long)r);
        CHECK(r == 0xBEEF && g_last.exc == EXC_BAD_ACCESS, id, "bad load handled and emulated");
        remove_task();
    }
}

static void t_sig(void)
{
    install_signals(sig_handler);
    g_sig_seen = 0; uint64_t r = trap_brk();
    INFO("I1.sig", "brk #0x77 -> signal %d (%s) si_code=%d, x0 after return=0x%llx", g_sig_no, strsignal(g_sig_no), g_sig_code, (unsigned long long)r);
    CHECK(r == 0xBEEF && g_sig_no == SIGTRAP, "I1.sig", "brk gives SIGTRAP, handler edits ucontext (pc+4, x0) and sigreturn resumes with it");
    g_sig_seen = 0; r = trap_udf();
    INFO("I1.sig", "udf #0x1234 -> signal %d (%s) si_code=%d si_addr=0x%llx, x0=0x%llx", g_sig_no, strsignal(g_sig_no), g_sig_code, (unsigned long long)g_sig_addr, (unsigned long long)r);
    g_sig_seen = 0; r = trap_load((void *)0x10);
    INFO("I1.sig", "ldr [0x10] (unmapped) -> signal %d (%s) si_code=%d (SEGV_MAPERR=%d SEGV_ACCERR=%d) si_addr=0x%llx, x0=0x%llx", g_sig_no, strsignal(g_sig_no), g_sig_code, SEGV_MAPERR, SEGV_ACCERR, (unsigned long long)g_sig_addr, (unsigned long long)r);
    install_signals(sig_unprotect);
    mprotect(g_ro, HI_PAGE, PROT_READ);
    g_sig_seen = 0; trap_store(g_ro);
    INFO("I1.sig", "str to a PROT_READ page -> signal %d (%s) si_code=%d (SEGV_ACCERR=%d) si_addr=0x%llx (page 0x%llx)", g_sig_no, strsignal(g_sig_no), g_sig_code, SEGV_ACCERR, (unsigned long long)g_sig_addr, (unsigned long long)(uintptr_t)g_ro);
    default_signals();
}

static void bench_sig(const char *label, uint64_t (*fn)(void), int n)
{
    uint64_t *v = malloc(sizeof(uint64_t) * n);
    for (int i = 0; i < n; i++) { uint64_t t0 = dp_ns(); (void)fn(); v[i] = dp_ns() - t0; }
    hi_report_us("I1.rtt", label, v, n);
    free(v);
}
static uint64_t f_brk(void) { return trap_brk(); }
static uint64_t f_udf(void) { return trap_udf(); }
static uint64_t f_load(void) { return trap_load((void *)0x10); }

static void t_rtt(void)
{
    install_signals(sig_handler);
    bench_sig("BSD signal : brk  -> SIGTRAP handler -> sigreturn", f_brk, 100000);
    bench_sig("BSD signal : udf  -> SIGILL  handler -> sigreturn", f_udf, 100000);
    bench_sig("BSD signal : bad ldr -> SIGSEGV handler -> sigreturn", f_load, 100000);
    default_signals();
    struct { int b; const char *n; } bs[] = {{EXCEPTION_STATE, "STATE"}, {EXCEPTION_STATE_IDENTITY, "STATE_IDENTITY"}, {EXCEPTION_DEFAULT, "DEFAULT"}};
    for (int i = 0; i < 3; i++) {
        install_task(bs[i].b);
        g_mode = MODE_SKIP;
        char label[100];
        snprintf(label, sizeof label, "Mach %-14s : brk  -> handler thread -> reply", bs[i].n);
        bench_sig(label, f_brk, 30000);
        snprintf(label, sizeof label, "Mach %-14s : udf  -> handler thread -> reply", bs[i].n);
        bench_sig(label, f_udf, 30000);
        snprintf(label, sizeof label, "Mach %-14s : bad ldr -> handler thread -> reply", bs[i].n);
        bench_sig(label, f_load, 30000);
        remove_task();
    }
    /* thread level port on the faulting thread, STATE behaviour */
    thread_set_exception_ports(mach_thread_self(), MASK_FOUR, g_exc_port, EXCEPTION_STATE | MACH_EXCEPTION_CODES, ARM_THREAD_STATE64);
    g_mode = MODE_SKIP;
    bench_sig("Mach STATE (thread-level port)  : brk  -> handler thread -> reply", f_brk, 30000);
    thread_set_exception_ports(mach_thread_self(), EXC_MASK_ALL, MACH_PORT_NULL, EXCEPTION_DEFAULT, THREAD_STATE_NONE);
}

static void t_wprot(void)
{
    enum { N = 20000 };
    uint64_t *v = malloc(sizeof(uint64_t) * N);
    install_signals(sig_unprotect);
    for (int i = 0; i < N; i++) {
        mprotect(g_ro, HI_PAGE, PROT_READ);
        uint64_t t0 = dp_ns(); trap_store(g_ro); v[i] = dp_ns() - t0;
    }
    hi_report_us("I1.wprot", "BSD signal : store fault, handler mprotect(RW), retry", v, N);
    default_signals();
    uint64_t *m = malloc(sizeof(uint64_t) * N);
    for (int i = 0; i < N; i++) { uint64_t t0 = dp_ns(); mprotect(g_ro, HI_PAGE, PROT_READ | PROT_WRITE); m[i] = dp_ns() - t0; }
    hi_report_us("I1.wprot", "reference: one mprotect(RW) call on one 16 KiB page", m, N);
    install_task(EXCEPTION_STATE);
    g_mode = MODE_UNPROTECT; g_seen = 0;
    for (int i = 0; i < N; i++) {
        mprotect(g_ro, HI_PAGE, PROT_READ);
        uint64_t t0 = dp_ns(); trap_store(g_ro); v[i] = dp_ns() - t0;
    }
    hi_report_us("I1.wprot", "Mach STATE   : store fault, handler mprotect(RW), retry", v, N);
    CHECK(g_seen >= N, "I1.wprot", "every store faulted and was repaired (%llu exceptions)", (unsigned long long)g_seen);
    remove_task();
    mprotect(g_ro, HI_PAGE, PROT_READ | PROT_WRITE);
    free(v); free(m);
}

static void t_fall(void)
{
    install_task(EXCEPTION_STATE);
    g_mode = MODE_PASS; g_seen = 0;
    install_signals(sig_handler);
    g_sig_seen = 0;
    uint64_t r = trap_brk();
    INFO("I1.fall", "Mach handler answered KERN_FAILURE for brk: exceptions seen by the handler=%llu, BSD signal handler ran %d time(s) with signal %d, x0=0x%llx", (unsigned long long)g_seen, g_sig_seen, g_sig_no, (unsigned long long)r);
    CHECK(g_seen == 1 && g_sig_seen == 1 && r == 0xBEEF, "I1.fall", "a Mach handler can decline (KERN_FAILURE) and the exception falls through to the BSD signal handler");
    default_signals();
    remove_task();
}

static void *thread_trap(void *arg)
{
    uint64_t r = trap_brk();
    *(uint64_t *)arg = r;
    return NULL;
}

static void t_thread(void)
{
    install_task(EXCEPTION_STATE);
    g_mode = MODE_SKIP; g_seen = 0;
    uint64_t r = 0; pthread_t t;
    pthread_create(&t, NULL, thread_trap, &r);
    pthread_join(t, NULL);
    CHECK(r == 0xBEEF && g_seen == 1, "I1.thread", "a brk in a thread created AFTER the registration reached the task-level handler (inherits the port)");
    /* thread port beats task port: register a second server for this thread */
    static mach_port_t second;
    mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &second);
    mach_port_insert_right(mach_task_self(), second, second, MACH_MSG_TYPE_MAKE_SEND);
    kern_return_t kr = thread_set_exception_ports(mach_thread_self(), EXC_MASK_BREAKPOINT, second, EXCEPTION_STATE | MACH_EXCEPTION_CODES, ARM_THREAD_STATE64);
    INFO("I1.thread", "thread_set_exception_ports(second port, nobody listens) -> %s ; a brk now should go to the thread port and block: not executed (would hang); skipped", hi_kr_str(kr));
    thread_set_exception_ports(mach_thread_self(), EXC_MASK_ALL, MACH_PORT_NULL, EXCEPTION_DEFAULT, THREAD_STATE_NONE);
    remove_task();
}

int main(void)
{
    dp_watchdog(150);
    g_ro = mmap(NULL, HI_PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    memset(g_ro, 0, HI_PAGE);
    g_exc_port = hi_exc_start(hook, NULL);
    t_register();
    t_trap();
    t_sig();
    t_fall();
    t_thread();
    t_wprot();
    t_rtt();
    return dp_summary("i1_mach_exc_self");
}
