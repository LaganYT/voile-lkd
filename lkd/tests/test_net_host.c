/* test_net_host.c: the guest talks to native macOS processes over AF_UNIX (graphics needs G4: the window
 * server wsd and the vtest renderer). A host thread plays the native peer with raw host calls. Checked:
 *   1. a guest connect() to a path under a host directory bound in the vfs reaches the real host socket,
 *      and getpeereid on the host side sees the runtime's uid;
 *   2. SCM_RIGHTS both ways: a host POSIX shm descriptor becomes a guest descriptor that mm maps
 *      MAP_SHARED at offset 0 (16 KiB multiple), and a guest pipe end becomes a usable host descriptor;
 *   3. poll and epoll work on that socket and on a received pipe end (the fence descriptors);
 *   4. writes through the guest mapping and through the host mapping of the same shm object are coherent.
 * Linux references: unix(7), cmsg(3), mmap(2) MAP_SHARED, shm_open(3) semantics measured on macOS
 * (reports/darwin-process-facts-a.md F5, F6). */
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include "test_fs_util.h"
#include "sig.h"
#include "mm.h"

static char ROOT[PATH_MAX], HOSTDIR[PATH_MAX];
static lkd_task *T;

struct hostside {
    int lfd;                 /* host listening socket */
    int conn;                /* accepted socket */
    uid_t peer_uid;
    gid_t peer_gid;
    int shm_fd;              /* shm object the host created and sent */
    uint8_t *shm_map;        /* host mapping */
    int fence_wr;            /* write end of the fence pipe sent as read end */
    int guest_pipe_fd;       /* descriptor received from the guest */
    char got[16];
    int status;
};

#define SHM_SIZE 65536

static void *host_server(void *arg)
{
    struct hostside *h = arg;
    h->conn = accept(h->lfd, NULL, NULL);
    if (h->conn < 0) { h->status = 1; return NULL; }
    if (getpeereid(h->conn, &h->peer_uid, &h->peer_gid) < 0) { h->status = 2; return NULL; }
    /* send a shm object and a fence pipe read end in one message, like wsd/vtest replies */
    char nm[32];
    snprintf(nm, sizeof nm, "/lkdnh.%d", (int)getpid());
    h->shm_fd = shm_open(nm, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (h->shm_fd < 0) { h->status = 3; return NULL; }
    shm_unlink(nm);
    if (ftruncate(h->shm_fd, SHM_SIZE) < 0) { h->status = 4; return NULL; }
    h->shm_map = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, h->shm_fd, 0);
    if (h->shm_map == MAP_FAILED) { h->status = 5; return NULL; }
    memset(h->shm_map, 0, SHM_SIZE);
    memcpy(h->shm_map, "from-host", 10);
    int fence[2];
    if (pipe(fence) < 0) { h->status = 6; return NULL; }
    h->fence_wr = fence[1];
    int fds[2] = { h->shm_fd, fence[0] };
    char ctl[CMSG_SPACE(sizeof fds)];
    memset(ctl, 0, sizeof ctl);
    struct iovec iov = { "hdr", 3 };
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = ctl, .msg_controllen = sizeof ctl };
    struct cmsghdr *c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof fds);
    memcpy(CMSG_DATA(c), fds, sizeof fds);
    if (sendmsg(h->conn, &m, 0) != 3) { h->status = 7; return NULL; }
    close(fence[0]);          /* only the guest holds the read end now */
    /* wait for the guest's reply carrying a pipe write end */
    char buf[16] = { 0 };
    char rctl[CMSG_SPACE(sizeof(int) * 4)];
    struct iovec riov = { buf, sizeof buf };
    struct msghdr rm = { .msg_iov = &riov, .msg_iovlen = 1, .msg_control = rctl, .msg_controllen = sizeof rctl };
    ssize_t n = recvmsg(h->conn, &rm, 0);
    if (n <= 0) { h->status = 8; return NULL; }
    memcpy(h->got, buf, (size_t)n);
    h->guest_pipe_fd = -1;
    for (struct cmsghdr *rc = CMSG_FIRSTHDR(&rm); rc; rc = CMSG_NXTHDR(&rm, rc))
        if (rc->cmsg_level == SOL_SOCKET && rc->cmsg_type == SCM_RIGHTS) memcpy(&h->guest_pipe_fd, CMSG_DATA(rc), sizeof(int));
    if (h->guest_pipe_fd >= 0) (void)!write(h->guest_pipe_fd, "host-wrote", 10);
    h->status = 0;
    return NULL;
}

static int64_t send_one_fd(int sock, int gfd, const char *payload)
{
    struct { struct lx_cmsghdr h; int32_t fd[1]; } ctl;
    memset(&ctl, 0, sizeof ctl);
    ctl.h.cmsg_len = LX_CMSG_LEN(4);
    ctl.h.cmsg_level = LX_SOL_SOCKET;
    ctl.h.cmsg_type = LX_SCM_RIGHTS;
    ctl.fd[0] = gfd;
    struct lx_iovec iov = { P(payload), strlen(payload) };
    struct lx_msghdr m = { .msg_iov = P(&iov), .msg_iovlen = 1, .msg_control = P(&ctl), .msg_controllen = LX_CMSG_SPACE(4) };
    return SC(T, sendmsg, sock, P(&m), 0);
}

LT_TEST(guest_reaches_host_socket_and_exchanges_fds)
{
    struct hostside h;
    memset(&h, 0, sizeof h);
    h.lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s/wsd.sock", HOSTDIR);
    sa.sun_len = (uint8_t)SUN_LEN(&sa);
    LT_EQ(bind(h.lfd, (struct sockaddr *)&sa, sa.sun_len), 0);
    chmod(sa.sun_path, 0600);
    LT_EQ(listen(h.lfd, 4), 0);
    pthread_t pt;
    pthread_create(&pt, NULL, host_server, &h);

    /* guest side */
    int c = (int)SC(T, socket, LX_AF_UNIX, LX_SOCK_STREAM | LX_SOCK_CLOEXEC, 0);
    struct lx_sockaddr_un ga = { .sun_family = LX_AF_UNIX };
    strcpy(ga.sun_path, "/run/wsd/wsd.sock");
    LT_EQ(SC(T, connect, c, P(&ga), 2 + 18), 0);

    /* receive the shm object and the fence pipe end */
    struct { struct lx_cmsghdr h; int32_t fd[4]; } ctl;
    memset(&ctl, 0, sizeof ctl);
    char hdr[8] = { 0 };
    struct lx_iovec iov = { P(hdr), sizeof hdr };
    struct lx_msghdr m = { .msg_iov = P(&iov), .msg_iovlen = 1, .msg_control = P(&ctl), .msg_controllen = LX_CMSG_LEN(8) };
    LT_EQ(SC(T, recvmsg, c, P(&m), LX_MSG_CMSG_CLOEXEC), 3);
    LT_MEMEQ(hdr, "hdr", 3);
    LT_EQ(ctl.h.cmsg_level, LX_SOL_SOCKET);
    LT_EQ(ctl.h.cmsg_type, LX_SCM_RIGHTS);
    LT_EQ((ctl.h.cmsg_len - sizeof(struct lx_cmsghdr)) / 4, 2);
    int shmfd = ctl.fd[0], fence_rd = ctl.fd[1];
    LT_EQ(SC(T, fcntl, shmfd, LX_F_GETFD), LX_FD_CLOEXEC);

    /* fstat of the shm descriptor: regular file semantics, the size the host set */
    struct lx_stat st;
    LT_EQ(SC(T, fstat, shmfd, P(&st)), 0);
    LT_EQ(st.st_size, SHM_SIZE);
    LT_EQ(st.st_mode & LX_S_IFMT, LX_S_IFREG);
    /* read and write on it are refused like the host does (ENXIO), mapping is the interface */
    char tmp[4];
    LT_ERRNO(SC(T, read, shmfd, P(tmp), 4), LX_ENXIO);

    /* mmap MAP_SHARED through mm at offset 0 */
    T->mm = lkd_mm_create();
    int64_t g = SC(T, mmap, 0, SHM_SIZE, LX_PROT_READ | LX_PROT_WRITE, LX_MAP_SHARED, shmfd, 0);
    LT_CHECK(g > 0);
    uint8_t *gm = lkd_g2h((uint64_t)g);
    LT_STREQ((char *)gm, "from-host");
    /* guest writes are seen by the host mapping, host writes by the guest mapping */
    memcpy(gm + 100, "guest-wrote", 12);
    LT_STREQ((char *)h.shm_map + 100, "guest-wrote");
    memcpy(h.shm_map + 20000, "host-late", 10);
    LT_STREQ((char *)gm + 20000, "host-late");
    /* the last 16 KiB page works too, and a second mapping of the same descriptor aliases the first */
    int64_t g2 = SC(T, mmap, 0, 16384, LX_PROT_READ | LX_PROT_WRITE, LX_MAP_SHARED, shmfd, SHM_SIZE - 16384);
    LT_CHECK(g2 > 0);
    ((uint8_t *)lkd_g2h((uint64_t)g2))[5] = 0x5a;
    LT_EQ(gm[SHM_SIZE - 16384 + 5], 0x5a);
    LT_EQ(h.shm_map[SHM_SIZE - 16384 + 5], 0x5a);
    /* a 4 KiB mapping at offset 4096: mm places it at an address with the same position in a 16 KiB
     * page (sub-page MAP_SHARED, docs/status/mm.md) and it aliases the object at that offset */
    int64_t g4 = SC(T, mmap, 0, 4096, LX_PROT_READ, LX_MAP_SHARED, shmfd, 4096);
    LT_CHECK(g4 > 0);
    LT_EQ((uint64_t)g4 & 0x3fff, 0x1000);
    h.shm_map[4096 + 7] = 0x6b;
    if (g4 > 0) LT_EQ(((uint8_t *)lkd_g2h((uint64_t)g4))[7], 0x6b);
    /* MAP_PRIVATE of a host shm is not available (macOS: EINVAL, MESURE F6) */
    int64_t pm = SC(T, mmap, 0, 16384, LX_PROT_READ, LX_MAP_PRIVATE, shmfd, 0);
    LT_CHECK(pm < 0);
    /* /proc/<pid>/maps shows the object as a deleted shm file */
    char fdesc[96];
    lkd_fdobj *so = lkd_fd_get(T, shmfd, NULL);
    LT_CHECK(so != NULL);
    int dl = so ? lkd_fs_describe(so, fdesc, sizeof fdesc) : -1;
    LT_CHECK(dl > 0 && strstr(fdesc, "(deleted)") != NULL);
    if (so) lkd_fdobj_put(so);

    /* the fence pipe end is pollable and epoll-able; nothing yet */
    struct lx_pollfd pf = { fence_rd, LX_POLLIN, 0 };
    LT_EQ(SC(T, poll, P(&pf), 1, 0), 0);
    int ep = (int)SC(T, epoll_create1, 0);
    struct lx_epoll_event ev = { LX_EPOLLIN, 1 }, out[2];
    LT_EQ(SC(T, epoll_ctl, ep, LX_EPOLL_CTL_ADD, fence_rd, P(&ev)), 0);
    ev.data = 2;
    LT_EQ(SC(T, epoll_ctl, ep, LX_EPOLL_CTL_ADD, c, P(&ev)), 0);
    LT_EQ(SC(T, epoll_wait, ep, P(out), 2, 0), 0);
    /* the host signals the fence: both poll and epoll see it */
    LT_EQ(write(h.fence_wr, "F", 1), 1);
    LT_EQ(SC(T, poll, P(&pf), 1, 1000), 1);
    LT_EQ(pf.revents, LX_POLLIN);
    LT_EQ(SC(T, epoll_wait, ep, P(out), 2, 1000), 1);
    LT_EQ(out[0].data, 1);
    char fb;
    LT_EQ(SC(T, read, fence_rd, P(&fb), 1), 1);
    LT_EQ(fb, 'F');

    /* guest sends a pipe write end to the host; the host writes into it; the guest reads it back */
    int32_t gp[2];
    LT_EQ(SC(T, pipe, P(gp)), 0);
    LT_EQ(send_one_fd(c, gp[1], "pipe-end"), 8);
    pthread_join(pt, NULL);
    LT_EQ(h.status, 0);
    LT_STREQ(h.got, "pipe-end");
    LT_EQ(h.peer_uid, getuid());                                   /* getpeereid sees the runtime's uid */
    LT_EQ(h.peer_gid, getgid());
    char rb[16] = { 0 };
    SC(T, close, gp[1]);
    LT_EQ(SC(T, read, gp[0], P(rb), 10), 10);
    LT_STREQ(rb, "host-wrote");

    /* host closes its side: the guest socket gets EPOLLIN|EPOLLHUP|RDHUP, read returns EOF */
    close(h.conn);
    LT_EQ(SC(T, epoll_wait, ep, P(out), 2, 1000), 1);
    LT_EQ(out[0].data, 2);
    LT_CHECK(out[0].events & LX_EPOLLHUP);
    char eb[4];
    LT_EQ(SC(T, read, c, P(eb), 4), 0);

    SC(T, munmap, (uint64_t)g, SHM_SIZE);
    SC(T, munmap, (uint64_t)g2, 16384);
    lkd_mm_put(T->mm);
    T->mm = NULL;
    SC(T, close, ep); SC(T, close, gp[0]); SC(T, close, fence_rd); SC(T, close, shmfd); SC(T, close, c);
    munmap(h.shm_map, SHM_SIZE);
    close(h.shm_fd); close(h.fence_wr); close(h.lfd);
    unlink(sa.sun_path);
}

/* A guest path outside any bound host directory does not reach the host: the file system is the guest
 * root, not the host root. */
LT_TEST(no_escape_to_host_paths)
{
    int c = (int)SC(T, socket, LX_AF_UNIX, LX_SOCK_STREAM, 0);
    struct lx_sockaddr_un ga = { .sun_family = LX_AF_UNIX };
    strcpy(ga.sun_path, "/run/wsd/../../../../../../tmp/nothing.sock");
    LT_ERRNO(SC(T, connect, c, P(&ga), 2 + 45), LX_ENOENT);
    SC(T, close, c);
}

int main(void)
{
    make_tmpdir(ROOT, sizeof ROOT, "nethost");
    make_tmpdir(HOSTDIR, sizeof HOSTDIR, "nethostd");
    put_dir(ROOT, "run");
    put_dir(ROOT, "run/wsd");
    lkd_config cfg = { .size = sizeof cfg, .guest_root = ROOT };
    if (lkd_init_config(&cfg) < 0 || fake_engine_init() < 0) { fprintf(stderr, "init failed\n"); return 2; }
    T = fs_task();
    lkd_sig_set_mask(T, LX_SIGBIT(LX_SIGPIPE));
    if (lkd_vfs_mount_bind("/run/wsd", HOSTDIR, 0) < 0) { fprintf(stderr, "bind mount failed\n"); return 2; }
    LT_RUN(guest_reaches_host_socket_and_exchanges_fds);
    LT_RUN(no_escape_to_host_paths);
    rm_rf(ROOT);
    rm_rf(HOSTDIR);
    return lt_finish();
}
