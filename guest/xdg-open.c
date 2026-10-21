/* xdg-open: an x86-64 Linux program for the engine's guest, which has no browser, installed as the root's
 * /usr/bin/xdg-open.
 *
 * It opens a web address in the Mac's default browser: it sends it to wsd (WSD_OPEN_URL), at WINE_OBELISK_SOCKET, the
 * socket Wine's display driver uses, else /run/obelisk/wsd.sock. Wine's winebrowser runs it for the links a game
 * opens; Rocket League opens Epic's sign-in page this way when its exchange code is refused, and waited on its title
 * screen without it.
 *
 * Exit status as xdg-utils' xdg-open: 1 bad usage, 3 no wsd to reach, 4 wsd refused the address (it opens http and
 * https only). */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "wsd_protocol.h"

static int send_all(int fd, const void *p, size_t n)
{
    const char *c = p;
    while (n) {
        ssize_t w = send(fd, c, n, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        c += w;
        n -= (size_t)w;
    }
    return 0;
}

static int recv_all(int fd, void *p, size_t n)
{
    char *c = p;
    while (n) {
        ssize_t r = recv(fd, c, n, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        c += r;
        n -= (size_t)r;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2 || !argv[1][0] || strlen(argv[1]) > WSD_MAX_PAYLOAD) {
        fprintf(stderr, "usage: xdg-open <http or https address>\n");
        return 1;
    }
    const char *path = getenv("WINE_OBELISK_SOCKET");
    if (!path) path = "/run/obelisk/wsd.sock";
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof(addr.sun_path)) return 3;
    strcpy(addr.sun_path, path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof(addr))) {
        fprintf(stderr, "xdg-open: no window server at %s: %s\n", path, strerror(errno));
        return 3;
    }

    /* wsd answers in order: an error for the address comes before the pong of the ping sent after it. */
    struct wsd_hdr open = { WSD_OPEN_URL, (uint32_t)strlen(argv[1]) };
    struct wsd_hdr ping_hdr = { WSD_PING, sizeof(struct wsd_ping) };
    struct wsd_ping ping = { 1, 0 };
    if (send_all(fd, &open, sizeof(open)) || send_all(fd, argv[1], open.len) ||
        send_all(fd, &ping_hdr, sizeof(ping_hdr)) || send_all(fd, &ping, sizeof(ping)))
        return 3;
    for (;;) {
        struct wsd_hdr hdr;
        char payload[WSD_MAX_PAYLOAD];
        if (recv_all(fd, &hdr, sizeof(hdr)) || hdr.len > sizeof(payload) || recv_all(fd, payload, hdr.len)) return 3;
        if (hdr.type == WSD_EV_ERROR && hdr.len >= sizeof(struct wsd_ev_error)) {
            struct wsd_ev_error error;
            memcpy(&error, payload, sizeof(error));
            if (error.request_type == WSD_OPEN_URL) {
                fprintf(stderr, "xdg-open: the Mac did not open the address (%d)\n", error.code);
                return 4;
            }
        }
        if (hdr.type == WSD_EV_PONG) return 0;
    }
}
