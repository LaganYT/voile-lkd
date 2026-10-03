/* test_ptrace_requests.c: the ptrace(2) requests against a tracee run by a thread of this process.
 * Linux behaviour modelled (v6.12): kernel/ptrace.c SYSCALL_DEFINE4(ptrace), ptrace_attach,
 * ptrace_check_attach, ptrace_request, ptrace_resume, ptrace_regset, ptrace_get_syscall_info;
 * arch/x86/kernel/ptrace.c arch_ptrace, getreg/putreg, genregs_set, ptrace_set_debugreg;
 * arch/x86/kernel/fpu/regset.c xfpregs_set/xstateregs_set. Expected values are written as literals. */
#include "test_ptrace_util.h"

static struct tracee *attach_loop(void)
{
    struct tracee *e = tracee_start(body_loop, NULL);
    for (int i = 0; i < 4000 && e->calls < 3; i++) usleep(500);
    LT_EQ(PT(LX_PTRACE_ATTACH, e->tid, 0, 0), 0);
    LT_CHECK(expect_stop(e->tid, 0x137f));
    return e;
}

/* leave the tracee running untraced, make it exit, collect it */
static void finish(struct tracee *e)
{
    (void)PT(LX_PTRACE_DETACH, e->tid, 0, 0);
    tracee_quit(e);
    get64(e->wr[0]);
    pthread_join(e->th, NULL);
    int st;
    (void)wait_pid(e->tid, &st, LX_WNOHANG);
    close(e->rd[0]); close(e->rd[1]); close(e->wr[0]); close(e->wr[1]);
    free(e);
}

/* ---- ATTACH, SEIZE and the access rules -------------------------------------------------------- */

/* ptrace_attach(): ESRCH for no task, EPERM for oneself (same_thread_group) and for a task that is
 * already traced; the uid rule and dumpable of __ptrace_may_access(); CAP_SYS_PTRACE lifts both. */
LT_TEST(attach_errors)
{
    LT_ERRNO(PT(LX_PTRACE_ATTACH, 0x3ffffff0, 0, 0), LX_ESRCH);
    LT_ERRNO(PT(LX_PTRACE_ATTACH, 0, 0, 0), LX_ESRCH);
    LT_ERRNO(PT(LX_PTRACE_ATTACH, TR->tid, 0, 0), LX_EPERM);
    struct tracee *e = tracee_start(body_loop, NULL);
    for (int i = 0; i < 4000 && e->calls < 3; i++) usleep(500);
    /* a tracee of another uid */
    uint32_t keep = e->t->proc->cred.uid, keep_e = e->t->proc->cred.euid;
    e->t->proc->cred.uid = e->t->proc->cred.euid = 2000;
    lkd_registry_publish(e->t);
    LT_ERRNO(PT(LX_PTRACE_ATTACH, e->tid, 0, 0), LX_EPERM);
    uint64_t cap = TR->proc->cred.cap_eff;
    TR->proc->cred.cap_eff |= 1ull << 19; /* CAP_SYS_PTRACE */
    e->t->proc->cred.uid = e->t->proc->cred.euid = keep;
    (void)keep_e;
    /* not dumpable: refused without the capability */
    e->t->proc->dumpable = 0;
    lkd_registry_publish(e->t);
    LT_EQ(lkd_ptrace_check_access(TR, e->pid, LKD_PTRACE_MODE_ATTACH), 0);
    TR->proc->cred.cap_eff = cap;
    LT_ERRNO(lkd_ptrace_check_access(TR, e->pid, LKD_PTRACE_MODE_ATTACH), LX_EPERM);
    LT_ERRNO(PT(LX_PTRACE_ATTACH, e->tid, 0, 0), LX_EPERM);
    e->t->proc->dumpable = 1;
    lkd_registry_publish(e->t);
    LT_EQ(lkd_ptrace_check_access(TR, e->pid, LKD_PTRACE_MODE_READ), 0);
    LT_EQ(lkd_ptrace_check_access(TR, TR->proc->pid, LKD_PTRACE_MODE_ATTACH), 0); /* own process */
    LT_ERRNO(lkd_ptrace_check_access(TR, 0x3ffffff0, LKD_PTRACE_MODE_READ), LX_ESRCH);
    LT_EQ(PT(LX_PTRACE_ATTACH, e->tid, 0, 0), 0);
    LT_ERRNO(PT(LX_PTRACE_ATTACH, e->tid, 0, 0), LX_EPERM);      /* task->ptrace is set */
    LT_ERRNO(PT(LX_PTRACE_SEIZE, e->tid, 0, 0), LX_EPERM);
    LT_CHECK(expect_stop(e->tid, 0x137f));
    LT_EQ(lkd_ptrace_tracer_of(e->tid), TR->tid);
    finish(e);
    LT_EQ(lkd_ptrace_tracer_of(e->tid), 0);
}

/* ptrace_attach() for SEIZE: addr must be 0 and the flags a subset of PTRACE_O_MASK (EIO), then
 * check_ptrace_options() (SUSPEND_SECCOMP needs CONFIG_SECCOMP, absent here: EINVAL). No SIGSTOP is sent. */
LT_TEST(seize_arguments)
{
    struct tracee *e = tracee_start(body_loop, NULL);
    for (int i = 0; i < 4000 && e->calls < 3; i++) usleep(500);
    LT_ERRNO(PT(LX_PTRACE_SEIZE, e->tid, 1, 0), LX_EIO);
    LT_ERRNO(PT(LX_PTRACE_SEIZE, e->tid, 0, 0x400000), LX_EIO);
    LT_ERRNO(PT(LX_PTRACE_SEIZE, e->tid, 0, LX_PTRACE_O_SUSPEND_SECCOMP), LX_EINVAL);
    LT_EQ(PT(LX_PTRACE_SEIZE, e->tid, 0, LX_PTRACE_O_TRACESYSGOOD), 0);
    usleep(30000);
    int st = 0;
    LT_EQ(wait_pid(e->tid, &st, LX_WNOHANG), 0);            /* seize does not stop the tracee */
    LT_CHECK(e->calls > 3);
    finish(e);
}

/* ptrace_check_attach(): ESRCH unless the caller thread is the tracer and the tracee is stopped. */
LT_TEST(requests_need_a_stopped_tracee_of_the_caller)
{
    struct tracee *e = tracee_start(body_loop, NULL);
    for (int i = 0; i < 4000 && e->calls < 3; i++) usleep(500);
    struct lx_user_regs r;
    LT_ERRNO(PT(LX_PTRACE_GETREGS, e->tid, 0, P(&r)), LX_ESRCH);           /* not traced */
    LT_ERRNO(PT(LX_PTRACE_CONT, e->tid, 0, 0), LX_ESRCH);
    LT_ERRNO(PT(LX_PTRACE_PEEKDATA, e->tid, 0x1000, P(&r)), LX_ESRCH);
    LT_ERRNO(PT(LX_PTRACE_KILL, e->tid, 0, 0), LX_ESRCH);                  /* KILL ignores the state, not the link */
    LT_ERRNO(PT(LX_PTRACE_SETOPTIONS, e->tid, 0, 0), LX_ESRCH);
    LT_EQ(PT(LX_PTRACE_SEIZE, e->tid, 0, 0), 0);
    LT_ERRNO(PT(LX_PTRACE_GETREGS, e->tid, 0, P(&r)), LX_ESRCH);           /* traced but running */
    LT_ERRNO(PT(LX_PTRACE_CONT, e->tid, 0, 0), LX_ESRCH);
    LT_ERRNO(PT(LX_PTRACE_GETSIGINFO, e->tid, 0, P(&r)), LX_ESRCH);
    LT_ERRNO(PT(LX_PTRACE_DETACH, e->tid, 0, 0), LX_ESRCH);
    LT_EQ(PT(LX_PTRACE_INTERRUPT, e->tid, 0, 0), 0);                       /* seized: allowed while running */
    LT_CHECK(expect_stop(e->tid, 0x80057f));                               /* PTRACE_EVENT_STOP, SIGTRAP */
    /* another thread of the tracer's process is not the tracer: the real parent is a thread */
    lkd_task *other = lkd_task_alloc(NULL, 0);
    lkd_task_publish(other);
    LT_ERRNO(sct(other, LKD_NR_ptrace, LX_PTRACE_GETREGS, (uint64_t)e->tid, 0, P(&r), 0, 0), LX_ESRCH);
    LT_ERRNO(sct(other, LKD_NR_ptrace, LX_PTRACE_CONT, (uint64_t)e->tid, 0, 0, 0, 0), LX_ESRCH);
    LT_EQ(PT(LX_PTRACE_GETREGS, e->tid, 0, P(&r)), 0);
    finish(e);
}

/* ---- register sets -------------------------------------------------------------------------------- */

/* ptrace_regset(): iov_len must be a multiple of the element size (EINVAL), is clamped to the set, the
 * clamped length is written back; unknown notes are EINVAL; the I/O bitmap is ENXIO without ioperm. */
LT_TEST(getregset_formats)
{
    struct tracee *e = attach_loop();
    struct fake_cpu *c = fake_cpu_of(e->t);
    uint8_t buf[4096];
    struct lx_iovec iov;
    iov = (struct lx_iovec){ P(buf), 4096 };
    memset(buf, 0x5a, sizeof buf);
    LT_EQ(PT(LX_PTRACE_GETREGSET, e->tid, LX_NT_PRSTATUS, P(&iov)), 0);
    LT_EQ(iov.iov_len, 216);                                     /* clamped and written back */
    LT_EQ(buf[216], 0x5a);                                       /* nothing past the set */
    struct lx_user_regs *r = (struct lx_user_regs *)buf;
    LT_EQ(r->rip, c->regs.rip);
    LT_EQ(r->rsp, 0x7ffffff0000ull);
    LT_EQ(r->cs, 0x33);
    LT_EQ(r->ss, 0x2b);
    LT_EQ(r->eflags, 0x246);
    LT_EQ(r->fs_base, 0x7f0000001000ull);
    LT_EQ(r->rbx, 0x1111);
    LT_EQ(r->orig_rax, (uint64_t)-1);                            /* signal stop from an interrupt */
    iov = (struct lx_iovec){ P(buf), 215 };
    LT_ERRNO(PT(LX_PTRACE_GETREGSET, e->tid, LX_NT_PRSTATUS, P(&iov)), LX_EINVAL);
    iov.iov_len = 7;
    LT_ERRNO(PT(LX_PTRACE_GETREGSET, e->tid, LX_NT_PRSTATUS, P(&iov)), LX_EINVAL);
    iov.iov_len = 1;
    LT_ERRNO(PT(LX_PTRACE_GETREGSET, e->tid, LX_NT_PRSTATUS, P(&iov)), LX_EINVAL);
    memset(buf, 0x5a, sizeof buf);
    iov.iov_len = 16;
    LT_EQ(PT(LX_PTRACE_GETREGSET, e->tid, LX_NT_PRSTATUS, P(&iov)), 0);
    LT_EQ(iov.iov_len, 16);
    LT_EQ(buf[16], 0x5a);                                        /* only r15 and r14 */
    iov.iov_len = 0;
    LT_EQ(PT(LX_PTRACE_GETREGSET, e->tid, LX_NT_PRSTATUS, P(&iov)), 0);
    LT_EQ(iov.iov_len, 0);
    iov = (struct lx_iovec){ 0, 216 };
    LT_ERRNO(PT(LX_PTRACE_GETREGSET, e->tid, LX_NT_PRSTATUS, P(&iov)), LX_EFAULT);
    LT_ERRNO(PT(LX_PTRACE_GETREGSET, e->tid, LX_NT_PRSTATUS, 0), LX_EFAULT);
    /* NT_PRFPREG: the 512-byte FXSAVE image */
    iov = (struct lx_iovec){ P(buf), 4096 };
    LT_EQ(PT(LX_PTRACE_GETREGSET, e->tid, LX_NT_PRFPREG, P(&iov)), 0);
    LT_EQ(iov.iov_len, 512);
    uint16_t cwd; uint32_t mxcsr;
    memcpy(&cwd, buf, 2); memcpy(&mxcsr, buf + 24, 4);
    LT_EQ(cwd, 0x37f);
    LT_EQ(mxcsr, 0x1f80);
    /* NT_X86_XSTATE: the XSAVE image of the engine (832 bytes), software-reserved bytes filled in */
    iov = (struct lx_iovec){ P(buf), 4096 };
    LT_EQ(PT(LX_PTRACE_GETREGSET, e->tid, LX_NT_X86_XSTATE, P(&iov)), 0);
    LT_EQ(iov.iov_len, 832);
    uint32_t magic1; memcpy(&magic1, buf + 464, 4);
    LT_EQ(magic1, 0x46505853);                                   /* FP_XSTATE_MAGIC1 */
    /* notes the x86-64 view does not have, and the I/O bitmap of a task without ioperm() */
    iov = (struct lx_iovec){ P(buf), 8 };
    LT_ERRNO(PT(LX_PTRACE_GETREGSET, e->tid, LX_NT_PRXFPREG, P(&iov)), LX_EINVAL);
    LT_ERRNO(PT(LX_PTRACE_GETREGSET, e->tid, LX_NT_PRPSINFO, P(&iov)), LX_EINVAL);
    LT_ERRNO(PT(LX_PTRACE_GETREGSET, e->tid, 0x12345, P(&iov)), LX_EINVAL);
    LT_ERRNO(PT(LX_PTRACE_GETREGSET, e->tid, LX_NT_X86_IO_BITMAP, P(&iov)), LX_ENXIO);
    LT_ERRNO(PT(LX_PTRACE_SETREGSET, e->tid, LX_NT_X86_IO_BITMAP, P(&iov)), LX_EOPNOTSUPP);
    finish(e);
}

/* genregs_set(): a read-modify-write of the whole set is accepted (the EAC pattern); each word goes
 * through putreg(): eflags keeps the bits outside FLAG_MASK, selectors must have RPL 3 or be null, cs and
 * ss cannot be zero, the FS/GS bases must be below TASK_SIZE_MAX. The new values reach the engine when the
 * tracee resumes, not before. */
LT_TEST(setregset_and_putreg_rules)
{
    struct tracee *e = attach_loop();
    struct fake_cpu *c = fake_cpu_of(e->t);
    struct lx_user_regs r, back;
    struct lx_iovec iov = { P(&r), sizeof r };
    LT_EQ(PT(LX_PTRACE_GETREGSET, e->tid, LX_NT_PRSTATUS, P(&iov)), 0);
    uint64_t rip0 = r.rip;
    iov.iov_len = sizeof r;
    LT_EQ(PT(LX_PTRACE_SETREGSET, e->tid, LX_NT_PRSTATUS, P(&iov)), 0);      /* unchanged: accepted */
    LT_EQ(iov.iov_len, 216);
    r.rdi = 0xabcdef; r.rsi = 7; r.rdx = 9; r.rsp = 0x7ffffff0100ull; r.r15 = 0x1515;
    r.rip = 0x600000;
    iov.iov_len = sizeof r;
    LT_EQ(PT(LX_PTRACE_SETREGSET, e->tid, LX_NT_PRSTATUS, P(&iov)), 0);
    LT_EQ(c->regs.rip, rip0);                                      /* the tracee is still parked */
    iov = (struct lx_iovec){ P(&back), sizeof back };
    LT_EQ(PT(LX_PTRACE_GETREGSET, e->tid, LX_NT_PRSTATUS, P(&iov)), 0);
    LT_EQ(back.rip, 0x600000);                                     /* the view follows the write */
    LT_EQ(back.rdi, 0xabcdef);
    LT_EQ(back.r15, 0x1515);
    /* eflags: only FLAG_MASK is writable; 0x246 had IF (0x200) which stays, TF (0x100) is writable */
    r = back; r.eflags = 0x100 | 0x1;
    iov = (struct lx_iovec){ P(&r), sizeof r };
    LT_EQ(PT(LX_PTRACE_SETREGSET, e->tid, LX_NT_PRSTATUS, P(&iov)), 0);
    iov = (struct lx_iovec){ P(&back), sizeof back };
    PT(LX_PTRACE_GETREGSET, e->tid, LX_NT_PRSTATUS, P(&iov));
    LT_EQ(back.eflags, 0x200 | 0x2 | 0x100 | 0x1);                 /* 0x2 and 0x200 kept from the old value */
    /* word-by-word errors: a bad selector fails, the words before it are applied */
    r = back; r.r14 = 0x1414; r.fs = 1;                            /* RPL 1 */
    iov = (struct lx_iovec){ P(&r), sizeof r };
    LT_ERRNO(PT(LX_PTRACE_SETREGSET, e->tid, LX_NT_PRSTATUS, P(&iov)), LX_EIO);
    iov = (struct lx_iovec){ P(&back), sizeof back };
    PT(LX_PTRACE_GETREGSET, e->tid, LX_NT_PRSTATUS, P(&iov));
    LT_EQ(back.r14, 0x1414);
    LT_EQ(back.fs, 0);
    r = back; r.cs = 0;
    iov = (struct lx_iovec){ P(&r), sizeof r };
    LT_ERRNO(PT(LX_PTRACE_SETREGSET, e->tid, LX_NT_PRSTATUS, P(&iov)), LX_EIO);
    r = back; r.fs_base = 0x00007ffffffff000ull;
    LT_ERRNO(PT(LX_PTRACE_SETREGSET, e->tid, LX_NT_PRSTATUS, P(&iov)), LX_EIO);
    r = back; r.gs_base = 0x7f0000002000ull; r.ds = 0x2b;
    LT_EQ(PT(LX_PTRACE_SETREGSET, e->tid, LX_NT_PRSTATUS, P(&iov)), 0);
    /* a partial set writes only the leading words */
    uint64_t two[2] = { 0x77, 0x88 };
    iov = (struct lx_iovec){ P(two), 16 };
    LT_EQ(PT(LX_PTRACE_SETREGSET, e->tid, LX_NT_PRSTATUS, P(&iov)), 0);
    LT_EQ(iov.iov_len, 16);
    iov = (struct lx_iovec){ P(&back), sizeof back };
    PT(LX_PTRACE_GETREGSET, e->tid, LX_NT_PRSTATUS, P(&iov));
    LT_EQ(back.r15, 0x77);
    LT_EQ(back.r14, 0x88);
    LT_EQ(back.r13, 0);
    iov = (struct lx_iovec){ P(two), 12 };
    LT_ERRNO(PT(LX_PTRACE_SETREGSET, e->tid, LX_NT_PRSTATUS, P(&iov)), LX_EINVAL);
    /* resume: the registers reach the engine; rip 0x600000 was set earlier and survives the later sets */
    LT_EQ(PT(LX_PTRACE_SETREGS, e->tid, 0, P(&back)), 0);
    back.rip = 0x600010;
    LT_EQ(PT(LX_PTRACE_SETREGS, e->tid, 0, P(&back)), 0);
    LT_EQ(PT(LX_PTRACE_CONT, e->tid, 0, 0), 0);
    for (int i = 0; i < 4000 && c->regs.rip == rip0; i++) usleep(500);
    LT_CHECK(c->regs.rip >= 0x600010 && c->regs.rip < 0x600100);   /* the tracee kept running from there */
    LT_EQ(c->regs.r15, 0x77);
    LT_EQ(c->regs.gs_base, 0x7f0000002000ull);
    finish(e);
}

/* PTRACE_GETREGS/SETREGS and PEEKUSER/POKEUSER of the same words (arch_ptrace): offsets are
 * word-aligned and below sizeof(struct user) = 928, debug registers sit at 848, the rest reads as zero. */
LT_TEST(user_area)
{
    struct tracee *e = attach_loop();
    struct fake_cpu *c = fake_cpu_of(e->t);
    struct lx_user_regs r;
    LT_EQ(PT(LX_PTRACE_GETREGS, e->tid, 0, P(&r)), 0);
    LT_EQ(r.rip, c->regs.rip);
    uint64_t v = 0;
    LT_EQ(PT(LX_PTRACE_PEEKUSER, e->tid, offsetof(struct lx_user_regs, rip), P(&v)), 0);
    LT_EQ(v, r.rip);
    LT_EQ(PT(LX_PTRACE_PEEKUSER, e->tid, offsetof(struct lx_user_regs, cs), P(&v)), 0);
    LT_EQ(v, 0x33);
    LT_ERRNO(PT(LX_PTRACE_PEEKUSER, e->tid, 3, P(&v)), LX_EIO);            /* misaligned */
    LT_ERRNO(PT(LX_PTRACE_PEEKUSER, e->tid, 928, P(&v)), LX_EIO);          /* past struct user */
    v = 1234;
    LT_EQ(PT(LX_PTRACE_PEEKUSER, e->tid, 304, P(&v)), 0);                  /* i387 area: not provided, zero */
    LT_EQ(v, 0);
    LT_ERRNO(PT(LX_PTRACE_PEEKUSER, e->tid, 0, 0), LX_EFAULT);
    LT_EQ(PT(LX_PTRACE_POKEUSER, e->tid, offsetof(struct lx_user_regs, rbx), 0x4242), 0);
    LT_EQ(PT(LX_PTRACE_PEEKUSER, e->tid, offsetof(struct lx_user_regs, rbx), P(&v)), 0);
    LT_EQ(v, 0x4242);
    LT_ERRNO(PT(LX_PTRACE_POKEUSER, e->tid, offsetof(struct lx_user_regs, ss), 0), LX_EIO);
    LT_ERRNO(PT(LX_PTRACE_POKEUSER, e->tid, 5, 0), LX_EIO);
    LT_ERRNO(PT(LX_PTRACE_POKEUSER, e->tid, 4096, 0), LX_EIO);
    /* debug registers: addresses can be stored; enabling a breakpoint (DR7) is refused, never faked */
    LT_EQ(PT(LX_PTRACE_POKEUSER, e->tid, 848 + 0 * 8, 0x401234), 0);
    LT_EQ(PT(LX_PTRACE_PEEKUSER, e->tid, 848 + 0 * 8, P(&v)), 0);
    LT_EQ(v, 0x401234);
    LT_ERRNO(PT(LX_PTRACE_POKEUSER, e->tid, 848 + 1 * 8, 0x00007ffffffff000ull), LX_EINVAL);
    LT_ERRNO(PT(LX_PTRACE_POKEUSER, e->tid, 848 + 7 * 8, 0x1), LX_EIO);
    LT_ERRNO(PT(LX_PTRACE_POKEUSER, e->tid, 848 + 4 * 8, 0x1), LX_EIO);    /* DR4 does not exist */
    LT_EQ(PT(LX_PTRACE_POKEUSER, e->tid, 848 + 7 * 8, 0), 0);
    LT_EQ(PT(LX_PTRACE_PEEKUSER, e->tid, 848 + 6 * 8, P(&v)), 0);
    LT_EQ(v, 0xffff0ff0);                                                  /* DR6 reads with its reserved bits */
    finish(e);
}

/* PTRACE_GETFPREGS/SETFPREGS and the FP regsets: the whole 512-byte image or EINVAL, MXCSR reserved bits are
 * masked off silently; NT_X86_XSTATE needs the full image (EFAULT) and a valid header (EINVAL). */
LT_TEST(fp_state)
{
    struct tracee *e = attach_loop();
    struct fake_cpu *c = fake_cpu_of(e->t);
    struct lx_user_fpregs fp;
    LT_EQ(PT(LX_PTRACE_GETFPREGS, e->tid, 0, P(&fp)), 0);
    LT_EQ(fp.mxcsr, 0x1f80);
    LT_EQ(fp.cwd, 0x37f);
    fp.mxcsr = 0xffffffff;
    fp.xmm_space[4] = 0x11223344;
    LT_EQ(PT(LX_PTRACE_SETFPREGS, e->tid, 0, P(&fp)), 0);
    struct lx_user_fpregs g;
    LT_EQ(PT(LX_PTRACE_GETFPREGS, e->tid, 0, P(&g)), 0);
    LT_EQ(g.mxcsr, 0xffff);                                       /* & MXCSR_MASK (0xffff in the image) */
    LT_EQ(g.xmm_space[4], 0x11223344);
    uint8_t x[832];
    struct lx_iovec iov = { P(x), 832 };
    LT_EQ(PT(LX_PTRACE_GETREGSET, e->tid, LX_NT_X86_XSTATE, P(&iov)), 0);
    LT_EQ(iov.iov_len, 832);
    uint32_t xm; memcpy(&xm, x + 24, 4);
    LT_EQ(xm, 0xffff);                                            /* the XSAVE view follows SETFPREGS */
    uint8_t bad[832];
    memcpy(bad, x, 832);
    uint64_t xcomp = 1;
    memcpy(bad + 520, &xcomp, 8);                                 /* compacted format is not allowed */
    iov = (struct lx_iovec){ P(bad), 832 };
    LT_ERRNO(PT(LX_PTRACE_SETREGSET, e->tid, LX_NT_X86_XSTATE, P(&iov)), LX_EINVAL);
    memcpy(bad, x, 832);
    bad[530] = 1;                                                 /* reserved header bytes */
    LT_ERRNO(PT(LX_PTRACE_SETREGSET, e->tid, LX_NT_X86_XSTATE, P(&iov)), LX_EINVAL);
    iov = (struct lx_iovec){ P(x), 800 };
    LT_ERRNO(PT(LX_PTRACE_SETREGSET, e->tid, LX_NT_X86_XSTATE, P(&iov)), LX_EFAULT);
    iov.iov_len = 832;
    x[600] = 0x6b;                                                /* a YMM byte */
    LT_EQ(PT(LX_PTRACE_SETREGSET, e->tid, LX_NT_X86_XSTATE, P(&iov)), 0);
    iov = (struct lx_iovec){ P(&fp), 100 };
    LT_ERRNO(PT(LX_PTRACE_SETREGSET, e->tid, LX_NT_PRFPREG, P(&iov)), LX_EINVAL);   /* xfpregs_set: count != 512 */
    LT_EQ(PT(LX_PTRACE_CONT, e->tid, 0, 0), 0);
    for (int i = 0; i < 4000 && c->xsave[600] != 0x6b; i++) usleep(500);
    LT_EQ(c->xsave[600], 0x6b);                                   /* applied by the tracee at resume */
    uint32_t cm; memcpy(&cm, c->xsave + 24, 4);
    LT_EQ(cm, 0xffff);
    finish(e);
}

/* ---- syscall stops ---------------------------------------------------------------------------------- */

/* ptrace_report_syscall_entry/exit: wait status SIGTRAP (0x57f), SIGTRAP|0x80 with TRACESYSGOOD (0x857f);
 * at entry rax = -ENOSYS, orig_rax = number, rcx = rip after SYSCALL, r11 = rflags; at exit rax is the
 * result; GETEVENTMSG is 1 and 2; siginfo is {SIGTRAP, si_code = exit_code, pid = tid}. */
/* rt_sigreturn under PTRACE_SYSCALL: syscall_exit_work() reports its exit too (request ptrace-1: lkd_syscall
 * skipped the exit stop for LKD_RET_CONTEXT_REPLACED). The view is the restored context: orig_rax 15, rax
 * as restored from the frame (the fake engine's 0x777), rcx not patched to the return rip. */
static void body_sigreturn(struct tracee *e)
{
    char c;
    int died = 0;
    for (int i = 0; i < 40000 && !died; i++) {
        (void)gsys(e->t, LKD_NR_getpid, 0, 0, 0, 0, 0, 0, &died);
        e->calls++;
        if (read(e->rd[0], &c, 1) == 1) {
            if (c != 'R') break;
            struct fake_cpu *cp = fake_cpu_of(e->t);
            cp->regs.rip += 2;
            cp->regs.rax = 0x777;       /* what frame_restore put back (the fake restores no register) */
            cp->regs.orig_rax = 15;
            cp->regs.rcx = 0x1234;
            lkd_sysreq rq = { LKD_NR_rt_sigreturn, { 0 } };
            int64_t rax = 0;
            (void)lkd_syscall(e->t, &rq, &rax);
            if (safe_point(e->t) < 0) died = 1;
            put64(e->wr[1], 5);
        }
        usleep(300);
    }
    put64(e->wr[1], 1);
    tracee_exit(e, 3);
}

LT_TEST(sigreturn_has_an_exit_stop)
{
    struct tracee *e = tracee_start(body_sigreturn, NULL);
    for (int i = 0; i < 4000 && e->calls < 3; i++) usleep(500);
    LT_EQ(PT(LX_PTRACE_SEIZE, e->tid, 0, 0), 0);
    LT_EQ(PT(LX_PTRACE_INTERRUPT, e->tid, 0, 0), 0);
    LT_CHECK(expect_stop(e->tid, 0x80057f));
    char x = 'R';
    (void)!write(e->rd[1], &x, 1);
    int ok = 1;
    struct lx_user_regs r;
    /* syscall stops until the entry of rt_sigreturn */
    for (int i = 0; i < 20 && ok; i++) {
        ok = PT(LX_PTRACE_SYSCALL, e->tid, 0, 0) == 0 && expect_stop(e->tid, 0x57f) && PT(LX_PTRACE_GETREGS, e->tid, 0, P(&r)) == 0;
        if (ok && r.orig_rax == 15 && r.rax == (uint64_t)-38) break;
    }
    LT_CHECK(ok);
    LT_EQ(r.orig_rax, 15);
    LT_EQ(PT(LX_PTRACE_SYSCALL, e->tid, 0, 0), 0);
    LT_CHECK(expect_stop(e->tid, 0x57f));
    uint64_t msg = 0;
    LT_EQ(PT(LX_PTRACE_GETEVENTMSG, e->tid, 0, P(&msg)), 0);
    LT_EQ(msg, 2);                                                          /* PTRACE_EVENTMSG_SYSCALL_EXIT */
    LT_EQ(PT(LX_PTRACE_GETREGS, e->tid, 0, P(&r)), 0);
    LT_EQ(r.orig_rax, 15);
    LT_EQ(r.rax, 0x777);
    LT_EQ(r.rcx, 0x1234);
    LT_EQ(PT(LX_PTRACE_CONT, e->tid, 0, 0), 0);
    LT_EQ(get64(e->wr[0]), 5);
    finish(e);
}

LT_TEST(syscall_stops)
{
    struct tracee *e = attach_loop();
    struct lx_user_regs r;
    uint64_t msg = 0;
    LT_EQ(PT(LX_PTRACE_SYSCALL, e->tid, 0, 0), 0);
    LT_CHECK(expect_stop(e->tid, 0x57f));
    LT_EQ(PT(LX_PTRACE_GETREGS, e->tid, 0, P(&r)), 0);
    LT_EQ(r.orig_rax, 39);
    LT_EQ(r.rax, (uint64_t)-38);
    LT_EQ(r.rcx, r.rip);
    LT_EQ(r.r11, r.eflags);
    LT_EQ(r.r11, 0x246);
    LT_EQ(PT(LX_PTRACE_GETEVENTMSG, e->tid, 0, P(&msg)), 0);
    LT_EQ(msg, 1);
    struct lx_siginfo si;
    LT_EQ(PT(LX_PTRACE_GETSIGINFO, e->tid, 0, P(&si)), 0);
    LT_EQ(si.si_signo, 5);
    LT_EQ(si.si_code, 5);
    LT_EQ(si.u.kill.pid, e->tid);
    LT_EQ(PT(LX_PTRACE_SYSCALL, e->tid, 0, 0), 0);
    LT_CHECK(expect_stop(e->tid, 0x57f));
    LT_EQ(PT(LX_PTRACE_GETREGS, e->tid, 0, P(&r)), 0);
    LT_EQ(r.orig_rax, 39);
    LT_EQ(r.rax, e->pid);                                         /* the result of getpid() */
    LT_EQ(r.rcx, r.rip);
    LT_EQ(PT(LX_PTRACE_GETEVENTMSG, e->tid, 0, P(&msg)), 0);
    LT_EQ(msg, 2);
    /* without TRACESYSGOOD GET_SYSCALL_INFO sees no syscall stop (it keys on si_code SIGTRAP|0x80) */
    struct lx_ptrace_syscall_info info;
    memset(&info, 0xff, sizeof info);
    LT_EQ(PT(LX_PTRACE_GET_SYSCALL_INFO, e->tid, sizeof info, P(&info)), 24);
    LT_EQ(info.op, LX_PTRACE_SYSCALL_INFO_NONE);
    LT_EQ(info.arch, 0xc000003e);
    /* TRACESYSGOOD */
    LT_ERRNO(PT(LX_PTRACE_SETOPTIONS, e->tid, 0, 0x400000), LX_EINVAL);
    LT_ERRNO(PT(LX_PTRACE_SETOPTIONS, e->tid, 0, LX_PTRACE_O_SUSPEND_SECCOMP), LX_EINVAL);
    LT_EQ(PT(LX_PTRACE_SETOPTIONS, e->tid, 0, LX_PTRACE_O_TRACESYSGOOD), 0);
    LT_EQ(PT(LX_PTRACE_SYSCALL, e->tid, 0, 0), 0);
    LT_CHECK(expect_stop(e->tid, 0x857f));
    LT_EQ(PT(LX_PTRACE_GETSIGINFO, e->tid, 0, P(&si)), 0);
    LT_EQ(si.si_code, 0x85);
    memset(&info, 0, sizeof info);
    LT_EQ(PT(LX_PTRACE_GET_SYSCALL_INFO, e->tid, sizeof info, P(&info)), 80);   /* offsetofend(entry.args) */
    LT_EQ(info.op, LX_PTRACE_SYSCALL_INFO_ENTRY);
    LT_EQ(info.u.entry.nr, 39);
    LT_EQ(info.instruction_pointer, fake_cpu_of(e->t)->regs.rip);
    LT_EQ(info.stack_pointer, 0x7ffffff0000ull);
    LT_EQ(PT(LX_PTRACE_GET_SYSCALL_INFO, e->tid, 30, P(&info)), 80);            /* returns the full size */
    LT_EQ(PT(LX_PTRACE_SYSCALL, e->tid, 0, 0), 0);
    LT_CHECK(expect_stop(e->tid, 0x857f));
    memset(&info, 0, sizeof info);
    LT_EQ(PT(LX_PTRACE_GET_SYSCALL_INFO, e->tid, sizeof info, P(&info)), 33);   /* offsetofend(exit.is_error) */
    LT_EQ(info.op, LX_PTRACE_SYSCALL_INFO_EXIT);
    LT_EQ(info.u.exit.rval, e->pid);
    LT_EQ(info.u.exit.is_error, 0);
    LT_EQ(PT(LX_PTRACE_GETEVENTMSG, e->tid, 0, P(&msg)), 0);
    LT_EQ(msg, 2);
    /* CONT ends the syscall tracing: the tracee runs freely again */
    LT_EQ(PT(LX_PTRACE_CONT, e->tid, 0, 0), 0);
    uint64_t n0 = e->calls;
    usleep(20000);
    LT_CHECK(e->calls > n0 + 3);
    int st;
    LT_EQ(wait_pid(e->tid, &st, LX_WNOHANG), 0);
    finish(e);
}

/* syscall_trace_enter(): the tracer may change the number and the arguments (they are reloaded from
 * pt_regs), set orig_rax to -1 to skip the call (the result is rax, -ENOSYS if untouched), and change the
 * result at exit. PTRACE_SYSEMU skips every call and reports no exit stop. */
LT_TEST(syscall_modification_and_sysemu)
{
    struct tracee *e = attach_loop();
    struct lx_user_regs r;
    LT_EQ(PT(LX_PTRACE_SYSCALL, e->tid, 0, 0), 0);
    LT_CHECK(expect_stop(e->tid, 0x57f));
    LT_EQ(PT(LX_PTRACE_GETREGS, e->tid, 0, P(&r)), 0);
    r.orig_rax = 186;                                              /* gettid instead of getpid */
    LT_EQ(PT(LX_PTRACE_SETREGS, e->tid, 0, P(&r)), 0);
    LT_EQ(PT(LX_PTRACE_SYSCALL, e->tid, 0, 0), 0);
    LT_CHECK(expect_stop(e->tid, 0x57f));                          /* exit stop */
    LT_EQ(PT(LX_PTRACE_GETREGS, e->tid, 0, P(&r)), 0);
    LT_EQ(r.rax, e->tid);
    LT_EQ(r.orig_rax, 186);
    r.rax = 4242;                                                  /* change the result */
    LT_EQ(PT(LX_PTRACE_SETREGS, e->tid, 0, P(&r)), 0);
    LT_EQ(PT(LX_PTRACE_SYSCALL, e->tid, 0, 0), 0);
    LT_CHECK(expect_stop(e->tid, 0x57f));                          /* next entry */
    LT_EQ(e->last_ret, 4242);
    LT_EQ(PT(LX_PTRACE_GETREGS, e->tid, 0, P(&r)), 0);
    r.orig_rax = (uint64_t)-1;                                     /* skip, rax stays -ENOSYS */
    LT_EQ(PT(LX_PTRACE_SETREGS, e->tid, 0, P(&r)), 0);
    LT_EQ(PT(LX_PTRACE_SYSCALL, e->tid, 0, 0), 0);
    LT_CHECK(expect_stop(e->tid, 0x57f));                          /* exit stop of the skipped call */
    LT_EQ(PT(LX_PTRACE_GETREGS, e->tid, 0, P(&r)), 0);
    LT_EQ(r.rax, (uint64_t)-38);
    LT_EQ(PT(LX_PTRACE_SYSCALL, e->tid, 0, 0), 0);
    LT_CHECK(expect_stop(e->tid, 0x57f));
    LT_EQ(e->last_ret, -38);
    LT_EQ(PT(LX_PTRACE_GETREGS, e->tid, 0, P(&r)), 0);
    r.orig_rax = (uint64_t)-1;
    r.rax = (uint64_t)-1;                                          /* skip with an error of the tracer's choice */
    LT_EQ(PT(LX_PTRACE_SETREGS, e->tid, 0, P(&r)), 0);
    LT_EQ(PT(LX_PTRACE_SYSCALL, e->tid, 0, 0), 0);
    LT_CHECK(expect_stop(e->tid, 0x57f));
    LT_EQ(PT(LX_PTRACE_SYSCALL, e->tid, 0, 0), 0);
    LT_CHECK(expect_stop(e->tid, 0x57f));
    LT_EQ(e->last_ret, -1);
    /* SYSEMU */
    LT_EQ(PT(LX_PTRACE_GETREGS, e->tid, 0, P(&r)), 0);
    r.rax = 1234;
    LT_EQ(PT(LX_PTRACE_SETREGS, e->tid, 0, P(&r)), 0);
    LT_EQ(PT(LX_PTRACE_SYSEMU, e->tid, 0, 0), 0);
    LT_CHECK(expect_stop(e->tid, 0x57f));                          /* the next call's entry, no exit stop between */
    LT_EQ(PT(LX_PTRACE_GETREGS, e->tid, 0, P(&r)), 0);
    LT_EQ(r.orig_rax, 39);
    LT_EQ(r.rax, (uint64_t)-38);
    LT_EQ(e->last_ret, 1234);
    r.rax = 55;
    LT_EQ(PT(LX_PTRACE_SETREGS, e->tid, 0, P(&r)), 0);
    LT_EQ(PT(LX_PTRACE_SYSEMU, e->tid, 0, 0), 0);
    LT_CHECK(expect_stop(e->tid, 0x57f));
    LT_EQ(e->last_ret, 55);
    LT_EQ(PT(LX_PTRACE_CONT, e->tid, 0, 0), 0);
    finish(e);
}

/* ---- resuming with signals ------------------------------------------------------------------------- */

static int64_t kill_tracee(struct tracee *e, int sig) { return sct(TR, LKD_NR_kill, (uint64_t)e->pid, (uint64_t)sig, 0, 0, 0, 0); }

/* ptrace_resume(): data > _NSIG is EIO; at a signal-delivery-stop CONT(sig) delivers sig (the one the
 * tracer names, not necessarily the original), CONT(0) suppresses; GETSIGINFO shows the siginfo of the
 * signal (SI_USER with the sender's pid for kill()); SETSIGINFO changes what the handler sees. */
LT_TEST(signal_delivery_stop_and_resume_signal)
{
    struct tracee *e = attach_loop();
    LT_EQ(PT(LX_PTRACE_CONT, e->tid, 0, 0), 0);                    /* the SIGSTOP of ATTACH is suppressed */
    usleep(10000);
    fake_engine_reset();
    LT_EQ(kill_tracee(e, 10), 0);
    LT_CHECK(expect_stop(e->tid, 0xa7f));
    struct lx_siginfo si;
    LT_EQ(PT(LX_PTRACE_GETSIGINFO, e->tid, 0, P(&si)), 0);
    LT_EQ(si.si_signo, 10);
    LT_EQ(si.si_code, 0);                                          /* SI_USER */
    LT_EQ(si.u.kill.pid, TR->proc->pid);
    LT_ERRNO(PT(LX_PTRACE_CONT, e->tid, 0, 65), LX_EIO);           /* !valid_signal(65) */
    LT_EQ(PT(LX_PTRACE_CONT, e->tid, 0, 0), 0);                    /* suppressed */
    usleep(20000);
    LT_EQ(fake_frame_build_count, 0);
    LT_EQ(kill_tracee(e, 10), 0);
    LT_CHECK(expect_stop(e->tid, 0xa7f));
    LT_EQ(PT(LX_PTRACE_CONT, e->tid, 0, 10), 0);                   /* delivered: the handler frame is built */
    for (int i = 0; i < 4000 && !fake_frame_build_count; i++) usleep(500);
    LT_EQ(fake_frame_build_count, 1);
    LT_EQ(fake_last_delivery.signo, 10);
    LT_EQ(fake_last_delivery.handler, 0x500000);
    /* the tracer replaces the signal: SIGALRM (14) stop, CONT(SIGUSR2) delivers SIGUSR2 */
    LT_EQ(kill_tracee(e, 14), 0);
    LT_CHECK(expect_stop(e->tid, 0xe7f));
    LT_EQ(PT(LX_PTRACE_CONT, e->tid, 0, 12), 0);
    for (int i = 0; i < 4000 && fake_frame_build_count < 2; i++) usleep(500);
    LT_EQ(fake_last_delivery.signo, 12);
    LT_EQ(fake_last_delivery.info.si_signo, 12);                   /* ptrace_signal(): a fresh siginfo */
    LT_EQ(fake_last_delivery.info.si_code, 0);
    /* SETSIGINFO at the stop reaches the handler */
    LT_EQ(kill_tracee(e, 15), 0);
    LT_CHECK(expect_stop(e->tid, 0xf7f));
    LT_EQ(PT(LX_PTRACE_GETSIGINFO, e->tid, 0, P(&si)), 0);
    si.si_errno = 77;
    LT_EQ(PT(LX_PTRACE_SETSIGINFO, e->tid, 0, P(&si)), 0);
    LT_EQ(PT(LX_PTRACE_CONT, e->tid, 0, 15), 0);
    for (int i = 0; i < 4000 && fake_frame_build_count < 3; i++) usleep(500);
    LT_EQ(fake_last_delivery.info.si_errno, 77);
    LT_ERRNO(PT(LX_PTRACE_GETSIGINFO, e->tid, 0, P(&si)), LX_ESRCH);   /* running again */
    finish(e);
}

/* ptrace_report_syscall(): a signal given at a syscall stop is queued with send_sig(sig, current, 1), i.e.
 * SI_KERNEL, and delivered after the call (not immediately). */
LT_TEST(signal_at_syscall_stop_is_requeued)
{
    struct tracee *e = attach_loop();
    fake_engine_reset();
    LT_EQ(PT(LX_PTRACE_SYSCALL, e->tid, 0, 0), 0);
    LT_CHECK(expect_stop(e->tid, 0x57f));
    LT_EQ(PT(LX_PTRACE_SYSCALL, e->tid, 0, 10), 0);                // the signal rides on the entry stop
    LT_CHECK(expect_stop(e->tid, 0x57f));                          // exit stop: nothing delivered yet
    LT_EQ(fake_frame_build_count, 0);
    LT_EQ(PT(LX_PTRACE_SYSCALL, e->tid, 0, 0), 0);
    LT_CHECK(expect_stop(e->tid, 0xa7f));                          // signal-delivery-stop of the requeued signal
    struct lx_siginfo si;
    LT_EQ(PT(LX_PTRACE_GETSIGINFO, e->tid, 0, P(&si)), 0);
    LT_EQ(si.si_code, 0x80);                                       // SI_KERNEL, sender pid 0
    LT_EQ(si.u.kill.pid, 0);
    LT_EQ(PT(LX_PTRACE_CONT, e->tid, 0, 10), 0);
    for (int i = 0; i < 4000 && !fake_frame_build_count; i++) usleep(500);
    LT_EQ(fake_last_delivery.signo, 10);
    finish(e);
}

/* ---- single step, interrupt, sigmask, arch_prctl ---------------------------------------------------- */

/* arch_has_single_step(): the engine decides. Without ops->set_single_step support the request fails with
 * EIO before anything changes (and the tracee stays stopped); with support the step is requested at resume. */
LT_TEST(single_step_is_honest)
{
    struct tracee *e = attach_loop();
    LT_ERRNO(PT(LX_PTRACE_SINGLESTEP, e->tid, 0, 0), LX_EIO);
    LT_ERRNO(PT(LX_PTRACE_SYSEMU_SINGLESTEP, e->tid, 0, 0), LX_EIO);
    LT_ERRNO(PT(LX_PTRACE_SINGLEBLOCK, e->tid, 0, 0), LX_EIO);
    LT_EQ(PT(LX_PTRACE_GETREGS, e->tid, 0, P(&(struct lx_user_regs){0})), 0);   // still stopped
    fake_single_step_supported = 1;
    LT_EQ(PT(LX_PTRACE_SINGLESTEP, e->tid, 0, 0), 0);
    for (int i = 0; i < 4000 && !fake_cpu_of(e->t)->single_step; i++) usleep(500);
    LT_EQ(fake_cpu_of(e->t)->single_step, 1);
    fake_single_step_supported = 0;
    finish(e);
}

/* PTRACE_SEIZE + PTRACE_INTERRUPT: a PTRACE_EVENT_STOP trap, status (128 << 16) | (SIGTRAP << 8) | 0x7f;
 * non-seized tracees answer EIO to INTERRUPT (kernel/ptrace.c ptrace_request: "!seized -> EIO"). */
LT_TEST(seize_interrupt)
{
    struct tracee *e = attach_loop();
    LT_ERRNO(PT(LX_PTRACE_INTERRUPT, e->tid, 0, 0), LX_EIO);
    finish(e);
    e = tracee_start(body_loop, NULL);
    for (int i = 0; i < 4000 && e->calls < 3; i++) usleep(500);
    LT_EQ(PT(LX_PTRACE_SEIZE, e->tid, 0, 0), 0);
    LT_EQ(PT(LX_PTRACE_INTERRUPT, e->tid, 0, 0), 0);
    LT_CHECK(expect_stop(e->tid, 0x80057f));
    struct lx_siginfo si;
    LT_EQ(PT(LX_PTRACE_GETSIGINFO, e->tid, 0, P(&si)), 0);
    LT_EQ(si.si_signo, 5);
    LT_EQ(si.si_code, (128 << 8) | 5);
    LT_EQ(PT(LX_PTRACE_CONT, e->tid, 0, 0), 0);
    uint64_t n0 = e->calls;
    usleep(20000);
    LT_CHECK(e->calls > n0);
    finish(e);
}

/* PTRACE_INTERRUPT reaches a tracee blocked in a syscall (request ptrace-1): ptrace_trap_notify() wakes it,
 * the wait ends with a restart, and get_signal() runs do_jobctl_trap() first: PTRACE_EVENT_STOP, then the
 * interrupted nanosleep goes on after CONT. Before, the stop only came at the end of the sleep. */
static void body_sleeper(struct tracee *e)
{
    char c;
    int died = 0;
    for (int i = 0; i < 40 && !died; i++) {
        struct lx_timespec ts = { 1, 0 };
        e->last_ret = gsys(e->t, LKD_NR_nanosleep, P(&ts), 0, 0, 0, 0, 0, &died);
        e->calls++;
        if (read(e->rd[0], &c, 1) == 1) break;
    }
    put64(e->wr[1], 1);
    tracee_exit(e, 3);
}

LT_TEST(interrupt_reaches_a_blocked_tracee)
{
    struct tracee *e = tracee_start(body_sleeper, NULL);
    for (int i = 0; i < 4000 && !e->started; i++) usleep(500);
    usleep(50000);                                                          /* inside nanosleep */
    LT_EQ(PT(LX_PTRACE_SEIZE, e->tid, 0, 0), 0);
    int64_t t0 = lkd_host_mono_ns();
    LT_EQ(PT(LX_PTRACE_INTERRUPT, e->tid, 0, 0), 0);
    int st = 0;
    int64_t r = 0;
    for (int i = 0; i < 100 && (r = wait_pid(e->tid, &st, LX_WNOHANG)) == 0; i++) usleep(2000);
    LT_EQ(r, e->tid);
    LT_EQ(st, 0x80057f);
    LT_CHECK(lkd_host_mono_ns() - t0 < 300000000ll);                       /* not at the end of the 1 s sleep */
    LT_EQ(PT(LX_PTRACE_CONT, e->tid, 0, 0), 0);
    finish(e);
}

/* PTRACE_GETSIGMASK/SETSIGMASK: sizeof(sigset_t) is the only valid size; SIGKILL and SIGSTOP cannot be blocked. */
LT_TEST(blocked_mask)
{
    struct tracee *e = attach_loop();
    uint64_t m = 0xff;
    LT_ERRNO(PT(LX_PTRACE_GETSIGMASK, e->tid, 4, P(&m)), LX_EINVAL);
    LT_EQ(PT(LX_PTRACE_GETSIGMASK, e->tid, 8, P(&m)), 0);
    LT_EQ(m, 0);
    m = ~0ull;
    LT_EQ(PT(LX_PTRACE_SETSIGMASK, e->tid, 8, P(&m)), 0);
    LT_ERRNO(PT(LX_PTRACE_SETSIGMASK, e->tid, 16, P(&m)), LX_EINVAL);
    LT_EQ(PT(LX_PTRACE_CONT, e->tid, 0, 0), 0);
    for (int i = 0; i < 4000 && atomic_load(&e->t->sigmask) == 0; i++) usleep(500);
    LT_EQ(atomic_load(&e->t->sigmask), ~0ull & ~((1ull << 8) | (1ull << 18)));   // SIGKILL (9) and SIGSTOP (19)
    finish(e);
}

/* PTRACE_ARCH_PRCTL (arguments reversed: addr = value, data = option): do_arch_prctl_64(). */
LT_TEST(arch_prctl)
{
    struct tracee *e = attach_loop();
    uint64_t v = 0;
    LT_EQ(PT(LX_PTRACE_ARCH_PRCTL, e->tid, P(&v), LX_ARCH_GET_FS), 0);
    LT_EQ(v, 0x7f0000001000ull);
    LT_EQ(PT(LX_PTRACE_ARCH_PRCTL, e->tid, 0x7f0000009000ull, LX_ARCH_SET_FS), 0);
    LT_EQ(PT(LX_PTRACE_ARCH_PRCTL, e->tid, P(&v), LX_ARCH_GET_FS), 0);
    LT_EQ(v, 0x7f0000009000ull);
    LT_ERRNO(PT(LX_PTRACE_ARCH_PRCTL, e->tid, 0x00007ffffffff000ull, LX_ARCH_SET_GS), LX_EPERM);
    LT_ERRNO(PT(LX_PTRACE_ARCH_PRCTL, e->tid, 0, LX_ARCH_GET_CPUID), LX_ENODEV);
    LT_ERRNO(PT(LX_PTRACE_ARCH_PRCTL, e->tid, 0, 0x7777), LX_EINVAL);
    LT_EQ(PT(LX_PTRACE_CONT, e->tid, 0, 0), 0);
    for (int i = 0; i < 4000 && fake_cpu_of(e->t)->regs.fs_base != 0x7f0000009000ull; i++) usleep(500);
    LT_EQ(fake_cpu_of(e->t)->regs.fs_base, 0x7f0000009000ull);
    finish(e);
}

/* ptrace_request() default: EIO for requests the model does not have (seccomp, rseq, syscall user
 * dispatch, LISTEN on a non-seized tracee, 32-bit TLS), after the ESRCH check. */
LT_TEST(unsupported_requests_are_eio)
{
    struct tracee *e = attach_loop();
    char b[64];
    LT_ERRNO(PT(LX_PTRACE_SECCOMP_GET_FILTER, e->tid, 0, P(b)), LX_EIO);
    LT_ERRNO(PT(LX_PTRACE_GET_RSEQ_CONFIGURATION, e->tid, 32, P(b)), LX_EIO);
    LT_ERRNO(PT(LX_PTRACE_GET_SYSCALL_USER_DISPATCH_CONFIG, e->tid, 0, P(b)), LX_EIO);
    LT_ERRNO(PT(LX_PTRACE_LISTEN, e->tid, 0, 0), LX_EIO);
    LT_ERRNO(PT(LX_PTRACE_PEEKSIGINFO, e->tid, 0, P(b)), LX_EIO);
    LT_ERRNO(PT(LX_PTRACE_GET_THREAD_AREA, e->tid, 12, P(b)), LX_EIO);
    LT_ERRNO(PT(0x999, e->tid, 0, 0), LX_EIO);
    LT_ERRNO(PT(0x4300, e->tid, 0, 0), LX_EIO);
    LT_EQ(PT(LX_PTRACE_CONT, e->tid, 0, 0), 0);
    LT_ERRNO(PT(0x999, e->tid, 0, 0), LX_ESRCH);            /* unknown request on a running tracee: ESRCH first */
    finish(e);
}

/* ---- memory ------------------------------------------------------------------------------------------ */

#define RW (LX_PROT_READ | LX_PROT_WRITE)
#define PA (LX_MAP_PRIVATE | LX_MAP_ANONYMOUS)

/* PEEKDATA/POKEDATA (generic_ptrace_peekdata/pokedata): one 8-byte word with FOLL_FORCE; fewer bytes
 * copied is EIO; a bad destination is EFAULT. A word written into executable memory invalidates the
 * translated code of the tracee before the call returns (so before the tracee can run it again). */
LT_TEST(peek_poke_memory)
{
    int64_t a = sct(TR, LKD_NR_mmap, 0, 0x3000, RW, PA, (uint64_t)-1, 0);
    LT_CHECK(a > 0);
    uint8_t *h = (uint8_t *)lkd_g2h((uint64_t)a);
    for (int i = 0; i < 0x3000; i++) h[i] = (uint8_t)(i * 7);
    int64_t code = sct(TR, LKD_NR_mmap, 0, 0x2000, LX_PROT_READ | LX_PROT_EXEC, PA, (uint64_t)-1, 0);
    LT_CHECK(code > 0);
    struct tracee *e = attach_loop();
    uint64_t w = 0;
    LT_EQ(PT(LX_PTRACE_PEEKDATA, e->tid, (uint64_t)a + 0x10, P(&w)), 0);
    uint8_t want[8];
    for (int i = 0; i < 8; i++) want[i] = (uint8_t)((0x10 + i) * 7);
    LT_MEMEQ(&w, want, 8);
    LT_EQ(PT(LX_PTRACE_PEEKTEXT, e->tid, (uint64_t)a + 0x10, P(&w)), 0);
    LT_MEMEQ(&w, want, 8);
    LT_EQ(PT(LX_PTRACE_POKEDATA, e->tid, (uint64_t)a + 0x20, 0x1122334455667788ull), 0);
    LT_EQ(*(uint64_t *)(h + 0x20), 0x1122334455667788ull);
    LT_EQ(PT(LX_PTRACE_POKETEXT, e->tid, (uint64_t)a + 0x2ff8, 0x0102030405060708ull), 0);   /* last word of the map */
    LT_ERRNO(PT(LX_PTRACE_PEEKDATA, e->tid, (uint64_t)a + 0x2ffc, P(&w)), LX_EIO);          /* straddles the end */
    LT_ERRNO(PT(LX_PTRACE_POKEDATA, e->tid, (uint64_t)a + 0x2ffc, 1), LX_EIO);
    LT_ERRNO(PT(LX_PTRACE_PEEKDATA, e->tid, (uint64_t)a - 0x100000, P(&w)), LX_EIO);
    LT_ERRNO(PT(LX_PTRACE_PEEKDATA, e->tid, (uint64_t)a, 0), LX_EFAULT);                     /* put_user fails */
    /* the INT3 pattern of EAC: POKE into read-only code works and invalidates the translations */
    int before = fake_invalidate_count;
    uint64_t orig = 0x89485ed18949ed31ull;
    LT_EQ(PT(LX_PTRACE_POKEDATA, e->tid, (uint64_t)code + 0x1f0, 0x89485ed18949edccull), 0);
    LT_EQ(*(uint8_t *)lkd_g2h((uint64_t)code + 0x1f0), 0xcc);
    LT_EQ(fake_invalidate_count, before + 1);
    LT_EQ(fake_inval_start[before % 16] <= (uint64_t)code + 0x1f0, 1);
    LT_EQ(PT(LX_PTRACE_POKEDATA, e->tid, (uint64_t)code + 0x1f0, orig), 0);
    LT_EQ(fake_invalidate_count, before + 2);
    LT_ERRNO(lkd_user_write((uint64_t)code + 0x1f0, &w, 1), LX_EFAULT);     // still read-only for the guest
    finish(e);
    sct(TR, LKD_NR_munmap, (uint64_t)a, 0x3000, 0, 0, 0, 0);
    sct(TR, LKD_NR_munmap, (uint64_t)code, 0x2000, 0, 0, 0, 0);
}

int main(int argc, char **argv)
{
    if (argc > 1) g_filter = argv[1];
    test_setup(1);
    RUN(attach_errors);
    RUN(seize_arguments);
    RUN(requests_need_a_stopped_tracee_of_the_caller);
    RUN(getregset_formats);
    RUN(setregset_and_putreg_rules);
    RUN(user_area);
    RUN(fp_state);
    RUN(syscall_stops);
    RUN(syscall_modification_and_sysemu);
    RUN(signal_delivery_stop_and_resume_signal);
    RUN(signal_at_syscall_stop_is_requeued);
    RUN(single_step_is_honest);
    RUN(seize_interrupt);
    RUN(sigreturn_has_an_exit_stop);
    RUN(interrupt_reaches_a_blocked_tracee);
    RUN(blocked_mask);
    RUN(arch_prctl);
    RUN(unsupported_requests_are_eio);
    RUN(peek_poke_memory);
    return lt_finish();
}
