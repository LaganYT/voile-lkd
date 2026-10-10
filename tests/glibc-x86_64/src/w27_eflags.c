/* w27_eflags: the EFLAGS that a tracer and a signal handler see, as Linux x86-64 shows them.
 * RFLAGS has 22 defined bits; bits 3, 5, 15 and 22-63 read as 0, bit 1 as 1, and IF is set in user mode. At a
 * syscall-entry stop, the kernel's pt_regs->flags is the R11 that the syscall instruction saved: ptrace's eflags equals
 * the tracee's r11 and holds the arithmetic flags of the instruction before the syscall.
 * 1. Signal frame: flags set by an add, cmp or xor, then tgkill(SIGUSR1) to the witness itself. The handler reads
 *    uc_mcontext.gregs[REG_EFL].
 * 2. ptrace: a child (PTRACE_TRACEME, SIGSTOP) runs the same flag patterns, each followed by a raw getpid. The tracer
 *    follows it with PTRACE_SYSCALL and reads NT_PRSTATUS at each getpid entry stop.
 * 3. The same as 2 after the witness ran code from a page that it keeps writing (the self-modifying code path of the
 *    engine, as EAC's launcher module does), from a child forked by that thread.
 * Prints W27 lines, then W27 PASS or W27 FAIL. */
#define _GNU_SOURCE
#include <elf.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("W27 FAIL " __VA_ARGS__); printf("\n"); fflush(stdout); fails++; } } while (0)

#define F_CF 0x1ull
#define F_ZF 0x40ull
#define F_SF 0x80ull
#define F_IF 0x200ull
#define F_OF 0x800ull
#define STATUS (F_CF | F_ZF | F_SF | F_OF)
/* CF 1 PF AF ZF SF TF IF DF OF IOPL NT RF VM AC VIF VIP ID */
#define DEFINED 0x3f7fd7ull

enum { NPAT = 4 };
static const char *const pat_name[NPAT] = { "xor (ZF)", "add overflow (OF SF)", "add carry (CF ZF)", "cmp below (CF SF)" };
static const uint64_t pat_flags[NPAT] = { F_ZF, F_OF | F_SF, F_CF | F_ZF, F_CF | F_SF };

/* Sets the flags of pattern p, then makes system call nr with a1..a3, without any flag-changing instruction between. */
static long flagged_syscall(int p, long nr, long a1, long a2, long a3)
{
    long ret;
    register long r10 __asm__("r10") = 0;
    switch (p) {
    case 0:
        __asm__ volatile("xor %%ecx, %%ecx\n\tsyscall" : "=a"(ret) : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
                         : "rcx", "r11", "memory", "cc");
        break;
    case 1:
        __asm__ volatile("movabs $0x7fffffffffffffff, %%rcx\n\tadd $1, %%rcx\n\tsyscall" : "=a"(ret)
                         : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10) : "rcx", "r11", "memory", "cc");
        break;
    case 2:
        __asm__ volatile("mov $-1, %%rcx\n\tadd $1, %%rcx\n\tsyscall" : "=a"(ret) : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
                         : "rcx", "r11", "memory", "cc");
        break;
    default:
        __asm__ volatile("mov $1, %%ecx\n\tcmp $2, %%ecx\n\tsyscall" : "=a"(ret) : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
                         : "rcx", "r11", "memory", "cc");
        break;
    }
    return ret;
}

static void check_flags(const char *where, int p, uint64_t fl)
{
    CHECK((fl & ~DEFINED) == 0, "%s %s: eflags %#llx has reserved bits %#llx", where, pat_name[p], (unsigned long long)fl,
          (unsigned long long)(fl & ~DEFINED));
    CHECK((fl & 2) && (fl & F_IF), "%s %s: eflags %#llx without bit 1 or IF", where, pat_name[p], (unsigned long long)fl);
    CHECK((fl & STATUS) == pat_flags[p], "%s %s: CF ZF SF OF %#llx, expected %#llx (eflags %#llx)", where, pat_name[p],
          (unsigned long long)(fl & STATUS), (unsigned long long)pat_flags[p], (unsigned long long)fl);
}

static volatile uint64_t seen_efl;
static void on_usr1(int sig, siginfo_t *si, void *uc_)
{
    (void)sig; (void)si;
    seen_efl = (uint64_t)((ucontext_t *)uc_)->uc_mcontext.gregs[REG_EFL];
}

static void signal_frames(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_usr1;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGUSR1, &sa, NULL);
    const long pid = getpid(), tid = syscall(SYS_gettid);
    for (int p = 0; p < NPAT; p++) {
        seen_efl = 0;
        flagged_syscall(p, SYS_tgkill, pid, tid, SIGUSR1);
        check_flags("signal frame", p, seen_efl);
    }
    printf("W27 signal frames checked\n");
}

/* The tracee: stops, then runs each pattern before a raw getpid, then exits. */
static void tracee(void)
{
    ptrace(PTRACE_TRACEME, 0, 0, 0);
    raise(SIGSTOP);
    for (int p = 0; p < NPAT; p++) flagged_syscall(p, SYS_getpid, 0, 0, 0);
    syscall(SYS_exit_group, 0);
}

static void traced(const char *label)
{
    pid_t c = fork();
    if (c == 0) tracee();
    int st;
    CHECK(waitpid(c, &st, 0) == c && WIFSTOPPED(st) && WSTOPSIG(st) == SIGSTOP, "%s: no initial SIGSTOP (%#x)", label, st);
    ptrace(PTRACE_SETOPTIONS, c, 0, PTRACE_O_TRACESYSGOOD);
    int p = 0, entry = 1, stops = 0;
    for (;;) {
        if (ptrace(PTRACE_SYSCALL, c, 0, 0) != 0) break;
        if (waitpid(c, &st, 0) != c || !WIFSTOPPED(st)) break;
        if (WSTOPSIG(st) != (SIGTRAP | 0x80)) continue;
        struct user_regs_struct r;
        struct iovec iov = { &r, sizeof r };
        CHECK(ptrace(PTRACE_GETREGSET, c, (void *)NT_PRSTATUS, &iov) == 0 && iov.iov_len == sizeof r, "%s: GETREGSET", label);
        if (r.orig_rax == SYS_getpid) {
            if (entry && p < NPAT) {
                check_flags(label, p, r.eflags);
                CHECK(r.eflags == r.r11, "%s %s: eflags %#llx != r11 %#llx at the entry stop", label, pat_name[p],
                      (unsigned long long)r.eflags, (unsigned long long)r.r11);
                stops++;
            }
            if (!entry) p++;
            entry = !entry;
        }
    }
    if (!WIFEXITED(st)) kill(c, SIGKILL), waitpid(c, &st, 0);
    CHECK(stops == NPAT, "%s: %d getpid entry stops, expected %d", label, stops, NPAT);
    printf("W27 %s checked (%d stops)\n", label, stops);
}

/* Code in a page that the loop keeps writing: every store lands next to code the engine has translated. */
static void self_modifying_page(void)
{
    unsigned char *pg = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(pg != MAP_FAILED, "mmap rwx");
    if (pg == MAP_FAILED) return;
    static const unsigned char inc[] = { 0x48, 0x8d, 0x47, 0x01, 0xc3 }; /* lea rax, [rdi+1]; ret */
    memcpy(pg, inc, sizeof inc);
    volatile uint64_t *slot = (volatile uint64_t *)(pg + 2048);
    long (*fn)(long) = (long (*)(long))(void *)pg;
    long v = 0;
    for (int i = 0; i < 20000; i++) {
        *slot = (uint64_t)i;
        v = fn(v);
    }
    CHECK(v == 20000, "self-modifying page: %ld", v);
    munmap(pg, 4096);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    signal_frames();
    traced("ptrace");
    self_modifying_page();
    traced("ptrace after self-modifying code");
    printf(fails ? "W27 FAIL (%d)\n" : "W27 PASS\n", fails);
    return fails != 0;
}
