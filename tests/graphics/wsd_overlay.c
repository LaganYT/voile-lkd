/*
 * wsd_overlay.c - the overlay plane of a window (PRESENT_REGION, what a D3D swapchain presents to) is shown again after
 * the client removed it and presents to the same rectangle.
 *
 * A game that recreates its swapchain does exactly that: Neptune removes the plane (a 0x0 region) when the swapchain
 * goes away, and the new swapchain presents to the same full-window rectangle. On 2026-10-10 wsd kept the old
 * rectangle, so the new plane's layer was never placed: it stayed 0x0, no frame of ELDEN RING's second swapchain was
 * displayed (black window), and Neptune's pacing waited 100 ms for each frame (10 fps).
 *
 * Opens a 640x360 window for about a second. Counts the frames whose presented time is not zero, for the first plane
 * and for the plane created again. Ends with "RESULT PASS|FAIL".
 *
 * Build: clang -O2 -Wall -o wsd_overlay wsd_overlay.c
 * Run:   wsd --dir <private 0700 dir> --exit-on-disconnect & wsd_overlay <dir>/wsd.sock   (under run-with-timeout.sh)
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "../../wsd/wsd_protocol.h"

#define WIN 1u
#define NBUF 3
static const uint32_t W = 640, H = 360;
static int g_sock = -1;
static int g_busy[NBUF];
static int g_shown, g_zero, g_errors;

static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

static void die(const char *m)
{
    printf("FATAL: %s (errno %d)\nRESULT FAIL\n", m, errno);
    exit(1);
}

static void send_msg(uint32_t type, const void *p, uint32_t n, int fd)
{
    uint8_t buf[sizeof(struct wsd_hdr) + WSD_MAX_PAYLOAD];
    struct wsd_hdr h = {type, n};
    memcpy(buf, &h, sizeof h);
    if (n) memcpy(buf + sizeof h, p, n);
    struct iovec iov = {buf, sizeof h + n};
    struct msghdr m = {0};
    char ctl[CMSG_SPACE(sizeof(int))];
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    if (fd >= 0) {
        m.msg_control = ctl;
        m.msg_controllen = sizeof ctl;
        struct cmsghdr *c = CMSG_FIRSTHDR(&m);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &fd, sizeof(int));
    }
    if (sendmsg(g_sock, &m, 0) != (ssize_t)(sizeof h + n)) die("sendmsg");
}

static uint8_t g_in[1 << 16];
static size_t g_inlen;

/* Handles the events that arrive within timeout_ms. */
static void pump(int timeout_ms)
{
    uint64_t end = now_ns() + (uint64_t)timeout_ms * 1000000ull;
    for (;;) {
        while (g_inlen >= sizeof(struct wsd_hdr)) {
            struct wsd_hdr h;
            memcpy(&h, g_in, sizeof h);
            if (h.len > WSD_MAX_PAYLOAD) die("oversized event");
            if (g_inlen < sizeof h + h.len) break;
            const uint8_t *d = g_in + sizeof h;
            if (h.type == WSD_EV_RELEASE) {
                const struct wsd_ev_release *r = (const void *)d;
                if (r->buffer_id >= 1 && r->buffer_id <= NBUF) g_busy[r->buffer_id - 1] = 0;
            } else if (h.type == WSD_EV_PRESENTED) {
                const struct wsd_ev_presented *p = (const void *)d;
                if (p->presented_ns) g_shown++;
                else g_zero++;
            } else if (h.type == WSD_EV_ERROR) {
                const struct wsd_ev_error *e = (const void *)d;
                printf("server error: request %u code %d\n", e->request_type, e->code);
                g_errors++;
            }
            memmove(g_in, g_in + sizeof h + h.len, g_inlen - sizeof h - h.len);
            g_inlen -= sizeof h + h.len;
        }
        uint64_t t = now_ns();
        if (t >= end) return;
        struct pollfd p = {g_sock, POLLIN, 0};
        if (poll(&p, 1, (int)((end - t) / 1000000) + 1) <= 0) continue;
        ssize_t n = read(g_sock, g_in + g_inlen, sizeof g_in - g_inlen);
        if (n <= 0) die("server closed the connection");
        g_inlen += (size_t)n;
    }
}

/* Presents frames to the overlay plane at the full window rectangle; returns how many were displayed. */
static int plane_round(uint64_t *serial, int frames)
{
    g_shown = g_zero = 0;
    for (int f = 0; f < frames; f++) {
        int b = -1;
        for (int tries = 0; b < 0 && tries < 50; tries++) {
            for (int i = 0; i < NBUF; i++) if (!g_busy[i]) { b = i; break; }
            if (b < 0) pump(5);
        }
        if (b < 0) break;
        g_busy[b] = 1;
        struct wsd_present_region r = {{WIN, (uint32_t)b + 1, ++*serial, now_ns()}, 0, 0, W, H};
        send_msg(WSD_PRESENT_REGION, &r, sizeof r, -1);
        pump(17);
    }
    pump(400);
    return g_shown;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <socket>\n", argv[0]); return 2; }
    g_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    if (strlen(argv[1]) >= sizeof sa.sun_path) die("socket path too long");
    strcpy(sa.sun_path, argv[1]);
    if (connect(g_sock, (struct sockaddr *)&sa, sizeof sa) != 0) die("connect");

    struct wsd_hello hello = {WSD_PROTOCOL_VERSION, (uint32_t)getpid()};
    send_msg(WSD_HELLO, &hello, sizeof hello, -1);
    struct wsd_window_create wc = {WIN, W, H, 0};
    send_msg(WSD_WINDOW_CREATE, &wc, sizeof wc, -1);
    struct wsd_window_flag wsh = {WIN, 1};
    send_msg(WSD_WINDOW_SHOW, &wsh, sizeof wsh, -1);
    for (uint32_t i = 0; i < NBUF; i++) {
        char name[64];
        snprintf(name, sizeof name, "/wsdov.%d.%u", (int)getpid(), i);
        int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd < 0) die("shm_open");
        shm_unlink(name);
        size_t len = (size_t)W * H * 4;
        if (ftruncate(fd, (off_t)len) != 0) die("ftruncate");
        uint32_t *px = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (px == MAP_FAILED) die("mmap");
        for (size_t k = 0; k < (size_t)W * H; k++) px[k] = 0xFF2060C0u + i * 0x200000u;
        struct wsd_buffer_attach ba = {i + 1, W, H, W * 4, WSD_FMT_BGRA8, 0, 0, (uint64_t)len};
        send_msg(WSD_BUFFER_ATTACH, &ba, sizeof ba, fd);
        close(fd);
    }
    pump(500); /* the window appears */

    uint64_t serial = 0;
    int first = plane_round(&serial, 30);
    struct wsd_present_region off = {{WIN, 0, ++serial, now_ns()}, 0, 0, 0, 0};
    send_msg(WSD_PRESENT_REGION, &off, sizeof off, -1);
    pump(100);
    for (int i = 0; i < NBUF; i++) g_busy[i] = 0; /* the removed plane released its buffers */
    int again = plane_round(&serial, 30);
    send_msg(WSD_PRESENT_REGION, &off, sizeof off, -1);
    pump(50);

    printf("overlay: first plane %d frames shown, plane created again %d frames shown (%d not shown), %d errors\n", first,
           again, g_zero, g_errors);
    int ok = first > 0 && again > 0 && g_errors == 0;
    printf("RESULT %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
