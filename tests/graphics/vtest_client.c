/*
 * vtest_client.c - owned native witness for the Neptune vtest handshake on macOS.
 *
 * Replays the client sequence of NEP/mesa-triton/src/virtio/neptune/npt_renderer_vtest_wine.c
 * (create_renderer -> ping -> protocol version -> GET_PARAM -> GET_CAPSET -> CONTEXT_INIT)
 * against the annex-A virgl_test_server, then exercises:
 *   - RESOURCE_CREATE_BLOB(HOST3D_GUEST, MAPPABLE): fd via SCM_RIGHTS, mmap MAP_SHARED,
 *     coherence check through a second independent mapping of the same fd;
 *   - SUBMIT_CMD2(RING_IDX|OUT_FENCE_FD) with an empty batch: fd via SCM_RIGHTS, poll until readable.
 * Prints latency percentiles and a final "RESULT PASS|FAIL" line. Single-threaded, bounded loops.
 *
 * Usage: vtest_client <socket-path> [fence_iters<=2000] [blob_iters<=200]
 * Constants mirror virgl/vtest/vtest_protocol.h (protocol 4) and neptune_hw.h.
 */
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define VTEST_HDR_SIZE 2
#define VTEST_CMD_LEN 0
#define VTEST_CMD_ID 1
#define VCMD_RESOURCE_UNREF 3
#define VCMD_RESOURCE_BUSY_WAIT 7
#define VCMD_CREATE_RENDERER 8
#define VCMD_PING_PROTOCOL_VERSION 10
#define VCMD_PROTOCOL_VERSION 11
#define VCMD_GET_PARAM 15
#define VCMD_GET_CAPSET 16
#define VCMD_CONTEXT_INIT 17
#define VCMD_RESOURCE_CREATE_BLOB 18
#define VCMD_SUBMIT_CMD2 24
#define VCMD_PARAM_MAX_TIMELINE_COUNT 1
#define VCMD_BLOB_TYPE_HOST3D_GUEST 3
#define VCMD_BLOB_FLAG_MAPPABLE 1
#define SUBMIT2_RING_IDX 1
#define SUBMIT2_OUT_FENCE_FD 4
#define CLIENT_PROTOCOL_VERSION 4
#define NPT_CAPSET_ID 7            /* npt_common.h:131 */
#define NPT_WIRE_VERSION 1u        /* npt_protocol_defs.h:66 */

static int g_sock = -1;
static int g_fail;

#define CHECK(cond, ...) do { if (!(cond)) { printf("CHECK FAILED: " __VA_ARGS__); printf("\n"); g_fail = 1; } } while (0)

static double now_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
   return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void die(const char *what)
{
   printf("FATAL: %s (errno=%d %s)\nRESULT FAIL\n", what, errno, strerror(errno));
   exit(1);
}

static void wr(const void *buf, size_t n)
{
   const uint8_t *p = buf;
   while (n) {
      ssize_t r = write(g_sock, p, n);
      if (r <= 0) die("socket write");
      p += r; n -= (size_t)r;
   }
}

static void rd(void *buf, size_t n)
{
   uint8_t *p = buf;
   while (n) {
      ssize_t r = read(g_sock, p, n);
      if (r <= 0) die("socket read (server closed the connection?)");
      p += r; n -= (size_t)r;
   }
}

static void cmd(uint32_t id, const uint32_t *data, uint32_t ndw)
{
   uint32_t h[VTEST_HDR_SIZE] = { ndw, id };
   wr(h, sizeof(h));
   if (ndw) wr(data, ndw * 4);
}

static void skip_dw(uint32_t n)
{
   uint32_t d;
   while (n--) rd(&d, 4);
}

static int recv_fd(void)
{
   char c;
   char ctl[CMSG_SPACE(sizeof(int))];
   struct iovec iov = { &c, 1 };
   struct msghdr m = { 0 };
   m.msg_iov = &iov; m.msg_iovlen = 1;
   m.msg_control = ctl; m.msg_controllen = sizeof(ctl);
   ssize_t r = recvmsg(g_sock, &m, 0);
   if (r <= 0) die("recvmsg");
   struct cmsghdr *cm = CMSG_FIRSTHDR(&m);
   if (!cm || cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS) return -1;
   int fd;
   memcpy(&fd, CMSG_DATA(cm), sizeof(fd));
   return fd;
}

static int cmp_d(const void *a, const void *b)
{
   double x = *(const double *)a, y = *(const double *)b;
   return x < y ? -1 : x > y;
}

static void stats(const char *name, double *v, int n)
{
   if (n <= 0) { printf("%s: no samples\n", name); return; }
   qsort(v, (size_t)n, sizeof(double), cmp_d);
   printf("%s: n=%d min=%.4f p50=%.4f p95=%.4f p99=%.4f max=%.4f ms\n", name, n,
          v[0], v[n / 2], v[(int)(n * 0.95)], v[(int)(n * 0.99) < n ? (int)(n * 0.99) : n - 1], v[n - 1]);
}

/* Returns res_id; *fd gets the SCM_RIGHTS fd. */
static uint32_t create_blob(uint64_t size, int *fd)
{
   uint32_t d[6] = { VCMD_BLOB_TYPE_HOST3D_GUEST, VCMD_BLOB_FLAG_MAPPABLE,
                     (uint32_t)size, (uint32_t)(size >> 32), 0, 0 };
   uint32_t h[2], res;
   cmd(VCMD_RESOURCE_CREATE_BLOB, d, 6);
   rd(h, sizeof(h));
   rd(&res, 4);
   if (h[VTEST_CMD_LEN] > 1) skip_dw(h[VTEST_CMD_LEN] - 1);
   *fd = recv_fd();
   return res;
}

static int blob_check(uint64_t size, double *lat)
{
   double t0 = now_ms();
   int fd;
   uint32_t res = create_blob(size, &fd);
   if (fd < 0) { printf("blob %llu: no fd\n", (unsigned long long)size); return -1; }
   uint8_t *a = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
   if (a == MAP_FAILED) { close(fd); printf("blob mmap failed errno=%d\n", errno); return -1; }
   /* Touch one byte per 16 KiB page plus the last byte. */
   for (uint64_t off = 0; off < size; off += 16384) a[off] = (uint8_t)(off >> 14);
   a[size - 1] = 0xA5;
   if (lat) *lat = now_ms() - t0;
   /* Second independent MAP_SHARED view of the same object must see the writes. */
   uint8_t *b = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
   int ok = b != MAP_FAILED;
   if (ok) {
      for (uint64_t off = 0; off < size; off += 16384) ok &= b[off] == (uint8_t)(off >> 14);
      ok &= b[size - 1] == 0xA5;
      munmap(b, size);
   }
   struct stat st;
   fstat(fd, &st);
   munmap(a, size);
   close(fd);
   uint32_t u = res;
   cmd(VCMD_RESOURCE_UNREF, &u, 1);
   if (!lat)
      printf("blob size=%llu res_id=%u st_size=%lld shared_view_coherent=%s\n",
             (unsigned long long)size, res, (long long)st.st_size, ok ? "yes" : "NO");
   return ok ? 0 : -1;
}

/* Empty SUBMIT_CMD2 with RING_IDX|OUT_FENCE_FD; returns ms until the fence fd is readable. */
static double fence_roundtrip(uint32_t ring, int *signalled)
{
   uint32_t body[9] = { 1, SUBMIT2_RING_IDX | SUBMIT2_OUT_FENCE_FD, 0, 0, 0, 0, ring, 0, 0 };
   double t0 = now_ms();
   cmd(VCMD_SUBMIT_CMD2, body, 9);
   int fd = recv_fd();
   if (fd < 0) { *signalled = 0; return -1; }
   struct pollfd p = { fd, POLLIN, 0 };
   int r = poll(&p, 1, 2000);
   double dt = now_ms() - t0;
   *signalled = r == 1 && (p.revents & (POLLIN | POLLHUP));
   close(fd);
   return dt;
}

int main(int argc, char **argv)
{
   if (argc < 2) { fprintf(stderr, "usage: %s <socket> [fence_iters] [blob_iters]\n", argv[0]); return 2; }
   int fence_iters = argc > 2 ? atoi(argv[2]) : 200;
   int blob_iters = argc > 3 ? atoi(argv[3]) : 50;
   if (fence_iters < 1 || fence_iters > 2000) fence_iters = 200;
   if (blob_iters < 1 || blob_iters > 200) blob_iters = 50;
   setvbuf(stdout, NULL, _IOLBF, 0);

   struct sockaddr_un sa = { .sun_family = AF_UNIX };
   if (strlen(argv[1]) >= sizeof(sa.sun_path)) die("socket path too long");
   strcpy(sa.sun_path, argv[1]);
   g_sock = socket(AF_UNIX, SOCK_STREAM, 0);
   if (g_sock < 0 || connect(g_sock, (struct sockaddr *)&sa, sizeof(sa))) die("connect");
   double t_start = now_ms();

   /* CREATE_RENDERER("neptune") - no reply. */
   const char name[] = "neptune";
   uint32_t h[2] = { sizeof(name), VCMD_CREATE_RENDERER };
   wr(h, sizeof(h));
   wr(name, sizeof(name));

   /* PING_PROTOCOL_VERSION followed by a BUSY_WAIT on handle 0, as the Neptune client does. */
   uint32_t busy[2] = { 0, 0 };
   cmd(VCMD_PING_PROTOCOL_VERSION, NULL, 0);
   cmd(VCMD_RESOURCE_BUSY_WAIT, busy, 2);
   rd(h, sizeof(h));
   uint32_t proto = 0;
   if (h[VTEST_CMD_ID] == VCMD_PING_PROTOCOL_VERSION) {
      rd(h, sizeof(h));
      skip_dw(h[VTEST_CMD_LEN]);
      uint32_t v = CLIENT_PROTOCOL_VERSION;
      cmd(VCMD_PROTOCOL_VERSION, &v, 1);
      rd(h, sizeof(h));
      rd(&proto, 4);
   } else {
      skip_dw(h[VTEST_CMD_LEN]);
   }
   printf("protocol_version=%u\n", proto);
   CHECK(proto >= 3, "protocol %u < 3", proto);
   if (proto < 3) { printf("RESULT FAIL\n"); return 1; }

   uint32_t param = VCMD_PARAM_MAX_TIMELINE_COUNT, resp[2];
   cmd(VCMD_GET_PARAM, &param, 1);
   rd(h, sizeof(h));
   rd(resp, sizeof(resp));
   uint32_t max_tl = resp[0] ? resp[1] : 0;
   printf("max_timeline_count=%u\n", max_tl);
   CHECK(max_tl > 0, "MAX_TIMELINE_COUNT is 0");

   uint32_t cs_req[2] = { NPT_CAPSET_ID, 0 }, valid = 0, capset[15] = { 0 };
   cmd(VCMD_GET_CAPSET, cs_req, 2);
   rd(h, sizeof(h));
   rd(&valid, 4);
   uint32_t cs_dw = h[VTEST_CMD_LEN] - 1;
   for (uint32_t i = 0; i < cs_dw; i++) {
      uint32_t d;
      rd(&d, 4);
      if (i < 15) capset[i] = d;
   }
   printf("capset id=%u valid=%u bytes=%u wire_format_version=%u caps_flags=0x%x\n",
          NPT_CAPSET_ID, valid, cs_dw * 4, capset[0], capset[1]);
   CHECK(valid && capset[0] == NPT_WIRE_VERSION, "Neptune capset invalid or wire mismatch");

   uint32_t ctx_cap = NPT_CAPSET_ID;
   cmd(VCMD_CONTEXT_INIT, &ctx_cap, 1);
   /* CONTEXT_INIT has no reply; a GET_PARAM round trip proves the server kept the connection. */
   cmd(VCMD_GET_PARAM, &param, 1);
   rd(h, sizeof(h));
   rd(resp, sizeof(resp));
   printf("context_init=ok (post-init GET_PARAM answered) handshake_ms=%.3f\n", now_ms() - t_start);

   /* Blob correctness at three sizes, including a non-16K-multiple size. */
   static const uint64_t sizes[] = { 65536, 12288, 1u << 20, 16u << 20 };
   for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
      CHECK(blob_check(sizes[i], NULL) == 0, "blob size %llu", (unsigned long long)sizes[i]);

   /* Blob create+map+touch latency. */
   static double lat[2000];
   static const uint64_t lsizes[] = { 65536, 1u << 20, 16u << 20 };
   for (unsigned s = 0; s < 3; s++) {
      int n = 0;
      for (int i = 0; i < blob_iters; i++)
         if (blob_check(lsizes[s], &lat[n]) == 0) n++;
      char label[64];
      snprintf(label, sizeof(label), "blob_create_map_touch_%lluKiB", (unsigned long long)(lsizes[s] >> 10));
      stats(label, lat, n);
      CHECK(n == blob_iters, "%d/%d blobs ok", n, blob_iters);
   }

   /* Fence round trips on ring 0 and ring 1. */
   for (uint32_t ring = 0; ring < 2 && ring < max_tl; ring++) {
      int ok = 0, n = 0;
      for (int i = 0; i < fence_iters; i++) {
         int sig = 0;
         double dt = fence_roundtrip(ring, &sig);
         if (dt >= 0) lat[n++] = dt;
         ok += sig;
      }
      char label[64];
      snprintf(label, sizeof(label), "submit_cmd2_out_fence_ring%u", ring);
      printf("%s: readable=%d/%d\n", label, ok, fence_iters);
      stats(label, lat, n);
      CHECK(ok == fence_iters, "fence fds not all readable on ring %u", ring);
   }

   close(g_sock);
   printf("RESULT %s\n", g_fail ? "FAIL" : "PASS");
   return g_fail;
}
