/*
 * hi_exc.h - a Mach exception server thread built on the MIG server generated from
 * mach_exc.defs (see gen/, produced with: mig -arch arm64 -server gen/mach_excServer.c
 * -header gen/mach_exc.h -sheader gen/mach_excServer.h <SDK>/usr/include/mach/mach_exc.defs).
 *
 * The three behaviours are served: EXCEPTION_DEFAULT (no state in the message, the hook uses
 * thread_get_state / thread_set_state on the thread port), EXCEPTION_STATE (state in the message,
 * no ports) and EXCEPTION_STATE_IDENTITY (state and ports).  A hook sees the thread state as an
 * arm_thread_state64_t it may edit in place for the two state behaviours.
 *
 * Include this header at most once per program (it includes the generated server source).
 */
#ifndef HI_EXC_H
#define HI_EXC_H

#include "hi_util.h"

#define HI_EXC_DEFAULT 1
#define HI_EXC_STATE 2
#define HI_EXC_STATE_IDENTITY 3

struct hi_exc_event {
    int behavior;                 /* HI_EXC_* */
    exception_type_t exception;
    int64_t code[4];
    int ncode;
    mach_port_t thread, task;     /* MACH_PORT_NULL for HI_EXC_STATE */
    arm_thread_state64_t *ts;     /* editable in place for the state behaviours, NULL for DEFAULT */
};

/* Return KERN_SUCCESS to resume the thread (with the edited state), KERN_FAILURE to pass the exception on. */
typedef kern_return_t (*hi_exc_hook_t)(struct hi_exc_event *ev, void *ctx);

static hi_exc_hook_t g_hi_exc_hook;
static void *g_hi_exc_ctx;
static volatile uint64_t g_hi_exc_count[4];   /* indexed by HI_EXC_* */

static kern_return_t hi_exc_dispatch(struct hi_exc_event *ev)
{
    g_hi_exc_count[ev->behavior]++;
    if (!g_hi_exc_hook) return KERN_FAILURE;
    return g_hi_exc_hook(ev, g_hi_exc_ctx);
}

#include "gen/mach_excServer.c"

kern_return_t catch_mach_exception_raise(mach_port_t exception_port, mach_port_t thread, mach_port_t task,
                                         exception_type_t exception, mach_exception_data_t code, mach_msg_type_number_t codeCnt)
{
    (void)exception_port;
    struct hi_exc_event ev;
    memset(&ev, 0, sizeof ev);
    ev.behavior = HI_EXC_DEFAULT; ev.exception = exception; ev.thread = thread; ev.task = task;
    ev.ncode = codeCnt > 4 ? 4 : (int)codeCnt;
    for (int i = 0; i < ev.ncode; i++) ev.code[i] = code[i];
    kern_return_t kr = hi_exc_dispatch(&ev);
    mach_port_deallocate(mach_task_self(), thread);
    mach_port_deallocate(mach_task_self(), task);
    return kr;
}

kern_return_t catch_mach_exception_raise_state(mach_port_t exception_port, exception_type_t exception, const mach_exception_data_t code,
                                               mach_msg_type_number_t codeCnt, int *flavor, const thread_state_t old_state,
                                               mach_msg_type_number_t old_stateCnt, thread_state_t new_state, mach_msg_type_number_t *new_stateCnt)
{
    (void)exception_port;
    if (*flavor != ARM_THREAD_STATE64 || old_stateCnt != ARM_THREAD_STATE64_COUNT) return KERN_FAILURE;
    memcpy(new_state, old_state, old_stateCnt * sizeof(natural_t));
    *new_stateCnt = old_stateCnt;
    struct hi_exc_event ev;
    memset(&ev, 0, sizeof ev);
    ev.behavior = HI_EXC_STATE; ev.exception = exception;
    ev.ncode = codeCnt > 4 ? 4 : (int)codeCnt;
    for (int i = 0; i < ev.ncode; i++) ev.code[i] = code[i];
    ev.ts = (arm_thread_state64_t *)new_state;
    return hi_exc_dispatch(&ev);
}

kern_return_t catch_mach_exception_raise_state_identity(mach_port_t exception_port, mach_port_t thread, mach_port_t task,
                                                        exception_type_t exception, mach_exception_data_t code, mach_msg_type_number_t codeCnt,
                                                        int *flavor, thread_state_t old_state, mach_msg_type_number_t old_stateCnt,
                                                        thread_state_t new_state, mach_msg_type_number_t *new_stateCnt)
{
    (void)exception_port;
    kern_return_t kr = KERN_FAILURE;
    if (*flavor == ARM_THREAD_STATE64 && old_stateCnt == ARM_THREAD_STATE64_COUNT) {
        memcpy(new_state, old_state, old_stateCnt * sizeof(natural_t));
        *new_stateCnt = old_stateCnt;
        struct hi_exc_event ev;
        memset(&ev, 0, sizeof ev);
        ev.behavior = HI_EXC_STATE_IDENTITY; ev.exception = exception; ev.thread = thread; ev.task = task;
        ev.ncode = codeCnt > 4 ? 4 : (int)codeCnt;
        for (int i = 0; i < ev.ncode; i++) ev.code[i] = code[i];
        ev.ts = (arm_thread_state64_t *)new_state;
        kr = hi_exc_dispatch(&ev);
    }
    mach_port_deallocate(mach_task_self(), thread);
    mach_port_deallocate(mach_task_self(), task);
    return kr;
}

static void *hi_exc_thread(void *arg)
{
    mach_port_t port = (mach_port_t)(uintptr_t)arg;
    pthread_setname_np("hi-exc-server");
    hi_qos_user_interactive();
    struct hi_exc_buf { mach_msg_header_t h; char body[16384]; };
    struct hi_exc_buf *req = (struct hi_exc_buf *)malloc(sizeof *req), *rep = (struct hi_exc_buf *)malloc(sizeof *rep);
    for (;;) {
        memset(&req->h, 0, sizeof req->h);
        kern_return_t kr = mach_msg(&req->h, MACH_RCV_MSG | MACH_RCV_LARGE, 0, sizeof *req, port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
        if (kr == MACH_RCV_PORT_DIED || kr == MACH_RCV_INVALID_NAME) break;
        if (kr != MACH_MSG_SUCCESS) continue;
        if (mach_exc_server(&req->h, &rep->h)) {
            kr = mach_msg(&rep->h, MACH_SEND_MSG, rep->h.msgh_size, 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
            (void)kr;
        }
    }
    return NULL;
}

/* Creates the receive port (with a send right) and a server thread. Returns the port name. */
static HI_UNUSED mach_port_t hi_exc_start(hi_exc_hook_t hook, void *ctx)
{
    g_hi_exc_hook = hook;
    g_hi_exc_ctx = ctx;
    mach_port_t port = MACH_PORT_NULL;
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port) != KERN_SUCCESS) return MACH_PORT_NULL;
    mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND);
    pthread_t th;
    pthread_attr_t at; pthread_attr_init(&at);
    pthread_attr_set_qos_class_np(&at, QOS_CLASS_USER_INTERACTIVE, 0);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_create(&th, &at, hi_exc_thread, (void *)(uintptr_t)port);
    return port;
}

#endif
