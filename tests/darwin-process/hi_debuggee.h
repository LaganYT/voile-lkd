/*
 * hi_debuggee.h - a small target process used by the H, I and K witnesses.
 *
 * Every witness binary can play the debuggee when started with argv[1]=="debuggee"
 * (the parent uses hi_spawn_role(&c, "debuggee", NULL)).  The binary is signed with
 * com.apple.security.get-task-allow by build.sh.
 *
 * Layout of the debuggee:
 *   main thread    runs an assembly loop with known register values (x1..x17, x19..x26, x28
 *                  constant, x27 counts, v0..v7 constant) so thread_get_state can be validated;
 *   server thread  reads struct hi_msg commands on fd 3 and answers on fd 4;
 *   park thread    blocked in read(2) on a private pipe (a thread parked in a system call);
 *   more threads   on request (DC_SPAWN_THREADS), parked on a condition variable.
 */
#ifndef HI_DEBUGGEE_H
#define HI_DEBUGGEE_H

#include "hi_util.h"
#include <libkern/OSCacheControl.h>

#define HI_DBG_MAGIC 0x48494442475f4947ull   /* "HIDBG_IG" */

struct hi_dbg_info {
    uint64_t magic;
    uint64_t buf_addr, buf_size;      /* RW anonymous region, 1 MiB */
    uint64_t ro_addr, ro_size;        /* PROT_READ anonymous page(s) */
    uint64_t code_addr;               /* hi_target_fn, in __TEXT */
    uint64_t counter_addr;            /* incremented by the spin loop */
    uint64_t flag_addr;               /* set by hi_escape_fn */
    uint64_t args_addr;               /* 4 words written by hi_escape_fn */
    uint64_t escape_fn;
    uint64_t main_tid, srv_tid, park_tid;
    uint64_t spin_lo, spin_hi;        /* address range of the spin loop */
    uint64_t read_addr;               /* &read in the debuggee, for symbol comparison */
    uint32_t pid;
    uint32_t pad;
};

enum {
    DC_PING = 1, DC_VERIFY_BUF, DC_FILL_BUF, DC_CALL_TARGET, DC_GET_FLAG, DC_RO_READ, DC_EXIT,
    DC_BRK, DC_UDF, DC_BADLOAD, DC_PTRACE_ME, DC_DENY_ATTACH, DC_RAISE, DC_SPAWN_THREADS,
    DC_HANDOFF_TASK, DC_STORE_RO, DC_SELF_PIDINFO, DC_PTRACE, DC_WAITPID, DC_JIT_SETUP, DC_JIT_CALL
};

static struct hi_dbg_info g_dinfo;
static volatile uint64_t g_counter;
static volatile uint64_t g_flag;
static volatile uint64_t g_flag_args[4];
static unsigned char *g_buf;
static unsigned char *g_ro;
static int g_park_pipe[2];
static int g_dbg_rfd = 3, g_dbg_wfd = 4;
static void *g_jit;

__attribute__((noinline, used)) int hi_target_fn(void) { return 1; }
static int (*volatile g_target_fp)(void) = hi_target_fn;

__attribute__((noinline, used)) void hi_escape_fn(uint64_t a, uint64_t b, uint64_t c)
{
    g_flag_args[0] = a; g_flag_args[1] = b; g_flag_args[2] = c;
    g_flag = 0xE5CA9Eull;
    for (;;) usleep(10000);
}

/* The spin loop. x0 = address of the counter. */
__asm__(
    ".text\n"
    ".p2align 4\n"
    ".globl _hi_spin_asm\n"
    "_hi_spin_asm:\n"
    "    movz x1,  #0x1001\n    movz x2,  #0x1002\n    movz x3,  #0x1003\n    movz x4,  #0x1004\n"
    "    movz x5,  #0x1005\n    movz x6,  #0x1006\n    movz x7,  #0x1007\n    movz x8,  #0x1008\n"
    "    movz x9,  #0x1009\n    movz x10, #0x100a\n    movz x11, #0x100b\n    movz x12, #0x100c\n"
    "    movz x13, #0x100d\n    movz x14, #0x100e\n    movz x15, #0x100f\n    movz x16, #0x1010\n"
    "    movz x17, #0x1011\n    movz x19, #0x1013\n    movz x20, #0x1014\n    movz x21, #0x1015\n"
    "    movz x22, #0x1016\n    movz x23, #0x1017\n    movz x24, #0x1018\n    movz x25, #0x1019\n"
    "    movz x26, #0x101a\n    movz x28, #0x101c\n"
    "    movi v0.16b, #0x10\n    movi v1.16b, #0x11\n    movi v2.16b, #0x12\n    movi v3.16b, #0x13\n"
    "    movi v4.16b, #0x14\n    movi v5.16b, #0x15\n    movi v6.16b, #0x16\n    movi v7.16b, #0x17\n"
    "    mov  x27, #0\n"
    ".globl _hi_spin_loop\n"
    "_hi_spin_loop:\n"
    "    add  x27, x27, #1\n"
    "    str  x27, [x0]\n"
    "    b    _hi_spin_loop\n"
    ".globl _hi_spin_end\n"
    "    .p2align 2\n"
    "_hi_spin_end:\n"
    "    nop\n");
extern void hi_spin_asm(volatile uint64_t *counter);
extern char hi_spin_loop[], hi_spin_end[];

/* Registers the spin loop keeps constant: x[n] == 0x1000+n. */
static HI_UNUSED int hi_spin_reg_const(int n)
{
    return (n >= 1 && n <= 17) || (n >= 19 && n <= 26) || n == 28;
}

static pthread_mutex_t g_extra_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_extra_cv = PTHREAD_COND_INITIALIZER;

static void *dbg_park_thread(void *arg)
{
    (void)arg;
    uint64_t tid; pthread_threadid_np(NULL, &tid);
    g_dinfo.park_tid = tid;
    pthread_setname_np("hi-park-read");
    char c;
    for (;;) { ssize_t r = read(g_park_pipe[0], &c, 1); if (r <= 0 && errno != EINTR) break; }
    return NULL;
}

static void *dbg_extra_thread(void *arg)
{
    char nm[32]; snprintf(nm, sizeof nm, "hi-extra-%lu", (unsigned long)(uintptr_t)arg);
    pthread_setname_np(nm);
    pthread_mutex_lock(&g_extra_mu);
    for (;;) pthread_cond_wait(&g_extra_cv, &g_extra_mu);
    return NULL;
}

static uint64_t dbg_x0_trap_brk(void)
{
    register uint64_t x0 __asm__("x0") = 1;
    __asm__ volatile("brk #0x77" : "+r"(x0) : : "memory");
    return x0;
}
static uint64_t dbg_x0_trap_udf(void)
{
    register uint64_t x0 __asm__("x0") = 1;
    __asm__ volatile(".inst 0x00001234" : "+r"(x0) : : "memory");   /* udf #0x1234 */
    return x0;
}
static uint64_t dbg_x0_trap_badload(void)
{
    register uint64_t x0 __asm__("x0") = 1;
    register uint64_t x1 __asm__("x1") = 0x10;
    __asm__ volatile("ldr x0, [x1]" : "+r"(x0) : "r"(x1) : "memory");
    return x0;
}

static void *dbg_server_thread(void *arg)
{
    (void)arg;
    uint64_t tid; pthread_threadid_np(NULL, &tid);
    g_dinfo.srv_tid = tid;
    pthread_setname_np("hi-server");
    /* wait until the park thread has published its tid */
    for (int i = 0; i < 2000 && !g_dinfo.park_tid; i++) usleep(1000);
    if (hi_write_full(g_dbg_wfd, &g_dinfo, sizeof g_dinfo)) _exit(3);
    for (;;) {
        struct hi_msg m, r;
        if (hi_read_full(g_dbg_rfd, &m, sizeof m)) _exit(0);      /* parent went away */
        memset(&r, 0, sizeof r);
        r.cmd = m.cmd;
        switch (m.cmd) {
        case DC_PING: {
            uint64_t t; pthread_threadid_np(NULL, &t);
            r.a[0] = (uint64_t)getpid(); r.a[1] = t;
            break;
        }
        case DC_FILL_BUF: {
            uint64_t off = m.a[0], len = m.a[1], seed = m.a[2];
            if (off + len > g_dinfo.buf_size) { r.rc = EINVAL; break; }
            for (uint64_t i = 0; i < len; i++) g_buf[off + i] = (unsigned char)(seed + i);
            break;
        }
        case DC_VERIFY_BUF: {
            uint64_t off = m.a[0], len = m.a[1], seed = m.a[2];
            if (off + len > g_dinfo.buf_size) { r.rc = EINVAL; break; }
            for (uint64_t i = 0; i < len; i++)
                if (g_buf[off + i] != (unsigned char)(seed + i)) { r.rc = 1; r.a[0] = i; r.a[1] = g_buf[off + i]; break; }
            break;
        }
        case DC_CALL_TARGET:
            if (m.a[0]) sys_icache_invalidate((void *)hi_target_fn, 8);
            r.a[0] = (uint64_t)(int64_t)g_target_fp();
            break;
        case DC_GET_FLAG:
            r.a[0] = g_flag; r.a[1] = g_flag_args[0]; r.a[2] = g_flag_args[1]; r.a[3] = g_flag_args[2];
            break;
        case DC_RO_READ:
            r.a[0] = *(volatile uint64_t *)g_ro;
            break;
        case DC_STORE_RO:    /* writes to the read-only page: faults unless the parent made it writable */
            *(volatile uint64_t *)g_ro = m.a[0];
            r.a[0] = *(volatile uint64_t *)g_ro;
            break;
        case DC_BRK:      r.a[0] = dbg_x0_trap_brk(); break;
        case DC_UDF:      r.a[0] = dbg_x0_trap_udf(); break;
        case DC_BADLOAD:  r.a[0] = dbg_x0_trap_badload(); break;
        case DC_PTRACE_ME:
            errno = 0; r.rc = ptrace(PT_TRACE_ME, 0, 0, 0); r.a[0] = (uint64_t)errno;
            break;
        case DC_DENY_ATTACH:
            errno = 0; r.rc = ptrace(PT_DENY_ATTACH, 0, 0, 0); r.a[0] = (uint64_t)errno;
            break;
        case DC_RAISE:
            r.rc = kill(getpid(), (int)m.a[0]);
            break;
        case DC_SPAWN_THREADS: {
            int made = 0;
            pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 64 * 1024);
            for (uint64_t i = 0; i < m.a[0]; i++) {
                pthread_t t;
                if (pthread_create(&t, &at, dbg_extra_thread, (void *)(uintptr_t)i) == 0) { pthread_detach(t); made++; }
            }
            r.a[0] = (uint64_t)made;
            break;
        }
        case DC_HANDOFF_TASK: {
            /* look up the port the parent registered with mach_ports_register and send it our task port */
            mach_port_array_t ports = NULL; mach_msg_type_number_t n = 0;
            kern_return_t kr = mach_ports_lookup(mach_task_self(), &ports, &n);
            r.a[0] = (uint64_t)kr; r.a[1] = n;
            if (kr == KERN_SUCCESS && n > 0 && ports[0] != MACH_PORT_NULL && ports[0] != MACH_PORT_DEAD) {
                struct { mach_msg_header_t h; mach_msg_body_t b; mach_msg_port_descriptor_t d; } msg;
                memset(&msg, 0, sizeof msg);
                msg.h.msgh_bits = MACH_MSGH_BITS_SET(MACH_MSG_TYPE_COPY_SEND, 0, 0, MACH_MSGH_BITS_COMPLEX);
                msg.h.msgh_size = sizeof msg;
                msg.h.msgh_remote_port = ports[0];
                msg.h.msgh_id = 0x4849;
                msg.b.msgh_descriptor_count = 1;
                msg.d.name = m.a[0] ? mach_task_self() : mach_thread_self();
                msg.d.disposition = MACH_MSG_TYPE_COPY_SEND;
                msg.d.type = MACH_MSG_PORT_DESCRIPTOR;
                kern_return_t ks = mach_msg(&msg.h, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof msg, 0, MACH_PORT_NULL, 2000, MACH_PORT_NULL);
                r.a[2] = (uint64_t)ks;
            }
            break;
        }
        case DC_PTRACE:   /* ptrace(a0, a1, a2, a3) executed inside the debuggee */
            errno = 0;
            r.a[1] = (uint64_t)(int64_t)ptrace((int)m.a[0], (pid_t)m.a[1], (caddr_t)m.a[2], (int)m.a[3]);
            r.a[0] = (uint64_t)errno;
            break;
        case DC_WAITPID: {
            int st = 0;
            errno = 0;
            pid_t w = waitpid((pid_t)m.a[0], &st, (int)m.a[1]);
            r.a[0] = (uint64_t)(int64_t)w; r.a[1] = (uint64_t)(unsigned)st; r.a[2] = (uint64_t)errno;
            break;
        }
        case DC_JIT_SETUP: {   /* a MAP_JIT page holding "mov w0,#1 ; ret" */
            void *p = mmap(NULL, HI_PAGE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
            if (p == MAP_FAILED) { r.rc = errno; break; }
            pthread_jit_write_protect_np(0);
            ((uint32_t *)p)[0] = 0x52800020; ((uint32_t *)p)[1] = 0xd65f03c0;
            pthread_jit_write_protect_np(1);
            sys_icache_invalidate(p, 8);
            g_jit = p;
            r.a[0] = (uint64_t)(uintptr_t)p;
            break;
        }
        case DC_JIT_CALL:
            if (!g_jit) { r.rc = EINVAL; break; }
            if (m.a[0]) sys_icache_invalidate(g_jit, 8);
            r.a[0] = (uint64_t)(int64_t)((int (*)(void))g_jit)();
            break;
        case DC_EXIT:
            hi_write_full(g_dbg_wfd, &r, sizeof r);
            _exit(0);
        default:
            r.rc = ENOSYS;
        }
        if (hi_write_full(g_dbg_wfd, &r, sizeof r)) _exit(0);
    }
    return NULL;
}

/* Never returns. */
static HI_UNUSED void debuggee_main(void)
{
    dp_role = "debuggee";
    if (pipe(g_park_pipe)) _exit(2);
    g_buf = (unsigned char *)mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    g_ro = (unsigned char *)mmap(NULL, HI_PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (g_buf == MAP_FAILED || g_ro == MAP_FAILED) _exit(2);
    memset(g_buf, 0, 1 << 20);
    *(volatile uint64_t *)g_ro = 0x0123456789abcdefull;
    mprotect(g_ro, HI_PAGE, PROT_READ);
    uint64_t tid; pthread_threadid_np(NULL, &tid);
    g_dinfo.magic = HI_DBG_MAGIC;
    g_dinfo.buf_addr = (uint64_t)(uintptr_t)g_buf; g_dinfo.buf_size = 1 << 20;
    g_dinfo.ro_addr = (uint64_t)(uintptr_t)g_ro; g_dinfo.ro_size = HI_PAGE;
    g_dinfo.code_addr = (uint64_t)(uintptr_t)hi_target_fn;
    g_dinfo.counter_addr = (uint64_t)(uintptr_t)&g_counter;
    g_dinfo.flag_addr = (uint64_t)(uintptr_t)&g_flag;
    g_dinfo.args_addr = (uint64_t)(uintptr_t)g_flag_args;
    g_dinfo.escape_fn = (uint64_t)(uintptr_t)hi_escape_fn;
    g_dinfo.main_tid = tid;
    g_dinfo.spin_lo = (uint64_t)(uintptr_t)hi_spin_loop;
    g_dinfo.spin_hi = (uint64_t)(uintptr_t)hi_spin_end;
    g_dinfo.read_addr = (uint64_t)(uintptr_t)&read;
    g_dinfo.pid = (uint32_t)getpid();
    pthread_t t1, t2;
    pthread_create(&t1, NULL, dbg_park_thread, NULL);
    pthread_create(&t2, NULL, dbg_server_thread, NULL);
    pthread_detach(t1); pthread_detach(t2);
    hi_spin_asm(&g_counter);
    _exit(4);
}

/* Parent side: spawns the debuggee and receives its info block. */
static HI_UNUSED int hi_spawn_debuggee(struct hi_child *c, struct hi_dbg_info *info)
{
    if (hi_spawn_role(c, "debuggee", NULL)) return -1;
    if (hi_read_timeout(c->rfd, info, sizeof *info, 5000)) return -2;
    if (info->magic != HI_DBG_MAGIC) return -3;
    return 0;
}

#endif
