/*
 * h6_port_inherit.c - H: how can a Mach port reach a child process without launchd (no bootstrap registration)?
 * This decides how a tracer and a tracee can exchange a task port, thread ports or a message port cooperatively.
 *
 *   H6.registered   mach_ports_register in the parent, then fork only / fork+exec: what does the child look up?
 *   H6.special      posix_spawnattr_setspecialport_np with an own port in a special-port slot
 *   H6.bootstrap    bootstrap_register / bootstrap_check_in from an ad-hoc signed tool
 *   H6.handoff      the child sends mach_task_self() to the parent through such a port: is the task port movable,
 *                   and does mach_vm_read work on it, for a child WITH and WITHOUT get-task-allow
 *   H6.inherit      are task-level exception ports inherited by fork and exec
 * No task_for_pid is called anywhere in this program: ports only travel by explicit message.
 */
#include "hi_util.h"
#include <bootstrap.h>
#include <spawn.h>
#include <servers/bootstrap.h>

extern char **environ;

static const char *mode_names[] = {"fork-only", "fork+exec"};

static int send_port_msg(mach_port_t dest, mach_port_t port, int disposition, mach_msg_id_t id, kern_return_t *kr_out)
{
    struct { mach_msg_header_t h; mach_msg_body_t b; mach_msg_port_descriptor_t d; } msg;
    memset(&msg, 0, sizeof msg);
    msg.h.msgh_bits = MACH_MSGH_BITS_SET(MACH_MSG_TYPE_COPY_SEND, 0, 0, MACH_MSGH_BITS_COMPLEX);
    msg.h.msgh_size = sizeof msg; msg.h.msgh_remote_port = dest; msg.h.msgh_id = id;
    msg.b.msgh_descriptor_count = 1;
    msg.d.name = port; msg.d.disposition = disposition; msg.d.type = MACH_MSG_PORT_DESCRIPTOR;
    kern_return_t kr = mach_msg(&msg.h, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof msg, 0, MACH_PORT_NULL, 2000, MACH_PORT_NULL);
    if (kr_out) *kr_out = kr;
    return kr == KERN_SUCCESS;
}

static mach_port_t recv_port_msg(mach_port_t rcv, int ms, kern_return_t *kr_out, mach_msg_id_t *id_out)
{
    struct { mach_msg_header_t h; mach_msg_body_t b; mach_msg_port_descriptor_t d; mach_msg_trailer_t t; char pad[64]; } in;
    memset(&in, 0, sizeof in);
    kern_return_t kr = mach_msg(&in.h, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof in, rcv, ms, MACH_PORT_NULL);
    if (kr_out) *kr_out = kr;
    if (kr) return MACH_PORT_NULL;
    if (id_out) *id_out = in.h.msgh_id;
    return (in.h.msgh_bits & MACH_MSGH_BITS_COMPLEX) ? in.d.name : MACH_PORT_NULL;
}

/* ---- the child ------------------------------------------------------------ */
static int child_main(const char *how)
{
    dp_role = "child";
    mach_port_t dest = MACH_PORT_NULL;
    char b[160];
    if (!strcmp(how, "registered")) {
        mach_port_array_t ports = NULL; mach_msg_type_number_t n = 0;
        kern_return_t kr = mach_ports_lookup(mach_task_self(), &ports, &n);
        INFO("H6.registered", "child mach_ports_lookup -> %s n=%u ports: %x %x %x", hi_kr_str(kr), n, n > 0 ? ports[0] : 0, n > 1 ? ports[1] : 0, n > 2 ? ports[2] : 0);
        if (kr == 0 && n > 0) {
            dest = ports[0];
            mach_port_type_t ty = 0;
            kern_return_t k2 = mach_port_type(mach_task_self(), dest, &ty);
            INFO("H6.registered", "child port 0x%x type=0x%x (SEND=0x%x DEAD_NAME=0x%x) kr=%d", dest, ty, MACH_PORT_TYPE_SEND, MACH_PORT_TYPE_DEAD_NAME, k2);
        }
    } else if (!strcmp(how, "special") || !strcmp(how, "forkspecial")) {
        /* the parent put a send right to its own port in the child's TASK_BOOTSTRAP_PORT slot */
        kern_return_t kr = task_get_special_port(mach_task_self(), TASK_BOOTSTRAP_PORT, &dest);
        INFO("H6.special", "child task_get_special_port(TASK_BOOTSTRAP_PORT) -> %s port=0x%x", hi_kr_str(kr), dest);
    } else if (!strcmp(how, "bootstrap")) {
        kern_return_t kr = bootstrap_look_up(bootstrap_port, how, &dest);
        INFO("H6.bootstrap", "child bootstrap_look_up -> %s", hi_kr_str(kr));
    }
    if (dest == MACH_PORT_NULL) { INFO("H6.handoff", "child has no port to talk to"); return 0; }
    kern_return_t ks = 0;
    int ok = send_port_msg(dest, mach_task_self(), MACH_MSG_TYPE_COPY_SEND, 0x4849, &ks);
    INFO("H6.handoff", "child sends mach_task_self() as COPY_SEND -> %s", hi_kr_str(ks));
    (void)ok; (void)b;
    /* stay alive until the parent is done reading our memory */
    char c; read(3, &c, 1);
    return 0;
}

/* ---- the parent ----------------------------------------------------------- */
static volatile int g_marker = 0x13572468;

static void handoff_test(const char *id, const char *how, int do_exec, int with_entitlement)
{
    mach_port_t port;
    mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port);
    mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND);
    pid_t pid = -1;
    int pfd[2];
    pipe(pfd);
    char self[4096]; snprintf(self, sizeof self, "%s%s", dp_self_path(), with_entitlement ? "" : ".noent");

    if (!strcmp(how, "registered") || !strcmp(how, "forkspecial")) {
        mach_port_t saved = MACH_PORT_NULL;
        if (!strcmp(how, "registered")) {
            mach_port_t reg[1] = {port};
            kern_return_t kr = mach_ports_register(mach_task_self(), reg, 1);
            INFO(id, "mach_ports_register -> %s", hi_kr_str(kr));
        } else {
            task_get_special_port(mach_task_self(), TASK_BOOTSTRAP_PORT, &saved);
            kern_return_t kr = task_set_special_port(mach_task_self(), TASK_BOOTSTRAP_PORT, port);
            INFO(id, "parent task_set_special_port(TASK_BOOTSTRAP_PORT, own port) -> %s (restored right after the fork)", hi_kr_str(kr));
        }
        pid = fork();
        if (pid == 0) {
            dup2(pfd[0], 3);
            if (do_exec) {
                for (int fd = 4; fd < 64; fd++) close(fd);
                execl(self, self, "child", how, (char *)NULL);
                _exit(127);
            }
            _exit(child_main(how));   /* fork-only: same image, ports inherited */
        }
        if (saved != MACH_PORT_NULL) task_set_special_port(mach_task_self(), TASK_BOOTSTRAP_PORT, saved);
    } else {
        posix_spawnattr_t attr; posix_spawnattr_init(&attr);
        posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, pfd[0], 3);
        kern_return_t kr = posix_spawnattr_setspecialport_np(&attr, port, TASK_BOOTSTRAP_PORT);
        INFO(id, "posix_spawnattr_setspecialport_np(TASK_BOOTSTRAP_PORT) -> %d", kr);
        char *argv2[] = {self, "child", (char *)how, NULL};
        int rc = posix_spawn(&pid, self, &fa, &attr, argv2, environ);
        if (rc) { INFO(id, "posix_spawn failed %d", rc); return; }
    }
    dp_track(pid);
    kern_return_t kr = 0; mach_msg_id_t mid = 0;
    mach_port_t task = recv_port_msg(port, 3000, &kr, &mid);
    INFO(id, "%s / %s / %s: parent receive -> %s, got port 0x%x", how, do_exec ? "exec" : "fork-only", with_entitlement ? "get-task-allow" : "no entitlement", hi_kr_str(kr), task);
    if (task != MACH_PORT_NULL) {
        mach_port_type_t ty = 0; mach_port_type(mach_task_self(), task, &ty);
        pid_t tp = -1; kern_return_t kp = pid_for_task(task, &tp);
        INFO(id, "received port type=0x%x pid_for_task -> %s pid=%d (child is %d)", ty, hi_kr_str(kp), (int)tp, (int)pid);
        if (kp == 0 && tp == pid) {
            vm_offset_t data; mach_msg_type_number_t cnt;
            /* read the child's copy of g_marker: same image address layout is not guaranteed, so read the dyld info instead */
            task_dyld_info_data_t di; mach_msg_type_number_t dc = TASK_DYLD_INFO_COUNT;
            kern_return_t kd = task_info(task, TASK_DYLD_INFO, (task_info_t)&di, &dc);
            kern_return_t km = KERN_FAILURE;
            if (kd == 0) km = mach_vm_read(task, di.all_image_info_addr, 32, &data, &cnt);
            INFO(id, "mach_vm_read through the RECEIVED task port -> %s (task_info %s)", hi_kr_str(km), hi_kr_str(kd));
            CHECK(km == 0, id, "a task port handed over by the child itself is usable for memory access (%s)", with_entitlement ? "child has get-task-allow" : "child has NO get-task-allow");
            if (km == 0) mach_vm_deallocate(mach_task_self(), data, cnt);
            thread_act_array_t th; mach_msg_type_number_t nt = 0;
            kern_return_t kt = task_threads(task, &th, &nt);
            INFO(id, "task_threads through the received port -> %s (%u threads)", hi_kr_str(kt), kt ? 0 : nt);
            if (kt == 0 && nt > 0) {
                arm_thread_state64_t st; mach_msg_type_number_t sc = ARM_THREAD_STATE64_COUNT;
                kern_return_t kg = thread_get_state(th[0], ARM_THREAD_STATE64, (thread_state_t)&st, &sc);
                INFO(id, "thread_get_state through the received port -> %s", hi_kr_str(kg));
            }
        }
    }
    write(pfd[1], "x", 1);
    int st; dp_wait_timeout(pid, &st, 2000);
    close(pfd[0]); close(pfd[1]);
    mach_port_destruct(mach_task_self(), port, 0, 0);   /* replaces the deprecated mach_port_destroy */
    mach_ports_register(mach_task_self(), NULL, 0);
}

/* task-level exception ports and inheritance */
static void inherit_test(void)
{
    mach_port_t port;
    mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port);
    mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND);
    kern_return_t kr = task_set_exception_ports(mach_task_self(), EXC_MASK_BAD_INSTRUCTION, port, EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES, THREAD_STATE_NONE);
    INFO("H6.inherit", "parent task_set_exception_ports(EXC_MASK_BAD_INSTRUCTION) -> %s", hi_kr_str(kr));
    for (int do_exec = 0; do_exec < 2; do_exec++) {
        pid_t pid = fork();
        if (pid == 0) {
            if (do_exec) { execl(dp_self_path(), dp_self_path(), "excprobe", (char *)NULL); _exit(127); }
            exception_mask_t masks[EXC_TYPES_COUNT]; mach_msg_type_number_t cnt = EXC_TYPES_COUNT;
            mach_port_t ports[EXC_TYPES_COUNT]; exception_behavior_t beh[EXC_TYPES_COUNT]; thread_state_flavor_t fl[EXC_TYPES_COUNT];
            kern_return_t k = task_get_exception_ports(mach_task_self(), EXC_MASK_BAD_INSTRUCTION, masks, &cnt, ports, beh, fl);
            dp_role = "child";
            INFO("H6.inherit", "forked child (no exec) sees task exception port for BAD_INSTRUCTION: %s (n=%u port=0x%x)", k ? "error" : (cnt && ports[0] != MACH_PORT_NULL ? "inherited" : "not inherited"), cnt, cnt ? ports[0] : 0);
            _exit(0);
        }
        dp_track(pid);
        int st; dp_wait_timeout(pid, &st, 3000);
    }
    task_set_exception_ports(mach_task_self(), EXC_MASK_BAD_INSTRUCTION, MACH_PORT_NULL, EXCEPTION_DEFAULT, THREAD_STATE_NONE);
}

static int excprobe_main(void)
{
    dp_role = "child-exec";
    exception_mask_t masks[EXC_TYPES_COUNT]; mach_msg_type_number_t cnt = EXC_TYPES_COUNT;
    mach_port_t ports[EXC_TYPES_COUNT]; exception_behavior_t beh[EXC_TYPES_COUNT]; thread_state_flavor_t fl[EXC_TYPES_COUNT];
    kern_return_t k = task_get_exception_ports(mach_task_self(), EXC_MASK_BAD_INSTRUCTION, masks, &cnt, ports, beh, fl);
    INFO("H6.inherit", "forked+exec'd child sees task exception port for BAD_INSTRUCTION: %s (n=%u port=0x%x)", k ? "error" : (cnt && ports[0] != MACH_PORT_NULL ? "inherited" : "not inherited"), cnt, cnt ? ports[0] : 0);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "child")) return child_main(argv[2]);
    if (argc > 1 && !strcmp(argv[1], "excprobe")) return excprobe_main();
    dp_watchdog(90);
    signal(SIGPIPE, SIG_IGN);
    (void)mode_names; (void)g_marker;
    handoff_test("H6.registered", "registered", 0, 1);
    handoff_test("H6.registered", "registered", 1, 1);
    for (int ent = 1; ent >= 0; ent--) {
        handoff_test("H6.special", "special", 1, ent);
    }
    handoff_test("H6.special", "forkspecial", 0, 1);
    handoff_test("H6.special", "forkspecial", 1, 1);
    /* bootstrap registration from an ad-hoc tool */
    {
        mach_port_t port, got = MACH_PORT_NULL;
        mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port);
        mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND);
        char name[64]; snprintf(name, sizeof name, "com.zagrios.hi6.%d", (int)getpid());
        /* bootstrap_register is deprecated since macOS 10.5 and has no public replacement (bootstrap_register2 is
         * private, bootstrap_check_in needs a launchd plist). The call itself is the thing under test. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        kern_return_t kr = bootstrap_register(bootstrap_port, name, port);
#pragma clang diagnostic pop
        INFO("H6.bootstrap", "bootstrap_register(%s) from an ad-hoc signed tool -> %s", name, hi_kr_str(kr));
        mach_port_t svc = MACH_PORT_NULL;
        kern_return_t kc = bootstrap_check_in(bootstrap_port, name, &svc);
        INFO("H6.bootstrap", "bootstrap_check_in(%s) -> %s", name, hi_kr_str(kc));
        kern_return_t kl = bootstrap_look_up(bootstrap_port, name, &got);
        INFO("H6.bootstrap", "bootstrap_look_up(%s) -> %s", name, hi_kr_str(kl));
    }
    inherit_test();
    return dp_summary("h6_port_inherit");
}
