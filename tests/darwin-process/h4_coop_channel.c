/*
 * h4_coop_channel.c - H: reading another process's memory through a COOPERATIVE channel, against the Mach task port.
 *
 * The target (a binary started with exec, so unrelated to the tracer in the address-space sense) owns a private 4 MiB
 * "guest memory" area.  The tracer wants len bytes at an address of that area.  Methods compared, all measured
 * as the time from "tracer starts the request" to "bytes are in the tracer's own buffer":
 *
 *   direct-shm       the guest area lives in a shared mapping: plain memcpy, no request at all
 *   vm_read          mach_vm_read_overwrite through the task port (task_for_pid on the own child)
 *   vm_remap         memcpy from a mach_vm_remap alias of the target's private memory
 *   spin             request/response words in shared memory, both sides busy-poll (lower bound of any scheme)
 *   futex            same words, both sides sleep in os_sync_wait_on_address (SHARED) and are woken by os_sync_wake
 *   pthread-shared   process-shared mutex and condition variables in the shared page
 *   sem              two named POSIX semaphores
 *   pipe-notify      request over a pipe, one-byte reply over a pipe, payload through shared memory
 *   pipe-inline      request over a pipe, payload returned over a pipe
 *   unix-stream      like pipe, one AF_UNIX stream socketpair, payload through shared memory
 *   unix-dgram       AF_UNIX datagram socketpair, payload through shared memory
 *   mach-msg         Mach message with the payload inline in the reply (needs mach_ports_register to survive exec)
 *
 * Two QoS settings of both threads are measured: default and user-interactive.
 */
#include "hi_util.h"
#include <stdatomic.h>

#define QUIT (~0ull)
#define SH_DATA_OFF (16u * 1024)
#define SH_DATA_SIZE (2u << 20)
#define SH_TOTAL (SH_DATA_OFF + SH_DATA_SIZE + (4u << 20))
#define GUEST_SIZE (4u << 20)

enum { M_NONE = 0xff, M_SPIN = 0, M_FUTEX, M_PTHREAD, M_SEM, M_PIPE_NOTIFY, M_PIPE_INLINE, M_STREAM, M_DGRAM, M_MACH, M_COUNT };
static const char *mname[] = {"spin", "futex", "pthread-shared", "sem", "pipe-notify", "pipe-inline", "unix-stream", "unix-dgram", "mach-msg"};

struct sh {
    _Alignas(128) _Atomic uint32_t req_seq;
    _Alignas(128) _Atomic uint32_t rsp_seq;
    _Alignas(128) _Atomic uint32_t f_req;
    _Alignas(128) _Atomic uint32_t f_rsp;
    _Alignas(128) volatile uint64_t req_addr;
    volatile uint64_t req_len;
    _Alignas(128) pthread_mutex_t mu;
    pthread_cond_t cv_req, cv_rsp;
    uint32_t pt_req, pt_rsp;
    _Alignas(128) _Atomic uint32_t active;
    _Atomic uint32_t qos;     /* 0 default, 1 user-interactive */
};

static struct sh *g_sh;
static uint8_t *g_data;        /* shared payload area */
static uint8_t *g_shared_guest;/* guest area inside the shared mapping (for direct-shm) */
static uint8_t *g_guest;       /* target private guest memory (target process) */
static int fd_req_r, fd_rsp_w, fd_sock_s, fd_sock_d;
static sem_t *sem_req, *sem_rsp;
static char shname[64], semname_r[64], semname_s[64];

struct creq { uint64_t addr, len; };

static void apply_qos(uint32_t *cur)
{
    uint32_t want = atomic_load_explicit(&g_sh->qos, memory_order_relaxed);
    if (want != *cur) {
        pthread_set_qos_class_self_np(want ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_DEFAULT, 0);
        *cur = want;
    }
}

/* ============================ target side ============================== */
static mach_port_t t_mach_svc;

static void *svc_spin(void *arg)
{
    (void)arg; uint32_t last = 0, q = 99;
    for (;;) {
        while (atomic_load_explicit(&g_sh->active, memory_order_relaxed) != M_SPIN) usleep(500);
        apply_qos(&q);
        for (;;) {
            uint32_t s;
            while ((s = atomic_load_explicit(&g_sh->req_seq, memory_order_acquire)) == last) {
                if (atomic_load_explicit(&g_sh->active, memory_order_relaxed) != M_SPIN) goto out;
                __asm__ volatile("yield");
            }
            last = s;
            uint64_t len = g_sh->req_len;
            if (len != QUIT) memcpy(g_data, (void *)(uintptr_t)g_sh->req_addr, len);
            atomic_store_explicit(&g_sh->rsp_seq, s, memory_order_release);
            if (len == QUIT) break;
        }
    out:;
    }
    return NULL;
}

static void *svc_futex(void *arg)
{
    (void)arg; uint32_t last = 0, q = 99;
    for (;;) {
        uint32_t s = atomic_load_explicit(&g_sh->f_req, memory_order_acquire);
        if (s == last) { hi_futex_wait((volatile uint32_t *)&g_sh->f_req, last); continue; }
        apply_qos(&q);
        last = s;
        uint64_t len = g_sh->req_len;
        if (len != QUIT) memcpy(g_data, (void *)(uintptr_t)g_sh->req_addr, len);
        atomic_store_explicit(&g_sh->f_rsp, s, memory_order_release);
        hi_futex_wake((volatile uint32_t *)&g_sh->f_rsp);
    }
    return NULL;
}

static void *svc_pthread(void *arg)
{
    (void)arg; uint32_t last = 0, q = 99;
    for (;;) {
        pthread_mutex_lock(&g_sh->mu);
        while (g_sh->pt_req == last) pthread_cond_wait(&g_sh->cv_req, &g_sh->mu);
        last = g_sh->pt_req;
        pthread_mutex_unlock(&g_sh->mu);
        apply_qos(&q);
        uint64_t len = g_sh->req_len;
        if (len != QUIT) memcpy(g_data, (void *)(uintptr_t)g_sh->req_addr, len);
        pthread_mutex_lock(&g_sh->mu);
        g_sh->pt_rsp = last;
        pthread_cond_signal(&g_sh->cv_rsp);
        pthread_mutex_unlock(&g_sh->mu);
    }
    return NULL;
}

static void *svc_sem(void *arg)
{
    (void)arg; uint32_t q = 99;
    for (;;) {
        while (sem_wait(sem_req) != 0 && errno == EINTR) ;
        apply_qos(&q);
        uint64_t len = g_sh->req_len;
        if (len != QUIT) memcpy(g_data, (void *)(uintptr_t)g_sh->req_addr, len);
        sem_post(sem_rsp);
    }
    return NULL;
}

#define INLINE_FLAG (1ull << 62)
static void *svc_pipe(void *arg)
{
    (void)arg; uint32_t q = 99;
    for (;;) {
        struct creq rq;
        if (hi_read_full(fd_req_r, &rq, sizeof rq)) return NULL;
        apply_qos(&q);
        if (rq.len & INLINE_FLAG) {
            hi_write_full(fd_rsp_w, (void *)(uintptr_t)rq.addr, rq.len & ~INLINE_FLAG);
        } else {
            memcpy(g_data, (void *)(uintptr_t)rq.addr, rq.len);
            char x = 'k';
            hi_write_full(fd_rsp_w, &x, 1);
        }
    }
}

static void *svc_sock(void *arg)
{
    int fd = (int)(intptr_t)arg; uint32_t q = 99;
    for (;;) {
        struct creq rq;
        ssize_t r = recv(fd, &rq, sizeof rq, 0);
        if (r != (ssize_t)sizeof rq) { if (r < 0 && errno == EINTR) continue; return NULL; }
        apply_qos(&q);
        if (rq.len != QUIT) memcpy(g_data, (void *)(uintptr_t)rq.addr, rq.len);
        char x = 'k';
        send(fd, &x, 1, 0);
    }
}

struct mbuf { mach_msg_header_t h; uint8_t body[65536 + 256]; };
static void *svc_mach(void *arg)
{
    (void)arg; uint32_t q = 99;
    struct mbuf *m = malloc(sizeof *m);
    kern_return_t kr = mach_msg(&m->h, MACH_RCV_MSG, 0, sizeof *m, t_mach_svc, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    for (;;) {
        if (kr != MACH_MSG_SUCCESS) return NULL;
        apply_qos(&q);
        uint64_t addr, len;
        memcpy(&addr, m->body, 8); memcpy(&len, m->body + 8, 8);
        mach_port_t rp = m->h.msgh_remote_port;
        uint32_t n = len == QUIT ? 0 : (uint32_t)len;
        if (n) memcpy(m->body, (void *)(uintptr_t)addr, n);
        m->h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
        m->h.msgh_remote_port = rp;
        m->h.msgh_local_port = MACH_PORT_NULL;
        m->h.msgh_id = 2;
        m->h.msgh_size = (mach_msg_size_t)(sizeof(mach_msg_header_t) + ((n + 3) & ~3u));
        kr = mach_msg(&m->h, MACH_SEND_MSG | MACH_RCV_MSG, m->h.msgh_size, sizeof *m, t_mach_svc, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    }
}

static int open_shared(const char *name, int create)
{
    int fd = shm_open(name, create ? (O_CREAT | O_RDWR | O_EXCL) : O_RDWR, 0600);
    if (fd < 0) return -1;
    if (create && ftruncate(fd, SH_TOTAL) != 0) { close(fd); return -1; }
    void *p = mmap(NULL, SH_TOTAL, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return -1;
    g_sh = p;
    g_data = (uint8_t *)p + SH_DATA_OFF;
    g_shared_guest = g_data + SH_DATA_SIZE;
    return 0;
}

static int target_main(const char *name)
{
    dp_role = "target";
    signal(SIGPIPE, SIG_IGN);
    snprintf(shname, sizeof shname, "%s", name);
    if (open_shared(shname, 0)) _exit(10);
    fd_req_r = 5; fd_rsp_w = 6; fd_sock_s = 7; fd_sock_d = 8;
    g_guest = mmap(NULL, GUEST_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    for (size_t i = 0; i < GUEST_SIZE; i++) g_guest[i] = (uint8_t)(i * 131 + 7);
    snprintf(semname_r, sizeof semname_r, "%s.r", name + 1);
    snprintf(semname_s, sizeof semname_s, "%s.s", name + 1);
    char a[40], b[40];
    snprintf(a, sizeof a, "/%s", semname_r); snprintf(b, sizeof b, "/%s", semname_s);
    sem_req = sem_open(a, 0); sem_rsp = sem_open(b, 0);
    struct { uint64_t guest; uint32_t pid; int32_t sem_ok; int32_t mach_ok; int32_t mach_kr; } hello = {(uint64_t)(uintptr_t)g_guest, (uint32_t)getpid(), sem_req != SEM_FAILED && sem_rsp != SEM_FAILED, 0, 0};

    /* Mach: find the port the tracer registered before the spawn, create our service port, send it over */
    mach_port_t tracer_port = MACH_PORT_NULL;
    kern_return_t kr = task_get_special_port(mach_task_self(), TASK_BOOTSTRAP_PORT, &tracer_port);
    hello.mach_kr = kr;
    if (kr == KERN_SUCCESS && tracer_port != MACH_PORT_NULL && tracer_port != MACH_PORT_DEAD) {
        mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &t_mach_svc);
        mach_port_insert_right(mach_task_self(), t_mach_svc, t_mach_svc, MACH_MSG_TYPE_MAKE_SEND);
        struct { mach_msg_header_t h; mach_msg_body_t b; mach_msg_port_descriptor_t d; } msg;
        memset(&msg, 0, sizeof msg);
        msg.h.msgh_bits = MACH_MSGH_BITS_SET(MACH_MSG_TYPE_COPY_SEND, 0, 0, MACH_MSGH_BITS_COMPLEX);
        msg.h.msgh_size = sizeof msg; msg.h.msgh_remote_port = tracer_port; msg.h.msgh_id = 1;
        msg.b.msgh_descriptor_count = 1;
        msg.d.name = t_mach_svc; msg.d.disposition = MACH_MSG_TYPE_COPY_SEND; msg.d.type = MACH_MSG_PORT_DESCRIPTOR;
        kern_return_t ks = mach_msg(&msg.h, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof msg, 0, MACH_PORT_NULL, 3000, MACH_PORT_NULL);
        hello.mach_ok = ks == KERN_SUCCESS;
        hello.mach_kr = ks;
    }
    pthread_t t;
    pthread_create(&t, NULL, svc_spin, NULL);
    pthread_create(&t, NULL, svc_futex, NULL);
    pthread_create(&t, NULL, svc_pthread, NULL);
    pthread_create(&t, NULL, svc_sem, NULL);
    pthread_create(&t, NULL, svc_pipe, NULL);
    pthread_create(&t, NULL, svc_sock, (void *)(intptr_t)fd_sock_s);
    pthread_create(&t, NULL, svc_sock, (void *)(intptr_t)fd_sock_d);
    if (hello.mach_ok) pthread_create(&t, NULL, svc_mach, NULL);
    hi_write_full(4, &hello, sizeof hello);
    char c;
    while (read(3, &c, 1) > 0) ;     /* tracer closes the control pipe when done */
    _exit(0);
}

/* ============================ tracer side ============================== */
static int t_pipe_req_w, t_pipe_rsp_r, t_sock_s, t_sock_d;
static mach_port_t t_reply_port, t_target_svc;
static uint8_t *g_buf;
static uint32_t g_seq;

static uint64_t guest_base;

static void bench_method(int m, size_t len, int n)
{
    uint64_t *v = malloc(sizeof(uint64_t) * n);
    struct mbuf *mb = (m == M_MACH) ? malloc(sizeof *mb) : NULL;
    int ok = 1;
    atomic_store(&g_sh->active, m);
    usleep(2000);
    for (int i = -50; i < n; i++) {     /* 50 warm-up requests */
        uint64_t off = ((uint64_t)(i + 50) * 4160) % (GUEST_SIZE - len - 64);
        uint64_t addr = guest_base + off;
        uint64_t t0 = dp_ns();
        switch (m) {
        case M_SPIN: {
            g_sh->req_addr = addr; g_sh->req_len = len;
            uint32_t s = ++g_seq;
            atomic_store_explicit(&g_sh->req_seq, s, memory_order_release);
            while (atomic_load_explicit(&g_sh->rsp_seq, memory_order_acquire) != s) __asm__ volatile("yield");
            memcpy(g_buf, g_data, len);
            break;
        }
        case M_FUTEX: {
            g_sh->req_addr = addr; g_sh->req_len = len;
            uint32_t s = ++g_seq;
            atomic_store_explicit(&g_sh->f_req, s, memory_order_release);
            hi_futex_wake((volatile uint32_t *)&g_sh->f_req);
            uint32_t cur;
            while ((cur = atomic_load_explicit(&g_sh->f_rsp, memory_order_acquire)) != s) hi_futex_wait((volatile uint32_t *)&g_sh->f_rsp, cur);
            memcpy(g_buf, g_data, len);
            break;
        }
        case M_PTHREAD: {
            pthread_mutex_lock(&g_sh->mu);
            g_sh->req_addr = addr; g_sh->req_len = len;
            uint32_t s = ++g_sh->pt_req;
            pthread_cond_signal(&g_sh->cv_req);
            while (g_sh->pt_rsp != s) pthread_cond_wait(&g_sh->cv_rsp, &g_sh->mu);
            pthread_mutex_unlock(&g_sh->mu);
            memcpy(g_buf, g_data, len);
            break;
        }
        case M_SEM:
            g_sh->req_addr = addr; g_sh->req_len = len;
            sem_post(sem_req);
            while (sem_wait(sem_rsp) != 0 && errno == EINTR) ;
            memcpy(g_buf, g_data, len);
            break;
        case M_PIPE_NOTIFY: {
            struct creq rq = {addr, len}; char x;
            hi_write_full(t_pipe_req_w, &rq, sizeof rq);
            hi_read_full(t_pipe_rsp_r, &x, 1);
            memcpy(g_buf, g_data, len);
            break;
        }
        case M_PIPE_INLINE: {
            struct creq rq = {addr, len | INLINE_FLAG};
            hi_write_full(t_pipe_req_w, &rq, sizeof rq);
            hi_read_full(t_pipe_rsp_r, g_buf, len);
            break;
        }
        case M_STREAM: {
            struct creq rq = {addr, len}; char x;
            send(t_sock_s, &rq, sizeof rq, 0);
            recv(t_sock_s, &x, 1, 0);
            memcpy(g_buf, g_data, len);
            break;
        }
        case M_DGRAM: {
            struct creq rq = {addr, len}; char x;
            send(t_sock_d, &rq, sizeof rq, 0);
            recv(t_sock_d, &x, 1, 0);
            memcpy(g_buf, g_data, len);
            break;
        }
        case M_MACH: {
            mb->h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE);
            mb->h.msgh_remote_port = t_target_svc; mb->h.msgh_local_port = t_reply_port; mb->h.msgh_id = 1;
            mb->h.msgh_size = sizeof(mach_msg_header_t) + 16;
            memcpy(mb->body, &addr, 8); memcpy(mb->body + 8, &len, 8);
            kern_return_t kr = mach_msg(&mb->h, MACH_SEND_MSG | MACH_RCV_MSG, mb->h.msgh_size, sizeof *mb, t_reply_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
            if (kr != MACH_MSG_SUCCESS) { ok = 0; }
            else memcpy(g_buf, mb->body, len);
            break;
        }
        }
        uint64_t d = dp_ns() - t0;
        if (i >= 0) v[i] = d;
        if (!ok) break;
    }
    if (ok) {
        /* correctness: the last request */
        uint64_t off = ((uint64_t)(n - 1 + 50) * 4160) % (GUEST_SIZE - len - 64);
        int good = 1;
        for (size_t k = 0; k < len; k += (len > 4096 ? 997 : 1)) if (g_buf[k] != (uint8_t)((off + k) * 131 + 7)) { good = 0; break; }
        if (!good) FAIL("H4.lat", "%s len=%zu: payload received is wrong", mname[m], len);
        char label[96];
        snprintf(label, sizeof label, "%-14s %7zu B", mname[m], len);
        hi_report_us("H4.lat", label, v, n);
    } else INFO("H4.lat", "%s len=%zu: transport error", mname[m], len);
    /* quit request on the same channel so the service thread re-arms */
    atomic_store(&g_sh->active, M_NONE);
    free(v); free(mb);
}

struct tfp_arg { int pid; mach_port_t task; kern_return_t kr; };
static void *tfp_worker(void *p) { struct tfp_arg *a = p; a->kr = task_for_pid(mach_task_self(), a->pid, &a->task); return NULL; }

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "coop-target")) return target_main(argv[2]);
    dp_watchdog(220);
    signal(SIGPIPE, SIG_IGN);
    snprintf(shname, sizeof shname, "/hi4.%d", (int)getpid());
    if (open_shared(shname, 1)) { FAIL("H4.setup", "shm_open/ftruncate/mmap of %s failed errno=%d (%s)", shname, errno, strerror(errno)); return dp_summary("h4_coop_channel"); }
    PASS("H4.setup", "POSIX shared memory %s created and mapped (%u KiB)", shname, SH_TOTAL / 1024);
    pthread_mutexattr_t ma; pthread_condattr_t ca;
    pthread_mutexattr_init(&ma); pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED);
    pthread_condattr_init(&ca); pthread_condattr_setpshared(&ca, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&g_sh->mu, &ma); pthread_cond_init(&g_sh->cv_req, &ca); pthread_cond_init(&g_sh->cv_rsp, &ca);
    atomic_store(&g_sh->active, M_NONE);
    snprintf(semname_r, sizeof semname_r, "hi4.%d.r", (int)getpid());
    snprintf(semname_s, sizeof semname_s, "hi4.%d.s", (int)getpid());
    char a[40], b[40];
    snprintf(a, sizeof a, "/%s", semname_r); snprintf(b, sizeof b, "/%s", semname_s);
    sem_unlink(a); sem_unlink(b);
    sem_req = sem_open(a, O_CREAT | O_EXCL, 0600, 0);
    sem_rsp = sem_open(b, O_CREAT | O_EXCL, 0600, 0);
    INFO("H4.setup", "sem_open named semaphores %s", (sem_req != SEM_FAILED && sem_rsp != SEM_FAILED) ? "created" : "FAILED");

    int p1[2], p2[2], ss[2], sd[2];
    pipe(p1); pipe(p2);
    socketpair(AF_UNIX, SOCK_STREAM, 0, ss);
    socketpair(AF_UNIX, SOCK_DGRAM, 0, sd);
    int big = 1 << 20;
    for (int i = 0; i < 2; i++) { setsockopt(ss[i], SOL_SOCKET, SO_SNDBUF, &big, sizeof big); setsockopt(ss[i], SOL_SOCKET, SO_RCVBUF, &big, sizeof big); }
    t_pipe_req_w = p1[1]; t_pipe_rsp_r = p2[0]; t_sock_s = ss[0]; t_sock_d = sd[0];

    /* a port the target can find after exec */
    mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &t_reply_port);
    mach_port_insert_right(mach_task_self(), t_reply_port, t_reply_port, MACH_MSG_TYPE_MAKE_SEND);
    /* the spawned target inherits our TASK_BOOTSTRAP_PORT slot: temporarily point it at our own port (mach_ports_register does not deliver, see h6) */
    mach_port_t saved_bootstrap = MACH_PORT_NULL;
    task_get_special_port(mach_task_self(), TASK_BOOTSTRAP_PORT, &saved_bootstrap);
    kern_return_t kreg = task_set_special_port(mach_task_self(), TASK_BOOTSTRAP_PORT, t_reply_port);
    INFO("H4.setup", "task_set_special_port(TASK_BOOTSTRAP_PORT, own port) for the spawn -> %s", hi_kr_str(kreg));

    int keep[4] = {p1[0], p2[1], ss[1], sd[1]};
    struct hi_child c;
    int spawn_rc = hi_spawn_role_fds(&c, "coop-target", shname, keep, 4);
    task_set_special_port(mach_task_self(), TASK_BOOTSTRAP_PORT, saved_bootstrap);
    if (spawn_rc) { FAIL("H4.setup", "spawn failed"); return dp_summary("h4_coop_channel"); }
    struct { uint64_t guest; uint32_t pid; int32_t sem_ok; int32_t mach_ok; int32_t mach_kr; } hello;
    if (hi_read_timeout(c.rfd, &hello, sizeof hello, 8000)) { FAIL("H4.setup", "target did not report"); kill(c.pid, SIGKILL); return dp_summary("h4_coop_channel"); }
    guest_base = hello.guest;
    INFO("H4.setup", "target pid=%u guest memory at 0x%llx ; target reached the tracer through the inherited bootstrap slot: %s (kr=%d) ; target opened the named semaphores: %d",
         hello.pid, (unsigned long long)hello.guest, hello.mach_ok ? "yes" : "no", hello.mach_kr, hello.sem_ok);
    int have_mach = 0;
    if (hello.mach_ok) {
        struct { mach_msg_header_t h; mach_msg_body_t b; mach_msg_port_descriptor_t d; mach_msg_trailer_t t; } in;
        memset(&in, 0, sizeof in);
        kern_return_t kr = mach_msg(&in.h, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof in, t_reply_port, 3000, MACH_PORT_NULL);
        INFO("H4.setup", "mach_msg receive of the target's hello on the registered port -> %s bits=0x%x size=%u id=%d ndesc=%u desc.name=0x%x", hi_kr_str(kr), in.h.msgh_bits, in.h.msgh_size, in.h.msgh_id, in.b.msgh_descriptor_count, in.d.name);
        if (kr == KERN_SUCCESS && (in.h.msgh_bits & MACH_MSGH_BITS_COMPLEX)) { t_target_svc = in.d.name; have_mach = 1; }
        INFO("H4.setup", "received the target's Mach service port: %s", have_mach ? "yes" : "no");
    }
    g_buf = malloc(2u << 20);

    /* task port for the Mach methods */
    struct tfp_arg ta = {.pid = (int)hello.pid};
    mach_port_t task = MACH_PORT_NULL;
    if (hi_call_with_deadline(tfp_worker, &ta, 5000, NULL) && ta.kr == 0) task = ta.task;
    else INFO("H4.setup", "task_for_pid unavailable (kr=%d): vm_read and vm_remap rows skipped", ta.kr);

    mach_vm_address_t remap_local = 0;
    if (task) {
        vm_prot_t cur, max;
        kern_return_t kr = mach_vm_remap(mach_task_self(), &remap_local, GUEST_SIZE, 0, VM_FLAGS_ANYWHERE, task, guest_base, FALSE, &cur, &max, VM_INHERIT_NONE);
        INFO("H4.setup", "mach_vm_remap of the target's private guest memory -> %s", hi_kr_str(kr));
        if (kr) remap_local = 0;
    }
    struct { size_t len; int n; } sizes[] = {{8, 20000}, {256, 20000}, {4096, 10000}, {65536, 2000}, {1u << 20, 300}};
    for (int pass = 0; pass < 2; pass++) {
        atomic_store(&g_sh->qos, pass);
        pthread_set_qos_class_self_np(pass ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_DEFAULT, 0);
        INFO("H4.lat", "=== pass %d: threads at QoS %s ===", pass, pass ? "user-interactive" : "default");
        /* no-request references */
        for (size_t si = 0; si < sizeof sizes / sizeof sizes[0]; si++) {
            size_t len = sizes[si].len; int n = sizes[si].n;
            uint64_t *v = malloc(sizeof(uint64_t) * n);
            for (int i = 0; i < n; i++) {
                uint64_t off = ((uint64_t)i * 4160) % (GUEST_SIZE - len - 64);
                uint64_t t0 = dp_ns();
                memcpy(g_buf, g_shared_guest + off, len);
                v[i] = dp_ns() - t0;
            }
            char label[96]; snprintf(label, sizeof label, "%-14s %7zu B", "direct-shm", len);
            hi_report_us("H4.lat", label, v, n);
            if (task && pass == 0) {
                for (int i = 0; i < n; i++) {
                    uint64_t off = ((uint64_t)i * 4160) % (GUEST_SIZE - len - 64);
                    mach_vm_size_t out;
                    uint64_t t0 = dp_ns();
                    mach_vm_read_overwrite(task, guest_base + off, len, (mach_vm_address_t)(uintptr_t)g_buf, &out);
                    v[i] = dp_ns() - t0;
                }
                snprintf(label, sizeof label, "%-14s %7zu B", "vm_read", len);
                hi_report_us("H4.lat", label, v, n);
                if (remap_local) {
                    uint8_t *rm = (uint8_t *)(uintptr_t)remap_local;
                    for (int i = 0; i < n; i++) {
                        uint64_t off = ((uint64_t)i * 4160) % (GUEST_SIZE - len - 64);
                        uint64_t t0 = dp_ns();
                        memcpy(g_buf, rm + off, len);
                        v[i] = dp_ns() - t0;
                    }
                    snprintf(label, sizeof label, "%-14s %7zu B", "vm_remap", len);
                    hi_report_us("H4.lat", label, v, n);
                }
            }
            free(v);
        }
        for (int m = 0; m < M_COUNT; m++) {
            if (m == M_MACH && !have_mach) { INFO("H4.lat", "mach-msg skipped: the target's service port never arrived"); continue; }
            if (m == M_SEM && !(sem_req != SEM_FAILED && hello.sem_ok)) continue;
            for (size_t si = 0; si < sizeof sizes / sizeof sizes[0]; si++) {
                size_t len = sizes[si].len; int n = sizes[si].n;
                if (m == M_MACH && len > 65536) continue;
                if (m == M_PIPE_INLINE && len > 65536) n = 100;
                bench_method(m, len, n);
            }
        }
    }
    close(c.wfd);
    int st; dp_wait_timeout(c.pid, &st, 2000);
    shm_unlink(shname); sem_unlink(a); sem_unlink(b);
    return dp_summary("h4_coop_channel");
}
