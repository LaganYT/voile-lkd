/* w35_scmpipe: a pipe end sent to another process through SCM_RIGHTS keeps its blocking mode, as Linux shares the
 * open file description. wineserver creates each thread's request pipe, makes its own read end non-blocking and
 * sends the write end to the client, which writes its requests in blocking mode: one that met a full pipe got
 * EAGAIN under LKD and Wine ended the thread (ELDEN RING froze with XInput's lock held by that thread).
 * Case 1 (Wine's): the child creates the pipe, sets O_NONBLOCK on its read end, sends the write end; the parent sees
 * a blocking descriptor and writes 256 KiB (four times the pipe) while the child reads slowly: every write must
 * complete. Case 2: a write end sent with O_NONBLOCK set stays non-blocking: the receiver fills it and gets EAGAIN.
 * Usage: w35_scmpipe (no argument). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define TOTAL (256 * 1024)

static int failures;
#define CHECK(c, ...) do { if (!(c)) { printf("W35 FAIL " __VA_ARGS__); printf("\n"); failures++; } } while (0)

static void send_fd(int sock, int fd)
{
    char c = 'f';
    struct iovec iov = { &c, 1 };
    union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof(int))]; } u;
    memset(&u, 0, sizeof u);
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = u.buf, .msg_controllen = sizeof u.buf };
    struct cmsghdr *h = CMSG_FIRSTHDR(&m);
    h->cmsg_level = SOL_SOCKET;
    h->cmsg_type = SCM_RIGHTS;
    h->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(h), &fd, sizeof fd);
    if (sendmsg(sock, &m, 0) != 1) { perror("sendmsg"); exit(2); }
}

static int recv_fd(int sock)
{
    char c;
    struct iovec iov = { &c, 1 };
    union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof(int))]; } u;
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = u.buf, .msg_controllen = sizeof u.buf };
    if (recvmsg(sock, &m, 0) != 1) { perror("recvmsg"); exit(2); }
    struct cmsghdr *h = CMSG_FIRSTHDR(&m);
    if (!h || h->cmsg_type != SCM_RIGHTS) { printf("W35 FAIL no descriptor received\n"); exit(1); }
    int fd;
    memcpy(&fd, CMSG_DATA(h), sizeof fd);
    return fd;
}

/* Child: creates the pipe, sends the write end (O_NONBLOCK on it when nb), reads TOTAL bytes slowly (case 1) or
 * waits for the parent's verdict (case 2). */
static void child(int sock, int nb)
{
    int p[2];
    if (pipe(p) < 0) { perror("pipe"); _exit(2); }
    fcntl(p[0], F_SETFL, O_NONBLOCK);                 /* wineserver's own end */
    if (nb) fcntl(p[1], F_SETFL, O_NONBLOCK);
    send_fd(sock, p[1]);
    close(p[1]);
    char buf[4096];
    long got = 0;
    usleep(300 * 1000);                               /* let the writer fill the pipe first */
    for (;;) {
        ssize_t r = read(p[0], buf, sizeof buf);
        if (r > 0) { got += r; usleep(200); continue; }
        if (r == 0) break;
        if (errno == EAGAIN) { usleep(1000); continue; }
        perror("read");
        _exit(2);
    }
    _exit(nb ? 0 : (got == TOTAL ? 0 : 3));
}

int main(void)
{
    for (int nb = 0; nb < 2; nb++) {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) { perror("socketpair"); return 2; }
        pid_t pid = fork();
        if (pid == 0) { close(sv[0]); child(sv[1], nb); }
        close(sv[1]);
        int fd = recv_fd(sv[0]);
        int fl = fcntl(fd, F_GETFL);
        CHECK(((fl & O_NONBLOCK) != 0) == nb, "case %d: received O_NONBLOCK %d, sent %d", nb + 1, (fl & O_NONBLOCK) != 0, nb);
        CHECK((fl & O_ACCMODE) == O_WRONLY, "case %d: received access mode %#x, want O_WRONLY", nb + 1, fl & O_ACCMODE);
        char buf[4096];
        memset(buf, 'w', sizeof buf);
        long sent = 0;
        int eagain = 0;
        while (sent < TOTAL) {
            ssize_t w = write(fd, buf, sizeof buf);
            if (w > 0) { sent += w; continue; }
            if (w < 0 && errno == EAGAIN) { eagain++; if (nb) break; continue; }
            perror("write");
            return 2;
        }
        if (nb) CHECK(eagain > 0, "case 2: a full non-blocking pipe never gave EAGAIN (sent %ld)", sent);
        else CHECK(eagain == 0 && sent == TOTAL, "case 1: blocking writes got EAGAIN %d times (sent %ld of %d)", eagain, sent, TOTAL);
        close(fd);
        int st = 0;
        waitpid(pid, &st, 0);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "case %d: reader exit status %#x", nb + 1, st);
        close(sv[0]);
    }
    if (failures) {
        printf("W35 FAIL %d\n", failures);
        return 1;
    }
    printf("W35 PASS\n");
    return 0;
}
