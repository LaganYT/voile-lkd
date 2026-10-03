/*
 * i2_svc_probe.c - I: what does this kernel do with an arm64 'svc' that is not the Darwin convention (svc #0x80, number in x16)?
 *
 * This decides whether an UNMODIFIED arm64 Linux binary (svc #0, number in x8) could ever have its system calls
 * intercepted by a normal macOS process, without HVF.
 *
 * Every probe runs in its own forked child, under alarm(3), with BSD signal handlers and a Mach exception port
 * (EXC_SYSCALL, EXC_MACH_SYSCALL, BAD_INSTRUCTION, BAD_ACCESS, BREAKPOINT, SOFTWARE) that only LOG and decline (KERN_FAILURE).
 * The system call number put in x16 is always either 20 (getpid) or a value outside the system call tables, so that no
 * dangerous call can run: 0x7fffff00 (BSD table is far smaller) and -1000 (Mach trap table is far smaller).
 * x0 is set to 7 and x8 to 93 (Linux exit) before the svc: if the kernel looked at x8 instead of x16 the child would exit(7).
 */
#include "hi_util.h"
#include "hi_exc.h"
#include <sys/ucontext.h>

struct svc_ret { long x0; long carry; };

#define SVC_PROBE(name, imm) \
    __asm__(".text\n.p2align 2\n.globl _" #name "\n_" #name ":\n" \
            "  mov x16, x0\n  mov x8, x1\n  mov x0, #7\n  mov x1, #0\n  svc #" #imm "\n" \
            "  cset x1, cs\n  ret\n"); \
    extern struct svc_ret name(long x16, long x8);

SVC_PROBE(probe_svc_0, 0)
SVC_PROBE(probe_svc_1, 1)
SVC_PROBE(probe_svc_80, 0x80)
SVC_PROBE(probe_svc_1234, 0x1234)
SVC_PROBE(probe_svc_ffff, 0xffff)

struct variant {
    const char *name;
    struct svc_ret (*fn)(long, long);
    const char *imm;
    long x16, x8;
    const char *why;
};

static const struct variant variants[] = {
    {"control-svc80-getpid", probe_svc_80, "0x80", 20, 93, "Darwin convention, x16=20 getpid; x8=93 (Linux exit) must be ignored"},
    {"svc0-x16-getpid", probe_svc_0, "0", 20, 93, "Linux immediate, Darwin number in x16"},
    {"svc1-x16-getpid", probe_svc_1, "1", 20, 93, "immediate 1"},
    {"svc1234-x16-getpid", probe_svc_1234, "0x1234", 20, 93, "immediate 0x1234"},
    {"svcffff-x16-getpid", probe_svc_ffff, "0xffff", 20, 93, "immediate 0xffff"},
    {"svc0-x16-bsd-oor", probe_svc_0, "0", 0x7fffff00L, 172, "svc #0 with a BSD number far outside the table (what a Linux binary with garbage in x16 would do)"},
    {"svc80-x16-bsd-oor", probe_svc_80, "0x80", 0x7fffff00L, 172, "same with svc #0x80"},
    {"svc0-x16-mach-oor", probe_svc_0, "0", -1000L, 172, "svc #0 with a Mach trap number far outside the table"},
    {"svc80-x16-mach-oor", probe_svc_80, "0x80", -1000L, 172, "same with svc #0x80"},
};

/* ---- child ------------------------------------------------------------------ */
static const struct variant *g_cur;

static void sig_handler(int sig, siginfo_t *si, void *uctx)
{
    ucontext_t *uc = uctx;
    uintptr_t pc = (uintptr_t)arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
    INFO("I2.signal", "%s: caught signal %d (%s) si_code=%d si_addr=0x%llx pc=0x%llx x0=0x%llx x8=%llu x16=0x%llx", g_cur->name, sig, strsignal(sig), si->si_code, (unsigned long long)(uintptr_t)si->si_addr,
         (unsigned long long)pc, (unsigned long long)uc->uc_mcontext->__ss.__x[0], (unsigned long long)uc->uc_mcontext->__ss.__x[8], (unsigned long long)uc->uc_mcontext->__ss.__x[16]);
    _exit(100 + sig);
}

static kern_return_t hook(struct hi_exc_event *ev, void *ctx)
{
    (void)ctx;
    INFO("I2.mach", "%s: Mach exception type=%d code0=0x%llx code1=0x%llx (EXC_SYSCALL=%d EXC_MACH_SYSCALL=%d EXC_BAD_INSTRUCTION=%d EXC_BAD_ACCESS=%d EXC_BREAKPOINT=%d EXC_SOFTWARE=%d), declined", g_cur->name, ev->exception,
         (unsigned long long)ev->code[0], (unsigned long long)ev->code[1], EXC_SYSCALL, EXC_MACH_SYSCALL, EXC_BAD_INSTRUCTION, EXC_BAD_ACCESS, EXC_BREAKPOINT, EXC_SOFTWARE);
    return KERN_FAILURE;
}

static int child_main(int idx, int with_mach)
{
    dp_role = "child";
    g_cur = &variants[idx];
    alarm(4);
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = sig_handler; sa.sa_flags = SA_SIGINFO; sigemptyset(&sa.sa_mask);
    int sigs[] = {SIGSYS, SIGILL, SIGTRAP, SIGSEGV, SIGBUS, SIGFPE, SIGEMT};
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) sigaction(sigs[i], &sa, NULL);
    if (with_mach) {
        mach_port_t port = hi_exc_start(hook, NULL);
        kern_return_t kr = task_set_exception_ports(mach_task_self(), EXC_MASK_SYSCALL | EXC_MASK_MACH_SYSCALL | EXC_MASK_BAD_INSTRUCTION | EXC_MASK_BAD_ACCESS | EXC_MASK_BREAKPOINT | EXC_MASK_SOFTWARE,
                                                    port, EXCEPTION_STATE | MACH_EXCEPTION_CODES, ARM_THREAD_STATE64);
        if (kr) INFO("I2.setup", "%s: task_set_exception_ports -> %s", g_cur->name, hi_kr_str(kr));
    }
    INFO("I2.probe", "%s: executing svc #%s with x16=%ld x8=%ld x0=7 (%s)", g_cur->name, g_cur->imm, g_cur->x16, g_cur->x8, g_cur->why);
    struct svc_ret r = g_cur->fn(g_cur->x16, g_cur->x8);
    INFO("I2.result", "%s: the svc RETURNED to user mode: x0=%ld (0x%lx) carry=%ld ; getpid()=%d", g_cur->name, r.x0, r.x0, r.carry, (int)getpid());
    return 0;
}

/* ---- parent ----------------------------------------------------------------- */
int main(int argc, char **argv)
{
    if (argc > 3 && !strcmp(argv[1], "child")) return child_main(atoi(argv[2]), atoi(argv[3]));
    dp_watchdog(120);
    for (int with_mach = 0; with_mach < 2; with_mach++) {
        INFO("I2.mode", "=== %s ===", with_mach ? "with a Mach exception port for SYSCALL, MACH_SYSCALL, BAD_INSTRUCTION, BAD_ACCESS, BREAKPOINT, SOFTWARE plus BSD signal handlers" : "BSD signal handlers only");
        for (size_t i = 0; i < sizeof variants / sizeof variants[0]; i++) {
            pid_t pid = fork();
            if (pid == 0) {
                char a[16], b[16]; snprintf(a, sizeof a, "%zu", i); snprintf(b, sizeof b, "%d", with_mach);
                execl(dp_self_path(), dp_self_path(), "child", a, b, (char *)NULL);
                _exit(127);
            }
            dp_track(pid);
            int st = 0; char buf[128];
            pid_t w = dp_wait_timeout(pid, &st, 6000);
            if (w == pid) INFO("I2.exit", "%s: child ended with %s", variants[i].name, dp_status_str(st, buf, sizeof buf));
            else INFO("I2.exit", "%s: child had to be killed after 6 s (hang)", variants[i].name);
            if (w == pid && WIFEXITED(st) && WEXITSTATUS(st) == 7) FAIL("I2.exit", "%s: child exited with 7: the kernel executed Linux exit from x8", variants[i].name);
        }
    }
    return dp_summary("i2_svc_probe");
}
