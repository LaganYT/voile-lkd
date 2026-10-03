/* test_task_exec.c: execve/execveat preparation and commit. Models fs/exec.c, fs/binfmt_script.c and
 * the binfmt_elf header checks of Linux v6.12. The engine's exec_image is the fake one (records the
 * call, returns fake_exec_result). Cases that resolve files need the fs module (vfs); without it they
 * are reported as skipped. Files live in a private temporary directory removed at the end. */
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ltest.h"
#include "fake_engine.h"
#include "task.h"
#include "vfs.h"
#include "../src/task/task_priv.h"


static int allow_all(uint64_t gva, size_t len, int w) { (void)gva; (void)len; (void)w; return 0; }

static lkd_task *E;   /* the exec'ing process (created with lkd_task_create_main: it has mm, fdt, fsctx) */
static char D[512];   /* temporary directory (host path == guest path, guest root "/") */
static char RD[1024]; /* its canonical path */
static int have_vfs;

static int64_t sct(lkd_task *t, unsigned nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
    lkd_sysreq rq = { nr, { a0, a1, a2, a3, a4, 0 } };
    return lkd_dispatch(t, &rq);
}
#define P(x) ((uint64_t)(uintptr_t)(x))

static unsigned char elf_hdr[64];
static void make_elf(unsigned char *h, int cls, int data, int type, int mach)
{
    memset(h, 0, 64);
    h[0] = 0x7f; h[1] = 'E'; h[2] = 'L'; h[3] = 'F';
    h[4] = (unsigned char)cls; h[5] = (unsigned char)data; h[6] = 1;
    h[16] = (unsigned char)type; h[18] = (unsigned char)mach; h[19] = (unsigned char)(mach >> 8);
}

static void write_file(const char *name, const void *data, size_t n, mode_t mode)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", D, name);
    int fd = open(p, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd >= 0) { (void)!write(fd, data, n); close(fd); }
    chmod(p, mode);
}

static char *path(const char *name)
{
    static char buf[8][1024];
    static int i;
    char *b = buf[i++ & 7];
    snprintf(b, 1024, "%s/%s", D, name);
    return b;
}

/* binfmt_elf: ELF64, little endian, ET_EXEC or ET_DYN, EM_X86_64. 32-bit x86 would need IA32
 * emulation, which LKD does not have: ENOEXEC. Anything else without "#!" is ENOEXEC. */
LT_TEST(classify_headers)
{
    unsigned char h[64];
    make_elf(h, 2, 1, 2, 62);
    LT_EQ(exec_classify(h, 64), 1);
    make_elf(h, 2, 1, 3, 62);
    LT_EQ(exec_classify(h, 64), 1);
    make_elf(h, 2, 1, 1, 62);                       /* ET_REL */
    LT_ERRNO(exec_classify(h, 64), LX_ENOEXEC);
    make_elf(h, 1, 1, 2, 3);                        /* ELFCLASS32, EM_386 */
    LT_ERRNO(exec_classify(h, 64), LX_ENOEXEC);
    make_elf(h, 2, 1, 3, 183);                      /* EM_AARCH64 */
    LT_ERRNO(exec_classify(h, 64), LX_ENOEXEC);
    make_elf(h, 2, 2, 2, 62);                       /* big endian */
    LT_ERRNO(exec_classify(h, 64), LX_ENOEXEC);
    make_elf(h, 2, 1, 2, 62);
    LT_ERRNO(exec_classify(h, 12), LX_ENOEXEC);     /* truncated header */
    LT_EQ(exec_classify((const unsigned char *)"#!/bin/sh\n", 10), 2);
    unsigned char macho[4] = { 0xcf, 0xfa, 0xed, 0xfe };
    LT_ERRNO(exec_classify(macho, 4), LX_ENOEXEC);
    LT_ERRNO(exec_classify((const unsigned char *)"", 0), LX_ENOEXEC);
    LT_ERRNO(exec_classify((const unsigned char *)"hello", 5), LX_ENOEXEC);
}

/* load_script(): one optional argument (the rest of the line, trailing blanks trimmed), the name ends at
 * the first blank, a line without interpreter is ENOEXEC, a truncated interpreter path is ENOEXEC. */
LT_TEST(shebang_parsing)
{
    char in[300], arg[300];
    LT_EQ(exec_parse_shebang((const unsigned char *)"#!/bin/sh\n", 10, in, sizeof in, arg, sizeof arg), 0);
    LT_STREQ(in, "/bin/sh");
    LT_STREQ(arg, "");
    const char *s = "#! /usr/bin/env  python3 -u  \t\nrest";
    LT_EQ(exec_parse_shebang((const unsigned char *)s, strlen(s), in, sizeof in, arg, sizeof arg), 0);
    LT_STREQ(in, "/usr/bin/env");
    LT_STREQ(arg, "python3 -u");
    LT_ERRNO(exec_parse_shebang((const unsigned char *)"#!\n", 3, in, sizeof in, arg, sizeof arg), LX_ENOEXEC);
    LT_ERRNO(exec_parse_shebang((const unsigned char *)"#!  \t \n", 7, in, sizeof in, arg, sizeof arg), LX_ENOEXEC);
    LT_EQ(exec_parse_shebang((const unsigned char *)"#!/bin/sh", 9, in, sizeof in, arg, sizeof arg), 0); /* no newline, short file */
    LT_STREQ(in, "/bin/sh");
    unsigned char longl[256];
    memset(longl, 'a', sizeof longl);
    longl[0] = '#'; longl[1] = '!'; longl[2] = '/';
    LT_ERRNO(exec_parse_shebang(longl, 256, in, sizeof in, arg, sizeof arg), LX_ENOEXEC);  /* path truncated */
    longl[10] = ' ';
    LT_EQ(exec_parse_shebang(longl, 256, in, sizeof in, arg, sizeof arg), 0);              /* arg truncated: fine */
    LT_EQ(strlen(in), 8);
    LT_EQ(strlen(arg), 255 - 11);
}

/* do_execveat_common/bprm_execve error paths, before the point of no return: the process is unchanged. */
LT_TEST(exec_errors)
{
    if (!have_vfs) { LT_CHECK(1); printf("SKIP exec_errors: no vfs (fs module not linked)\n"); return; }
    char *argv[] = { "x", NULL };
    char *envp[] = { NULL };
    int before = fake_exec_count;
    LT_ERRNO(sct(E, LKD_NR_execve, P(path("noexec")), P(argv), P(envp), 0, 0), LX_EACCES);
    LT_ERRNO(sct(E, LKD_NR_execve, P(path("text")), P(argv), P(envp), 0, 0), LX_ENOEXEC);
    LT_ERRNO(sct(E, LKD_NR_execve, P(D), P(argv), P(envp), 0, 0), LX_EACCES);
    LT_ERRNO(sct(E, LKD_NR_execve, P(path("missing")), P(argv), P(envp), 0, 0), LX_ENOENT);
    LT_ERRNO(sct(E, LKD_NR_execve, P(path("badinterp")), P(argv), P(envp), 0, 0), LX_ENOENT);
    LT_ERRNO(sct(E, LKD_NR_execve, P(path("s6")), P(argv), P(envp), 0, 0), LX_ELOOP);   /* 7 nested scripts */
    LT_ERRNO(sct(E, LKD_NR_execve, P(path("i386")), P(argv), P(envp), 0, 0), LX_ENOEXEC);
    LT_ERRNO(sct(E, LKD_NR_execve, P(""), P(argv), P(envp), 0, 0), LX_ENOENT);
    LT_ERRNO(sct(E, LKD_NR_execve, 8, P(argv), P(envp), 0, 0), LX_EFAULT);
    LT_ERRNO(sct(E, LKD_NR_execveat, LX_AT_FDCWD, P(path("prog")), P(argv), P(envp), 0x8000), LX_EINVAL);
    LT_ERRNO(sct(E, LKD_NR_execveat, LX_AT_FDCWD, P(path("link")), P(argv), P(envp), LX_AT_SYMLINK_NOFOLLOW), LX_ELOOP);
    char *big = malloc(140000);
    memset(big, 'a', 139999);
    big[139999] = 0;
    char *bigv[] = { "x", big, NULL };
    LT_ERRNO(sct(E, LKD_NR_execve, P(path("prog")), P(bigv), P(envp), 0, 0), LX_E2BIG);  /* MAX_ARG_STRLEN */
    free(big);
    LT_EQ(fake_exec_count, before);
}

/* A script with an interpreter and an argument: argv becomes interp, arg, script path, argv[1..];
 * /proc/self/exe is the interpreter (the file loaded), comm is the script's basename
 * (__set_task_comm(kbasename(bprm->filename))). Five nested scripts are still allowed. */
LT_TEST(exec_script_success)
{
    if (!have_vfs) { LT_CHECK(1); printf("SKIP exec_script_success\n"); return; }
    fake_exec_result = 0;
    int before = fake_exec_count;
    char *argv[] = { "ignored0", "a1", NULL };
    char *envp[] = { "K=V", NULL };
    E->clear_child_tid = 0x1234;
    int64_t r = sct(E, LKD_NR_execve, P(path("script")), P(argv), P(envp), 0, 0);
    LT_EQ(r, LKD_RET_CONTEXT_REPLACED);
    LT_EQ(fake_exec_count, before + 1);
    LT_STREQ(E->comm, "script");
    char want_exe[1100];
    snprintf(want_exe, sizeof want_exe, "%s/prog", RD);
    LT_STREQ(E->proc->exe_path, want_exe);
    char want_cmd[3000];
    int n = snprintf(want_cmd, sizeof want_cmd, "%s/prog%c-x%c%s/script%ca1", D, 0, 0, D, 0) + 1;
    LT_EQ(E->proc->cmdline_len, n);
    LT_MEMEQ(E->proc->cmdline, want_cmd, (size_t)n);
    LT_EQ(E->proc->environ_len, 4);
    LT_EQ(E->clear_child_tid, 0);             /* exec_mm_release() */
    lkd_proc_info pi;
    LT_EQ(lkd_proc_lookup(E->proc->pid, &pi), 0);
    LT_STREQ(pi.exe, want_exe);
    LT_STREQ(pi.comm, "script");
    LT_EQ(pi.dumpable, 1);
    /* exec_binprm(): depth > 5 is ELOOP, so 5 nested scripts (s4 .. s0) load and 6 (s5) do not */
    r = sct(E, LKD_NR_execve, P(path("s4")), P(argv), P(envp), 0, 0);
    LT_EQ(r, LKD_RET_CONTEXT_REPLACED);
    LT_ERRNO(sct(E, LKD_NR_execve, P(path("s5")), P(argv), P(envp), 0, 0), LX_ELOOP);
    /* an execute-only binary makes the process non-dumpable (would_dump) */
    r = sct(E, LKD_NR_execve, P(path("xonly")), P(argv), P(envp), 0, 0);
    if (r == LKD_RET_CONTEXT_REPLACED) LT_EQ(E->proc->dumpable, 0);
    else LT_ERRNO(r, LX_EACCES);             /* the host refuses to open --x files for reading: documented */
    E->proc->dumpable = 1;
}

/* execveat(fd, "", AT_EMPTY_PATH): fexecve; the filename is /dev/fd/N and the comm is "N". */
LT_TEST(execveat_empty_path)
{
    if (!have_vfs) { LT_CHECK(1); printf("SKIP execveat_empty_path\n"); return; }
    int64_t fd = sct(E, LKD_NR_openat, (uint64_t)LX_AT_FDCWD, P(path("prog")), LX_O_RDONLY, 0, 0);
    LT_CHECK(fd >= 0);
    char *argv[] = { "p", NULL };
    char *envp[] = { NULL };
    LT_EQ(sct(E, LKD_NR_execveat, (uint64_t)fd, P(""), P(argv), P(envp), LX_AT_EMPTY_PATH), LKD_RET_CONTEXT_REPLACED);
    char want[16];
    snprintf(want, sizeof want, "%d", (int)fd);
    LT_STREQ(E->comm, want);
    LT_ERRNO(sct(E, LKD_NR_execveat, 9999, P(""), P(argv), P(envp), LX_AT_EMPTY_PATH), LX_EBADF);
}

static void put32(int fd, int32_t v) { (void)!write(fd, &v, sizeof v); }
static int32_t get32(int fd)
{
    int32_t v = -12345;
    if (read(fd, &v, sizeof v) != (ssize_t)sizeof v) return -12345;
    return v;
}

static lkd_task *g_victim;
static void *release_when_killed(void *arg)
{
    (void)arg;
    for (int i = 0; i < 2000 && !(atomic_load(&g_victim->flags) & LKD_TF_FATAL); i++) usleep(1000);
    lkd_task_release(g_victim, NULL);   /* what the engine does for a thread that must die */
    return NULL;
}

/* de_thread(): exec from a non-leader thread kills the other threads (the leader included) and the
 * caller takes over the leader's tid; the process keeps its pid. */
LT_TEST(exec_from_non_leader_thread)
{
    if (!have_vfs) { LT_CHECK(1); printf("SKIP exec_from_non_leader_thread\n"); return; }
    int32_t pid = E->proc->pid;
    lkd_task *th = fake_make_thread(E);
    fake_cpu_of(th);
    LT_EQ(E->proc->nthreads, 2);
    g_victim = E;
    pthread_t h;
    pthread_create(&h, NULL, release_when_killed, NULL);
    char *argv[] = { "p", NULL };
    char *envp[] = { NULL };
    int before = fake_exec_count;
    lkd_set_current(th);
    LT_EQ(sct(th, LKD_NR_execve, P(path("prog")), P(argv), P(envp), 0, 0), LKD_RET_CONTEXT_REPLACED);
    pthread_join(h, NULL);
    LT_EQ(fake_exec_count, before + 1);
    LT_EQ(th->tid, pid);
    LT_EQ(th->proc->nthreads, 1);
    LT_EQ(sct(th, LKD_NR_gettid, 0, 0, 0, 0, 0), pid);
    lkd_tid_info ti;
    LT_EQ(lkd_tid_lookup(pid, &ti), 0);
    LT_EQ(ti.state, 'R');
    int32_t tids[4];
    LT_EQ(lkd_tid_list(pid, tids, 4), 1);
    E = th;
}

/* A child that execs: setpgid(2) by the parent then answers EACCES (PF_FORKNOEXEC cleared). */
LT_TEST(setpgid_after_child_exec)
{
    if (!have_vfs) { LT_CHECK(1); printf("SKIP setpgid_after_child_exec\n"); return; }
    int pfd[2], go[2];
    LT_EQ(pipe(pfd), 0);
    LT_EQ(pipe(go), 0);
    fflush(NULL);
    int64_t pid = sct(E, LKD_NR_fork, 0, 0, 0, 0, 0);
    if (pid == 0) {
        alarm(20);
        lkd_task *c = lkd_current();
        char *argv[] = { "p", NULL };
        char *envp[] = { NULL };
        put32(pfd[1], (int32_t)(sct(c, LKD_NR_execve, P(path("prog")), P(argv), P(envp), 0, 0) == LKD_RET_CONTEXT_REPLACED));
        get32(go[0]);
        sct(c, LKD_NR_exit_group, 0, 0, 0, 0, 0);
        int st;
        if (lkd_task_release(c, &st)) _exit(st);
        _exit(99);
    }
    LT_EQ(get32(pfd[0]), 1);
    LT_ERRNO(sct(E, LKD_NR_setpgid, (uint64_t)pid, (uint64_t)pid, 0, 0, 0), LX_EACCES);
    put32(go[1], 1);
    int status = -1;
    LT_EQ(sct(E, LKD_NR_wait4, (uint64_t)pid, P(&status), 0, 0, 0), pid);
    LT_EQ(status, 0);
    close(pfd[0]); close(pfd[1]); close(go[0]); close(go[1]);
}

struct vf { int64_t ret; volatile int done; };
static void *vfork_thread(void *arg)
{
    struct vf *v = arg;
    lkd_set_current(E);
    v->ret = sct(E, LKD_NR_vfork, 0, 0, 0, 0, 0);
    v->done = 1;
    return NULL;
}

/* posix_spawn/vfork child that execs: it leaves the parent's host process for a host fork() child,
 * keeps its pid, and the parent is released (complete_vfork_done at exec_mm_release). */
LT_TEST(vfork_child_exec_moves_to_its_own_host_process)
{
    if (!have_vfs) { LT_CHECK(1); printf("SKIP vfork_child_exec\n"); return; }
    fake_engine_reset();
    int pfd[2];
    LT_EQ(pipe(pfd), 0);
    struct vf v = { 0, 0 };
    pthread_t h;
    pthread_create(&h, NULL, vfork_thread, &v);
    for (int i = 0; i < 400 && !fake_last_spawned; i++) usleep(5000);
    lkd_task *c = fake_last_spawned;
    LT_CHECK(c != NULL);
    if (!c) { pthread_join(h, NULL); return; }
    int32_t cpid = c->proc->pid, ppid = E->proc->pid;
    lkd_proc_info pi;
    LT_EQ(lkd_proc_lookup(cpid, &pi), 0);
    LT_EQ(pi.host_pid, getpid());              /* cohosted until exec */
    lkd_set_current(c);
    fake_cpu_of(c);
    char *argv[] = { "child-arg0", "x", NULL };
    char *envp[] = { NULL };
    fflush(NULL);
    int64_t r = sct(c, LKD_NR_execve, P(path("prog")), P(argv), P(envp), 0, 0);
    if (r == LKD_RET_CONTEXT_REPLACED) {
        /* the new host process */
        alarm(20);
        put32(pfd[1], (int32_t)sct(c, LKD_NR_getpid, 0, 0, 0, 0, 0));
        put32(pfd[1], (int32_t)sct(c, LKD_NR_getppid, 0, 0, 0, 0, 0));
        put32(pfd[1], (int32_t)getpid());
        sct(c, LKD_NR_exit_group, 4, 0, 0, 0, 0);
        int st;
        if (lkd_task_release(c, &st)) _exit(st);
        _exit(99);
    }
    LT_EQ(r, LKD_RET_EXIT_THREAD);             /* old host thread ends silently */
    LT_EQ(lkd_task_release(c, NULL), 0);
    lkd_set_current(E);
    pthread_join(h, NULL);
    LT_EQ(v.done, 1);
    LT_EQ(v.ret, cpid);
    LT_EQ(get32(pfd[0]), cpid);
    LT_EQ(get32(pfd[0]), ppid);
    int32_t host = get32(pfd[0]);
    LT_CHECK(host > 0 && host != getpid());
    int status = -1;
    LT_EQ(sct(E, LKD_NR_wait4, (uint64_t)cpid, P(&status), 0, 0, 0), cpid);
    LT_EQ(status, 4 << 8);
    close(pfd[0]); close(pfd[1]);
}

/* After the point of no return a failing loader kills the process with SIGSEGV (bprm_execve:
 * force_fatal_sig). */
LT_TEST(exec_image_failure_is_fatal)
{
    if (!have_vfs) { LT_CHECK(1); printf("SKIP exec_image_failure_is_fatal\n"); return; }
    fake_exec_result = -LX_ENOMEM;
    char *argv[] = { "p", NULL };
    char *envp[] = { NULL };
    LT_EQ(sct(E, LKD_NR_execve, P(path("prog")), P(argv), P(envp), 0, 0), LKD_RET_EXIT_GROUP);
    LT_EQ(E->proc->wait->group_exit, 1);
    LT_EQ(E->proc->wait->group_status, LX_SIGSEGV);
    fake_exec_result = 0;
}

int main(void)
{
    LT_EQ(fake_engine_init(), 0);
    lkd_usermem_set_check_fn(allow_all);
    const char *tmp = getenv("TMPDIR");
    snprintf(D, sizeof D, "%s/lkd-task-exec-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(D)) { perror("mkdtemp"); return 1; }
    if (!realpath(D, RD)) snprintf(RD, sizeof RD, "%s", D);
    make_elf(elf_hdr, 2, 1, 2, 62);
    write_file("prog", elf_hdr, 64, 0755);
    write_file("noexec", elf_hdr, 64, 0644);
    write_file("xonly", elf_hdr, 64, 0111);
    unsigned char h32[64];
    make_elf(h32, 1, 1, 2, 3);
    write_file("i386", h32, 64, 0755);
    write_file("text", "hello\n", 6, 0755);
    char line[1200];
    snprintf(line, sizeof line, "#!%s/prog -x\n", D);
    write_file("script", line, strlen(line), 0755);
    snprintf(line, sizeof line, "#!%s/missing\n", D);
    write_file("badinterp", line, strlen(line), 0755);
    snprintf(line, sizeof line, "#!%s/prog\n", D);
    write_file("s0", line, strlen(line), 0755);
    for (int i = 1; i <= 6; i++) {
        char nm[8];
        snprintf(line, sizeof line, "#!%s/s%d\n", D, i - 1);
        snprintf(nm, sizeof nm, "s%d", i);
        write_file(nm, line, strlen(line), 0755);
    }
    symlink(path("prog"), path("link"));
    static const char *argv0[] = { "/bin/test", NULL };
    lkd_main_spec spec = { .size = sizeof spec, .exe_path = "/bin/test", .argv = argv0, .cwd = "/" };
    E = lkd_task_create_main(&spec);
    lkd_set_current(E);
    fake_cpu_of(E);
    struct lx_stat st;
    have_vfs = E && lkd_vfs_stat(E, LX_AT_FDCWD, "/", 0, &st) == 0;
    LT_RUN(classify_headers);
    LT_RUN(shebang_parsing);
    LT_RUN(exec_errors);
    LT_RUN(exec_script_success);
    LT_RUN(execveat_empty_path);
    LT_RUN(exec_from_non_leader_thread);
    LT_RUN(setpgid_after_child_exec);
    LT_RUN(vfork_child_exec_moves_to_its_own_host_process);
    LT_RUN(exec_image_failure_is_fatal);
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", D);
    (void)!system(cmd);
    return lt_finish();
}
