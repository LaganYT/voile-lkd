/* test_ptrace_eac.c: the request sequence EasyAntiCheat issues against its child, end to end between two
 * real host processes (reports/eac-probes-digest.md section 3.2): the tracee is a forked guest process that
 * calls PTRACE_TRACEME and sends itself SIGSTOP; the tracer sets options 0x11, single-steps through
 * syscall stops reading NT_PRSTATUS (216 bytes), sees the exec event and the exit of execve, pokes an INT3
 * into the new image, reads the SIGTRAP stop, restores the byte and rewinds RIP, saves registers and FP
 * state, writes the path of a library and a return address of 9 on the tracee's stack, rewrites registers,
 * resumes, sees SIGSEGV with si_addr 9, restores everything, drops the signal with CONT(0), and finally
 * delivers a SIGSEGV with CONT(sig), which kills the tracee.
 *
 * The tracee plays the engine: its registers are the fake CPU's, its memory is a copy-on-write copy of the
 * tracer's, and the POKE reaches it through the mm service of its own host process, which must call the
 * engine's invalidate_code before the tracee runs again.
 * Linux behaviour modelled: ptrace(2) PTRACE_TRACEME, PTRACE_O_TRACEEXEC, PTRACE_SYSCALL, PTRACE_GETREGSET,
 * PTRACE_GETSIGINFO, PTRACE_POKEDATA, PTRACE_SETREGSET (kernel/ptrace.c, arch/x86/kernel/ptrace.c v6.12);
 * kernel/exit.c wait_task_stopped() for the wait statuses. */
#include "test_ptrace_util.h"

#define ENTRY_OFF 0x1f0ull
#define RW (LX_PROT_READ | LX_PROT_WRITE)
#define PA (LX_MAP_PRIVATE | LX_MAP_ANONYMOUS)

static uint64_t g_code, g_stack;
static int g_wfd;
static const uint64_t ORIG_WORD = 0x89485ed18949ed31ull;   /* 31 ed 49 89 d1 5e 48 89: _start of a program */
static const uint64_t INT3_WORD = 0x89485ed18949edccull;

static void child_die(lkd_task *t)
{
    int st = 0;
    if (lkd_task_release(t, &st)) _exit(st);
    _exit(98);
}

/* The tracee. Every value it reports is read back by the tracer at the end. */
static void eac_child(lkd_task *t)
{
    int died = 0;
    struct fake_cpu *c = fake_cpu_of(t);
    cpu_init(t);
    fake_invalidate_count = 0;      /* the count inherited from the parent's mprotect is not ours */
    int fd = g_wfd;
    put64(fd, gsys(t, LKD_NR_ptrace, LX_PTRACE_TRACEME, 0, 0, 0, 0, 0, &died));     /* [0] */
    died = self_signal(t, LX_SIGSTOP);                                                /* stop 1 */
    if (died) child_die(t);
    put64(fd, gsys(t, LKD_NR_getpid, 0, 0, 0, 0, 0, 0, &died));                       /* [1] */
    put64(fd, gsys(t, LKD_NR_gettid, 0, 0, 0, 0, 0, 0, &died));                       /* [2] */
    /* execve: the engine has loaded the new image (RIP = entry, RAX = 0, EFLAGS 0x202, the rest zero) */
    uint64_t entry = g_code + ENTRY_OFF;
    int32_t old_tid = t->tid;
    memset(&c->regs, 0, sizeof c->regs);
    c->regs.rip = entry; c->regs.eflags = 0x202; c->regs.cs = 0x33; c->regs.ss = 0x2b;
    c->regs.orig_rax = 59;
    atomic_store(&t->cur_nr, 59);
    (void)lkd_ptrace_event(t, LX_PTRACE_EVENT_EXEC, (uint64_t)old_tid);               /* event stop */
    /* the tail of lkd_syscall for LKD_RET_CONTEXT_REPLACED: the exit stop of execve (dispatch.c) */
    if (atomic_load(&t->ptrace.flags) & LKD_PT_SYSCALL_TRACE) lkd_ptrace_syscall_exit_replaced(t);
    atomic_store(&t->cur_nr, -1);
    /* "run" the new image: the first byte at the entry is what the tracer left there */
    uint8_t b = *(uint8_t *)lkd_g2h(entry);
    put64(fd, b);                                                                      /* [3] */
    put64(fd, fake_invalidate_count);                                                  /* [4] */
    c->regs.eflags = 0x246;
    c->regs.rsp = g_stack + 0x1000;
    c->regs.rbx = 0x1111; c->regs.rbp = 0x2222; c->regs.r12 = 0x3333;
    c->regs.fs_base = 0x7f0000001000ull;
    if (b == 0xcc) {
        c->regs.rip = entry + 1;                                                       /* int3 traps after the byte */
        lkd_fault(t, LX_SIGTRAP, LX_SI_KERNEL, 0, 0, 3);
        if (safe_point(t) < 0) child_die(t);                                          /* stop 2: SIGTRAP */
    }
    /* back from the SIGTRAP stop: the tracer rewrote the registers to call the injected function */
    put64(fd, (int64_t)c->regs.rip);                                                   /* [5] */
    put64(fd, (int64_t)c->regs.rdi);                                                   /* [6] */
    uint64_t w = 0;
    if (c->regs.rdi) lkd_user_read(c->regs.rdi, &w, 8);
    put64(fd, (int64_t)w);                                                             /* [7] path word */
    uint64_t ra = 0;
    lkd_user_read(c->regs.rsp, &ra, 8);
    put64(fd, (int64_t)ra);                                                            /* [8] return address 9 */
    /* the injected call returns to address 9: page fault, SIGSEGV with si_addr 9 */
    c->regs.rax = 0x1234;
    c->fp.mxcsr = 0x1fa0;
    memcpy(c->xsave + 24, &c->fp.mxcsr, 4);
    c->regs.rip = 9;
    lkd_fault(t, LX_SIGSEGV, LX_SEGV_MAPERR, 9, 4, 14);
    if (safe_point(t) < 0) child_die(t);                                              /* stop 3: SIGSEGV, dropped */
    put64(fd, (int64_t)c->regs.rip);                                                   /* [9] restored */
    put64(fd, (int64_t)c->regs.rax);                                                   /* [10] */
    uint32_t mx; memcpy(&mx, c->xsave + 24, 4);
    put64(fd, mx);                                                                     /* [11] FP restored */
    put64(fd, c->regs.rbx);                                                            /* [12] */
    /* a second fault, delivered by the tracer: SIGSEGV with the default action ends the process */
    c->regs.rip = 9;
    lkd_fault(t, LX_SIGSEGV, LX_SEGV_MAPERR, 9, 4, 14);
    if (safe_point(t) < 0) { put64(fd, 55); child_die(t); }                           /* stop 4, then death */
    put64(fd, 66);
    child_die(t);
}

LT_TEST(eac_sequence)
{
    int64_t code = sct(TR, LKD_NR_mmap, 0, 0x2000, RW, PA, (uint64_t)-1, 0);
    int64_t stack = sct(TR, LKD_NR_mmap, 0, 0x2000, RW, PA, (uint64_t)-1, 0);
    LT_CHECK(code > 0 && stack > 0);
    g_code = (uint64_t)code; g_stack = (uint64_t)stack;
    *(uint64_t *)lkd_g2h(g_code + ENTRY_OFF) = ORIG_WORD;
    LT_EQ(sct(TR, LKD_NR_mprotect, g_code, 0x2000, LX_PROT_READ | LX_PROT_EXEC, 0, 0, 0), 0);
    int rp[2];
    LT_EQ(pipe(rp), 0);
    g_wfd = rp[1];
    fflush(NULL);
    int64_t pid = sct(TR, LKD_NR_fork, 0, 0, 0, 0, 0, 0);
    if (pid == 0) {
        alarm(40);
        eac_child(lkd_current());
        _exit(97);
    }
    LT_CHECK(pid > 0);
    close(rp[1]);
    int32_t child = (int32_t)pid;
    struct lx_user_regs r, saved;
    struct lx_siginfo si;
    uint64_t msg = 0;
    int64_t n;

    /* wait4: the first stop is the tgkill(SIGSTOP) the child sent itself: 0x137f (a signal-delivery-stop).
     * waitid(WNOWAIT) peeks without consuming (CLD_TRAPPED), wait4 consumes. */
    struct lx_siginfo wi;
    memset(&wi, 0, sizeof wi);
    LT_EQ(sct(TR, LKD_NR_waitid, LX_P_PID, (uint64_t)child, P(&wi), LX_WSTOPPED | LX_WNOWAIT, 0, 0), 0);
    LT_EQ(wi.si_signo, LX_SIGCHLD);
    LT_EQ(wi.si_code, LX_CLD_TRAPPED);
    LT_EQ(wi.u.chld.pid, child);
    LT_CHECK(expect_stop(child, 0x137f));
    int st = 0;
    LT_EQ(wait_pid(child, &st, LX_WNOHANG), 0);                    /* consumed: nothing new to report */
    LT_EQ(lkd_ptrace_tracer_of(child), TR->tid);                   /* TracerPid */
    LT_EQ(PT(LX_PTRACE_GETSIGINFO, child, 0, P(&si)), 0);
    LT_EQ(si.si_signo, 19);
    LT_EQ(si.si_code, LX_SI_TKILL);                                /* -6 */
    LT_EQ(si.u.kill.pid, child);
    LT_EQ(PT(LX_PTRACE_SETOPTIONS, child, 0, LX_PTRACE_O_TRACESYSGOOD | LX_PTRACE_O_TRACEEXEC), 0);

    /* PTRACE_SYSCALL loop: getpid and gettid, entry and exit stops, 216-byte NT_PRSTATUS at each */
    const uint64_t nr[2] = { 39, 186 };
    for (int k = 0; k < 2; k++) {
        for (int phase = 0; phase < 2; phase++) {
            LT_EQ(PT(LX_PTRACE_SYSCALL, child, 0, 0), 0);
            LT_CHECK(expect_stop(child, 0x857f));
            get_regs216(child, &r, &n);
            LT_EQ(n, 216);
            LT_EQ(r.orig_rax, nr[k]);
            if (phase == 0) { LT_EQ(r.rax, (uint64_t)-38); LT_EQ(r.rcx, r.rip); LT_EQ(r.r11, r.eflags); }
            else LT_EQ(r.rax, k == 0 ? (uint64_t)child : (uint64_t)child);
            LT_EQ(PT(LX_PTRACE_GETEVENTMSG, child, 0, P(&msg)), 0);
            LT_EQ(msg, (uint64_t)(phase + 1));
        }
    }
    /* CONT: the child runs to the exec event without syscall stops */
    LT_EQ(PT(LX_PTRACE_CONT, child, 0, 0), 0);
    LT_CHECK(expect_stop(child, 0x4057f));                         /* PTRACE_EVENT_EXEC */
    LT_EQ(PT(LX_PTRACE_GETEVENTMSG, child, 0, P(&msg)), 0);
    LT_EQ(msg, (uint64_t)child);                                   /* the former thread id */
    LT_EQ(PT(LX_PTRACE_GETSIGINFO, child, 0, P(&si)), 0);
    LT_EQ(si.si_signo, 5);
    LT_EQ(si.si_code, 0x405);
    /* the first stop after exec is the exit of execve (0x857f, message 2), RIP = entry of the new image */
    LT_EQ(PT(LX_PTRACE_SYSCALL, child, 0, 0), 0);
    LT_CHECK(expect_stop(child, 0x857f));
    LT_EQ(PT(LX_PTRACE_GETEVENTMSG, child, 0, P(&msg)), 0);
    LT_EQ(msg, 2);
    get_regs216(child, &r, &n);
    LT_EQ(n, 216);
    LT_EQ(r.orig_rax, 59);
    LT_EQ(r.rax, 0);
    LT_EQ(r.rip, g_code + ENTRY_OFF);
    LT_EQ(r.eflags, 0x202);
    LT_EQ(r.fs_base, 0);
    LT_EQ(r.gs_base, 0);
    LT_EQ(r.rcx, 0);                                               /* execve returns by IRET: not patched */
    /* PEEK the entry, POKE the INT3 into the read-only code page of the tracee, CONT */
    uint64_t w = 0;
    LT_EQ(PT(LX_PTRACE_PEEKDATA, child, g_code + ENTRY_OFF, P(&w)), 0);
    LT_EQ(w, ORIG_WORD);
    LT_EQ(PT(LX_PTRACE_POKEDATA, child, g_code + ENTRY_OFF, INT3_WORD), 0);
    LT_EQ(PT(LX_PTRACE_CONT, child, 0, 0), 0);
    /* the INT3 executes: SIGTRAP, status 0x57f, si_code SI_KERNEL (0x80), si_addr 0, RIP = INT3 + 1 */
    LT_CHECK(expect_stop(child, 0x57f));
    LT_EQ(PT(LX_PTRACE_GETSIGINFO, child, 0, P(&si)), 0);
    LT_EQ(si.si_signo, 5);
    LT_EQ(si.si_code, 128);
    LT_EQ(si.u.fault.addr, 0);
    get_regs216(child, &r, &n);
    LT_EQ(n, 216);
    saved = r;
    LT_EQ(r.rip, g_code + ENTRY_OFF + 1);
    LT_EQ(r.orig_rax, (uint64_t)-1);
    LT_EQ(r.cs, 0x33);
    LT_EQ(r.ss, 0x2b);
    LT_EQ(r.eflags, 0x246);
    LT_EQ(r.fs_base, 0x7f0000001000ull);
    /* SETREGSET with RIP - 1 and nothing else changed, then restore the original byte */
    struct lx_user_regs w2 = r;
    w2.rip = g_code + ENTRY_OFF;
    struct lx_iovec iov = { P(&w2), sizeof w2 };
    LT_EQ(PT(LX_PTRACE_SETREGSET, child, LX_NT_PRSTATUS, P(&iov)), 0);
    LT_EQ(PT(LX_PTRACE_POKEDATA, child, g_code + ENTRY_OFF, ORIG_WORD), 0);
    LT_EQ(PT(LX_PTRACE_PEEKDATA, child, g_code + ENTRY_OFF, P(&w)), 0);
    LT_EQ(w, ORIG_WORD);
    /* read the registers back: the rewind is visible */
    get_regs216(child, &r, &n);
    LT_EQ(r.rip, g_code + ENTRY_OFF);
    saved = r;
    /* save the FP state, then prepare the injected call: path on the stack, return address 9 */
    uint8_t fp[512], fp2[512];
    iov = (struct lx_iovec){ P(fp), 512 };
    LT_EQ(PT(LX_PTRACE_GETREGSET, child, LX_NT_PRFPREG, P(&iov)), 0);
    LT_EQ(iov.iov_len, 512);
    uint64_t sp0 = saved.rsp, path = sp0 - 0x100, ret = sp0 - 0x108;
    LT_EQ(PT(LX_PTRACE_POKEDATA, child, path, 0x65732f636f72702full), 0);         /* "/proc/se" */
    LT_EQ(PT(LX_PTRACE_POKEDATA, child, path + 8, 0x34332f64662f666cull), 0);      /* "lf/fd/34" */
    LT_EQ(PT(LX_PTRACE_POKEDATA, child, path + 16, 0), 0);
    LT_EQ(PT(LX_PTRACE_POKEDATA, child, ret, 9), 0);
    struct lx_user_regs inj = saved;
    inj.rdx = 0; inj.rsi = 1; inj.rdi = path; inj.rip = g_code + 0x400; inj.rsp = ret;
    iov = (struct lx_iovec){ P(&inj), sizeof inj };
    LT_EQ(PT(LX_PTRACE_SETREGSET, child, LX_NT_PRSTATUS, P(&iov)), 0);
    LT_EQ(PT(LX_PTRACE_CONT, child, 0, 0), 0);
    /* the injected call returns to address 9: SIGSEGV 0xb7f, si_code SEGV_MAPERR, si_addr 9 */
    LT_CHECK(expect_stop(child, 0xb7f));
    LT_EQ(PT(LX_PTRACE_GETSIGINFO, child, 0, P(&si)), 0);
    LT_EQ(si.si_signo, 11);
    LT_EQ(si.si_code, LX_SEGV_MAPERR);
    LT_EQ(si.u.fault.addr, 9);
    get_regs216(child, &r, &n);
    LT_EQ(r.rip, 9);
    LT_EQ(r.rax, 0x1234);                                          /* the result of the injected call */
    LT_EQ(r.orig_rax, (uint64_t)-1);
    iov = (struct lx_iovec){ P(fp2), 512 };
    LT_EQ(PT(LX_PTRACE_GETREGSET, child, LX_NT_PRFPREG, P(&iov)), 0);
    uint32_t mx; memcpy(&mx, fp2 + 24, 4);
    LT_EQ(mx, 0x1fa0);                                             /* the callee changed MXCSR */
    /* restore everything and drop the signal with CONT(0) */
    iov = (struct lx_iovec){ P(&saved), sizeof saved };
    LT_EQ(PT(LX_PTRACE_SETREGSET, child, LX_NT_PRSTATUS, P(&iov)), 0);
    iov = (struct lx_iovec){ P(fp), 512 };
    LT_EQ(PT(LX_PTRACE_SETREGSET, child, LX_NT_PRFPREG, P(&iov)), 0);
    LT_EQ(PT(LX_PTRACE_CONT, child, 0, 0), 0);
    /* second fault: this time the tracer passes the signal on; the default action kills the tracee */
    LT_CHECK(expect_stop(child, 0xb7f));
    LT_EQ(PT(LX_PTRACE_GETSIGINFO, child, 0, P(&si)), 0);
    LT_EQ(si.u.fault.addr, 9);
    LT_EQ(PT(LX_PTRACE_CONT, child, 0, 11), 0);
    st = 0;
    LT_EQ(wait_pid(child, &st, 0), child);
    LT_EQ(st, 11);                                                 /* WIFSIGNALED, SIGSEGV, no core */
    LT_ERRNO(wait_pid(child, &st, LX_WNOHANG), LX_ECHILD);
    LT_ERRNO(PT(LX_PTRACE_GETREGS, child, 0, P(&r)), LX_ESRCH);

    /* what the tracee saw */
    LT_EQ(get64(rp[0]), 0);                                        /* [0] TRACEME */
    LT_EQ(get64(rp[0]), child);                                    /* [1] getpid */
    LT_EQ(get64(rp[0]), child);                                    /* [2] gettid */
    LT_EQ(get64(rp[0]), 0xcc);                                     /* [3] the POKEd byte is what it executes */
    LT_EQ(get64(rp[0]), 1);                                        /* [4] invalidate_code ran before it resumed */
    LT_EQ(get64(rp[0]), (int64_t)(g_code + 0x400));                /* [5] new RIP */
    LT_EQ(get64(rp[0]), (int64_t)path);                            /* [6] RDI */
    LT_EQ(get64(rp[0]), 0x65732f636f72702f);                       /* [7] the string it would read */
    LT_EQ(get64(rp[0]), 9);                                        /* [8] return address */
    LT_EQ(get64(rp[0]), (int64_t)(g_code + ENTRY_OFF));            /* [9] restored RIP */
    LT_EQ(get64(rp[0]), 0);                                        /* [10] restored RAX */
    LT_EQ(get64(rp[0]), 0x1f80);                                   /* [11] restored MXCSR */
    LT_EQ(get64(rp[0]), 0x1111);                                   /* [12] restored RBX */
    LT_EQ(get64(rp[0]), 55);                                       /* died in lkd_signal_next */
    close(rp[0]);
}

int main(void)
{
    test_setup(1);
    LT_RUN(eac_sequence);
    return lt_finish();
}
