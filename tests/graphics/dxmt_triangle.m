/*
 * dxmt_triangle.m - owned native witness: D3D11 through dxmt-native on macOS arm64.
 *
 * Compiled as Objective-C++ (-x objective-c++) because the MinGW D3D11 headers shipped with DXMT
 * only provide __uuidof and the COM interfaces in C++ mode.
 *
 * Modes:
 *   dxmt_triangle <dxmt-native> offscreen
 *       D3D11CreateDevice, vs_pass/ps_color (precompiled DXBC, dxbc/PROVENANCE.txt), one colored
 *       triangle into a 256x256 RGBA8 render target, STAGING readback, per-pixel check against
 *       the analytic barycentric color.
 *   dxmt_triangle <dxmt-native> shared
 *       Same triangle into a BGRA8 texture created with D3D11_RESOURCE_MISC_SHARED
 *       (DXMT native: linear texture over anonymous POSIX shm). GetSharedHandle returns the
 *       'DMTX' v2 POD; the fd is sent with SCM_RIGHTS over a socketpair to a second process
 *       (this binary, mode "consumer", posix_spawn'ed), which mmaps it and checks the pixels.
 *   dxmt_triangle - consumer        (internal; socket on fd 3)
 *
 * Output ends with "RESULT PASS" or "RESULT FAIL". No threads created here, bounded loops.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>

#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "dxbc/shaders_dxbc.h"

extern char **environ;

/* Mirrors dxmt/src/dxmt-native/dxmt_native.h (commit bcdcf6da). */
#define DMTX_MAGIC 0x58544D44u
typedef struct {
  uint32_t magic, version;
  int32_t fd;
  uint32_t width, height, dxgi_format, mip_levels, array_size, sample_count;
  uint32_t bind_flags, misc_flags, cpu_access;
  uint64_t stride, size, offset;
} dmtx_pod;

enum { W = 256, H = 256 };
static const float kClear[4] = {0.0f, 0.0f, 0.2f, 1.0f};

struct Vertex {
  float pos[2];
  float uv[2];
  float color[4];
};
static const Vertex kTri[3] = {
    {{0.0f, 0.8f}, {0, 0}, {1, 0, 0, 1}},
    {{0.8f, -0.8f}, {0, 0}, {0, 0, 1, 1}},
    {{-0.8f, -0.8f}, {0, 0}, {0, 1, 0, 1}},
};
static const D3D11_INPUT_ELEMENT_DESC kLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
};

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static uint32_t crc32_of(const uint8_t *p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  while (n--) {
    c ^= *p++;
    for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
  }
  return ~c;
}

/* Expected RGBA8 at pixel (x,y); returns 0 for pixels too close to an edge to judge. */
static int expected_px(int x, int y, uint8_t out[4]) {
  float px = x + 0.5f, py = y + 0.5f;
  float vx[3], vy[3];
  for (int i = 0; i < 3; i++) {
    vx[i] = (kTri[i].pos[0] + 1.0f) * 0.5f * W;
    vy[i] = (1.0f - kTri[i].pos[1]) * 0.5f * H;
  }
  float d = (vy[1] - vy[2]) * (vx[0] - vx[2]) + (vx[2] - vx[1]) * (vy[0] - vy[2]);
  float b0 = ((vy[1] - vy[2]) * (px - vx[2]) + (vx[2] - vx[1]) * (py - vy[2])) / d;
  float b1 = ((vy[2] - vy[0]) * (px - vx[2]) + (vx[0] - vx[2]) * (py - vy[2])) / d;
  float b2 = 1.0f - b0 - b1;
  float mn = fminf(b0, fminf(b1, b2));
  if (fabsf(mn) < 0.01f) return 0; /* within ~1.5 px of an edge */
  if (mn < 0) {
    for (int c = 0; c < 4; c++) out[c] = (uint8_t)lrintf(kClear[c] * 255.0f);
    return 1;
  }
  for (int c = 0; c < 4; c++)
    out[c] = (uint8_t)lrintf((b0 * kTri[0].color[c] + b1 * kTri[1].color[c] + b2 * kTri[2].color[c]) * 255.0f);
  return 1;
}

/* Checks an image; bgra selects the byte order in memory. Returns mismatch count. */
static int check_image(const uint8_t *base, size_t stride, int bgra, const char *tag) {
  int bad = 0, judged = 0, inside = 0;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      uint8_t e[4];
      if (!expected_px(x, y, e)) continue;
      const uint8_t *p = base + y * stride + x * 4;
      uint8_t got[4] = {p[0], p[1], p[2], p[3]};
      if (bgra) { got[0] = p[2]; got[2] = p[0]; }
      judged++;
      if (e[3] == 255 && !(e[0] == 0 && e[1] == 0 && e[2] == 51)) inside++;
      int ok = 1;
      for (int c = 0; c < 4; c++) ok &= abs((int)got[c] - (int)e[c]) <= 3;
      if (!ok && bad++ < 5)
        printf("  %s mismatch px(%d,%d) got %u,%u,%u,%u expected %u,%u,%u,%u\n", tag, x, y, got[0], got[1], got[2],
               got[3], e[0], e[1], e[2], e[3]);
    }
  const uint8_t *c = base + 162 * stride + 128 * 4;
  printf("%s: judged=%d inside_triangle=%d mismatches=%d centroid_px(128,162)=%u,%u,%u,%u (%s)\n", tag, judged, inside,
         bad, c[0], c[1], c[2], c[3], bgra ? "BGRA" : "RGBA");
  return bad;
}

#define HR(expr)                                                                   \
  do {                                                                             \
    HRESULT hr_ = (expr);                                                          \
    if (FAILED(hr_)) {                                                             \
      printf("FAILED %s -> hr=0x%08x\nRESULT FAIL\n", #expr, (unsigned)hr_);       \
      exit(1);                                                                     \
    }                                                                              \
  } while (0)

struct Dev {
  ID3D11Device *dev;
  ID3D11DeviceContext *ctx;
};

static Dev create_device(const char *lib) {
  double t0 = now_ms();
  void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
  if (!h) { printf("dlopen failed: %s\nRESULT FAIL\n", dlerror()); exit(1); }
  PFN_D3D11_CREATE_DEVICE create = (PFN_D3D11_CREATE_DEVICE)dlsym(h, "D3D11CreateDevice");
  if (!create) { printf("no D3D11CreateDevice\nRESULT FAIL\n"); exit(1); }
  double t1 = now_ms();
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  Dev d = {};
  D3D_FEATURE_LEVEL fl = (D3D_FEATURE_LEVEL)0;
  HR(create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &d.dev, &fl, &d.ctx));
  double t2 = now_ms();
  IDXGIDevice *dxgi = nullptr;
  IDXGIAdapter *ad = nullptr;
  DXGI_ADAPTER_DESC desc = {};
  char name[129] = {};
  if (SUCCEEDED(d.dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&ad)) &&
      SUCCEEDED(ad->GetDesc(&desc)))
    for (int i = 0; i < 128 && desc.Description[i]; i++) name[i] = desc.Description[i] < 128 ? (char)desc.Description[i] : '?';
  if (ad) ad->Release();
  if (dxgi) dxgi->Release();
  printf("device: feature_level=0x%x adapter='%s' vendor=0x%04x dlopen_ms=%.1f create_ms=%.1f\n", fl, name,
         desc.VendorId, t1 - t0, t2 - t1);
  return d;
}

/* Draws the triangle into rtv. Returns elapsed ms for draw + GPU completion (EVENT query). */
static double draw_triangle(Dev &d, ID3D11RenderTargetView *rtv, int compile_report) {
  double t0 = now_ms();
  ID3D11VertexShader *vs = nullptr;
  ID3D11PixelShader *ps = nullptr;
  ID3D11InputLayout *il = nullptr;
  HR(d.dev->CreateVertexShader(dxbc_vs_pass, dxbc_vs_pass_len, nullptr, &vs));
  HR(d.dev->CreatePixelShader(dxbc_ps_color, dxbc_ps_color_len, nullptr, &ps));
  HR(d.dev->CreateInputLayout(kLayout, 3, dxbc_vs_pass, dxbc_vs_pass_len, &il));
  double t1 = now_ms();
  D3D11_BUFFER_DESC bd = {sizeof(kTri), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
  D3D11_SUBRESOURCE_DATA init = {kTri, 0, 0};
  ID3D11Buffer *vb = nullptr;
  HR(d.dev->CreateBuffer(&bd, &init, &vb));

  d.ctx->ClearRenderTargetView(rtv, kClear);
  d.ctx->OMSetRenderTargets(1, &rtv, nullptr);
  D3D11_VIEWPORT vp = {0, 0, W, H, 0, 1};
  d.ctx->RSSetViewports(1, &vp);
  UINT stride = sizeof(Vertex), off = 0;
  d.ctx->IASetVertexBuffers(0, 1, &vb, &stride, &off);
  d.ctx->IASetInputLayout(il);
  d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  d.ctx->VSSetShader(vs, nullptr, 0);
  d.ctx->PSSetShader(ps, nullptr, 0);
  d.ctx->Draw(3, 0);

  /* Wait for GPU completion with an EVENT query (bounded: 2 s). */
  D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
  ID3D11Query *q = nullptr;
  HR(d.dev->CreateQuery(&qd, &q));
  d.ctx->End(q);
  d.ctx->Flush();
  BOOL done = FALSE;
  int spins = 0;
  while (d.ctx->GetData(q, &done, sizeof(done), 0) != S_OK || !done) {
    if (++spins > 20000) { printf("EVENT query never completed\nRESULT FAIL\n"); exit(1); }
    usleep(100);
  }
  double t2 = now_ms();
  if (compile_report) printf("shaders+layout_ms=%.2f draw_to_gpu_done_ms=%.2f\n", t1 - t0, t2 - t1);
  q->Release(); vb->Release(); il->Release(); ps->Release(); vs->Release();
  return t2 - t0;
}

static int readback_check(Dev &d, ID3D11Texture2D *tex, int bgra, const char *tag, uint32_t *crc) {
  D3D11_TEXTURE2D_DESC desc;
  tex->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  desc.MiscFlags = 0;
  ID3D11Texture2D *st = nullptr;
  HR(d.dev->CreateTexture2D(&desc, nullptr, &st));
  d.ctx->CopyResource(st, tex);
  D3D11_MAPPED_SUBRESOURCE m = {};
  HR(d.ctx->Map(st, 0, D3D11_MAP_READ, 0, &m));
  static uint8_t packed[W * H * 4];
  for (int y = 0; y < H; y++) memcpy(packed + y * W * 4, (uint8_t *)m.pData + y * m.RowPitch, W * 4);
  d.ctx->Unmap(st, 0);
  st->Release();
  *crc = crc32_of(packed, sizeof(packed));
  printf("%s: row_pitch=%u crc32=%08x\n", tag, m.RowPitch, *crc);
  return check_image(packed, W * 4, bgra, tag);
}

static int mode_offscreen(const char *lib) {
  Dev d = create_device(lib);
  D3D11_TEXTURE2D_DESC td = {W, H, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                             D3D11_BIND_RENDER_TARGET, 0, 0};
  ID3D11Texture2D *rt = nullptr;
  ID3D11RenderTargetView *rtv = nullptr;
  HR(d.dev->CreateTexture2D(&td, nullptr, &rt));
  HR(d.dev->CreateRenderTargetView(rt, nullptr, &rtv));
  draw_triangle(d, rtv, 1);
  uint32_t crc;
  int bad = readback_check(d, rt, 0, "offscreen_rgba8", &crc);
  /* Repeat draws to time the steady state (shader objects are recreated each time, cache warm). */
  double t[20];
  for (int i = 0; i < 20; i++) t[i] = draw_triangle(d, rtv, 0);
  double s = 0, mx = 0;
  for (int i = 0; i < 20; i++) { s += t[i]; if (t[i] > mx) mx = t[i]; }
  printf("steady draw+complete over 20 runs: mean=%.3f ms max=%.3f ms\n", s / 20, mx);
  rtv->Release(); rt->Release(); d.ctx->Release(); d.dev->Release();
  printf("RESULT %s\n", bad ? "FAIL" : "PASS");
  return bad != 0;
}

static int send_fd(int sock, const void *buf, size_t n, int fd) {
  char ctl[CMSG_SPACE(sizeof(int))];
  struct iovec iov = {(void *)buf, n};
  struct msghdr m = {};
  m.msg_iov = &iov; m.msg_iovlen = 1;
  m.msg_control = ctl; m.msg_controllen = sizeof(ctl);
  struct cmsghdr *c = CMSG_FIRSTHDR(&m);
  c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(c), &fd, sizeof(int));
  return sendmsg(sock, &m, 0) == (ssize_t)n ? 0 : -1;
}

static int mode_shared(const char *lib, const char *self) {
  Dev d = create_device(lib);
  D3D11_TEXTURE2D_DESC td = {W, H, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                             D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_SHARED};
  ID3D11Texture2D *rt = nullptr;
  ID3D11RenderTargetView *rtv = nullptr;
  HR(d.dev->CreateTexture2D(&td, nullptr, &rt));
  HR(d.dev->CreateRenderTargetView(rt, nullptr, &rtv));
  IDXGIResource *res = nullptr;
  HANDLE hnd = nullptr;
  HR(rt->QueryInterface(__uuidof(IDXGIResource), (void **)&res));
  HR(res->GetSharedHandle(&hnd));
  const dmtx_pod *pod = (const dmtx_pod *)hnd;
  if (!pod || pod->magic != DMTX_MAGIC) { printf("shared handle is not a DMTX POD\nRESULT FAIL\n"); return 1; }
  struct stat st;
  fstat(pod->fd, &st);
  printf("pod: magic=0x%08x version=%u fd=%d %ux%u fmt=%u stride=%llu size=%llu offset=%llu bind=0x%x misc=0x%x "
         "shm_st_size=%lld\n", pod->magic, pod->version, pod->fd, pod->width, pod->height, pod->dxgi_format,
         (unsigned long long)pod->stride, (unsigned long long)pod->size, (unsigned long long)pod->offset,
         pod->bind_flags, pod->misc_flags, (long long)st.st_size);
  int fail = pod->version != 2;

  draw_triangle(d, rtv, 1);
  uint32_t crc;
  fail |= readback_check(d, rt, 1, "shared_bgra8_staging", &crc) != 0;

  /* Producer-side CPU view of the same shm object (no D3D11 copy). */
  size_t page = (size_t)getpagesize();
  size_t floor_off = pod->offset & ~(page - 1), delta = pod->offset - floor_off;
  size_t maplen = (delta + pod->size + page - 1) & ~(page - 1);
  uint8_t *v = (uint8_t *)mmap(nullptr, maplen, PROT_READ, MAP_SHARED, pod->fd, (off_t)floor_off);
  if (v == MAP_FAILED) { printf("producer mmap failed errno=%d\n", errno); fail = 1; }
  else {
    fail |= check_image(v + delta, pod->stride, 1, "shared_bgra8_producer_mmap") != 0;
    munmap(v, maplen);
  }

  /* Consumer process over a socketpair (fd 3 in the child). */
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) { perror("socketpair"); return 1; }
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, sv[1], 3);
  posix_spawn_file_actions_addclose(&fa, sv[0]);
  char *args[] = {(char *)self, (char *)"-", (char *)"consumer", nullptr};
  pid_t pid;
  double t0 = now_ms();
  if (posix_spawn(&pid, self, &fa, nullptr, args, environ)) { perror("posix_spawn"); return 1; }
  close(sv[1]);
  dmtx_pod wire = *pod;
  wire.fd = -1; /* process-local; the receiver patches it */
  if (send_fd(sv[0], &wire, sizeof(wire), pod->fd)) { perror("sendmsg"); fail = 1; }
  uint32_t reply[2] = {0, 0}; /* mismatches, crc */
  ssize_t r = read(sv[0], reply, sizeof(reply));
  int status = 0;
  waitpid(pid, &status, 0);
  printf("consumer: pid=%d exit=%d reply_bytes=%zd mismatches=%u crc32=%08x producer_staging_crc32=%08x "
         "(both over raw BGRA rows) spawn_to_reply_ms=%.2f\n",
         pid, WEXITSTATUS(status), r, reply[0], reply[1], crc, now_ms() - t0);
  fail |= r != (ssize_t)sizeof(reply) || reply[0] != 0 || WEXITSTATUS(status) != 0 || reply[1] != crc;
  close(sv[0]);
  res->Release(); rtv->Release(); rt->Release(); d.ctx->Release(); d.dev->Release();
  printf("RESULT %s\n", fail ? "FAIL" : "PASS");
  return fail;
}

static int mode_consumer(void) {
  int sock = 3;
  dmtx_pod pod;
  char ctl[CMSG_SPACE(sizeof(int))];
  struct iovec iov = {&pod, sizeof(pod)};
  struct msghdr m = {};
  m.msg_iov = &iov; m.msg_iovlen = 1;
  m.msg_control = ctl; m.msg_controllen = sizeof(ctl);
  if (recvmsg(sock, &m, MSG_WAITALL) != (ssize_t)sizeof(pod)) { printf("[consumer] recvmsg failed\n"); return 1; }
  struct cmsghdr *c = CMSG_FIRSTHDR(&m);
  if (!c || c->cmsg_type != SCM_RIGHTS) { printf("[consumer] no fd\n"); return 1; }
  memcpy(&pod.fd, CMSG_DATA(c), sizeof(int));
  size_t page = (size_t)getpagesize();
  size_t floor_off = pod.offset & ~(page - 1), delta = pod.offset - floor_off;
  size_t maplen = (delta + pod.size + page - 1) & ~(page - 1);
  uint8_t *v = (uint8_t *)mmap(nullptr, maplen, PROT_READ, MAP_SHARED, pod.fd, (off_t)floor_off);
  uint32_t reply[2] = {0xFFFFFFFFu, 0};
  if (v != MAP_FAILED && pod.magic == DMTX_MAGIC && pod.width == W && pod.height == H) {
    printf("[consumer pid=%d] mapped fd=%d len=%zu stride=%llu fmt=%u\n", getpid(), pod.fd, maplen,
           (unsigned long long)pod.stride, pod.dxgi_format);
    reply[0] = (uint32_t)check_image(v + delta, pod.stride, 1, "[consumer] shared_bgra8_mmap");
    static uint8_t packed[W * H * 4];
    for (int y = 0; y < H; y++) memcpy(packed + y * W * 4, v + delta + y * pod.stride, W * 4);
    reply[1] = crc32_of(packed, sizeof(packed));
    munmap(v, maplen);
  }
  write(sock, reply, sizeof(reply));
  return reply[0] != 0;
}

/* ------------------------------------------------------------------ wsd mode (G3 wiring)
 * Renders an animated (rotating) triangle with DXMT into NBUF shared BGRA8 textures, attaches each
 * texture's shm fd to the window server once, then presents by buffer id: the server samples the
 * same shm pages DXMT rendered into (no copy anywhere). GPU completion is awaited with an EVENT
 * query before PRESENT; a buffer is reused only after EV_RELEASE. */
#include "../../src/wsd/wsd_protocol.h"

enum { NB = 3 };
static int g_ws = -1;
static uint8_t g_win[1 << 16];
static size_t g_winlen;

static uint64_t up_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

static void ws_send(uint32_t type, const void *p, uint32_t n, const void *extra, uint32_t en, int fd) {
  uint8_t buf[sizeof(wsd_hdr) + WSD_MAX_PAYLOAD];
  wsd_hdr h = {type, n + en};
  memcpy(buf, &h, sizeof(h));
  if (n) memcpy(buf + sizeof(h), p, n);
  if (en) memcpy(buf + sizeof(h) + n, extra, en);
  size_t total = sizeof(h) + n + en;
  if (fd >= 0) {
    if (send_fd(g_ws, buf, total, fd)) { perror("sendmsg"); exit(1); }
    return;
  }
  for (size_t off = 0; off < total;) {
    ssize_t r = write(g_ws, buf + off, total - off);
    if (r <= 0) { perror("write"); exit(1); }
    off += (size_t)r;
  }
}

/* One event within timeout_ms; returns its type (0 if none) and copies the payload. */
static uint32_t ws_event(uint8_t *payload, uint32_t *len, int timeout_ms) {
  for (;;) {
    if (g_winlen >= sizeof(wsd_hdr)) {
      wsd_hdr h;
      memcpy(&h, g_win, sizeof(h));
      if (g_winlen >= sizeof(h) + h.len) {
        memcpy(payload, g_win + sizeof(h), h.len);
        *len = h.len;
        memmove(g_win, g_win + sizeof(h) + h.len, g_winlen - sizeof(h) - h.len);
        g_winlen -= sizeof(h) + h.len;
        return h.type;
      }
    }
    struct pollfd p = {g_ws, POLLIN, 0};
    if (poll(&p, 1, timeout_ms) <= 0) return 0;
    ssize_t r = read(g_ws, g_win + g_winlen, sizeof(g_win) - g_winlen);
    if (r <= 0) { printf("window server closed the connection\nRESULT FAIL\n"); exit(1); }
    g_winlen += (size_t)r;
  }
}

static int cmp_u64(const void *a, const void *b) {
  uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return x < y ? -1 : x > y;
}
static void pct(const char *name, uint64_t *v, int n) {
  if (n <= 0) { printf("  %s: no samples\n", name); return; }
  qsort(v, (size_t)n, sizeof(*v), cmp_u64);
  int i99 = (int)(n * 0.99); if (i99 >= n) i99 = n - 1;
  printf("  %s: n=%d min=%.3f p50=%.3f p95=%.3f p99=%.3f max=%.3f ms\n", name, n, v[0] / 1e6, v[n / 2] / 1e6,
         v[(int)(n * 0.95)] / 1e6, v[i99] / 1e6, v[n - 1] / 1e6);
}

static int mode_wsd(const char *lib, const char *sock, int frames, uint32_t hz) {
  struct sockaddr_un sa = {};
  sa.sun_family = AF_UNIX;
  snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", sock);
  g_ws = socket(AF_UNIX, SOCK_STREAM, 0);
  if (g_ws < 0 || connect(g_ws, (struct sockaddr *)&sa, sizeof(sa))) { perror("connect"); return 1; }
  uint8_t ev[WSD_MAX_PAYLOAD];
  uint32_t evlen;
  wsd_hello hello = {WSD_PROTOCOL_VERSION, (uint32_t)getpid()};
  ws_send(WSD_HELLO, &hello, sizeof(hello), nullptr, 0, -1);
  while (ws_event(ev, &evlen, 2000) != WSD_EV_HELLO) {}

  Dev d = create_device(lib);
  ID3D11VertexShader *vs = nullptr;
  ID3D11PixelShader *ps = nullptr;
  ID3D11InputLayout *il = nullptr;
  HR(d.dev->CreateVertexShader(dxbc_vs_pass, dxbc_vs_pass_len, nullptr, &vs));
  HR(d.dev->CreatePixelShader(dxbc_ps_color, dxbc_ps_color_len, nullptr, &ps));
  HR(d.dev->CreateInputLayout(kLayout, 3, dxbc_vs_pass, dxbc_vs_pass_len, &il));
  D3D11_BUFFER_DESC bd = {sizeof(kTri), D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
  ID3D11Buffer *vb = nullptr;
  HR(d.dev->CreateBuffer(&bd, nullptr, &vb));
  D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
  ID3D11Query *q = nullptr;
  HR(d.dev->CreateQuery(&qd, &q));

  ID3D11Texture2D *tex[NB];
  ID3D11RenderTargetView *rtv[NB];
  int busy[NB] = {0};
  for (int i = 0; i < NB; i++) {
    D3D11_TEXTURE2D_DESC td = {W, H, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_SHARED};
    HR(d.dev->CreateTexture2D(&td, nullptr, &tex[i]));
    HR(d.dev->CreateRenderTargetView(tex[i], nullptr, &rtv[i]));
    IDXGIResource *res = nullptr;
    HANDLE hnd = nullptr;
    HR(tex[i]->QueryInterface(__uuidof(IDXGIResource), (void **)&res));
    HR(res->GetSharedHandle(&hnd));
    const dmtx_pod *pod = (const dmtx_pod *)hnd;
    if (!pod || pod->magic != DMTX_MAGIC) { printf("not a DMTX POD\nRESULT FAIL\n"); return 1; }
    wsd_buffer_attach a = {(uint32_t)i + 1, pod->width, pod->height, (uint32_t)pod->stride, WSD_FMT_BGRA8, 0,
                           pod->offset, pod->size};
    ws_send(WSD_BUFFER_ATTACH, &a, sizeof(a), nullptr, 0, pod->fd); /* the POD keeps its own fd */
    printf("texture %d: DMTX v%u fd=%d stride=%llu size=%llu offset=%llu attached to wsd\n", i, pod->version, pod->fd,
           (unsigned long long)pod->stride, (unsigned long long)pod->size, (unsigned long long)pod->offset);
    res->Release();
  }

  wsd_window_create wc = {1, 2 * W, 2 * H, WSD_STYLE_TITLED | WSD_STYLE_CLOSABLE | WSD_STYLE_TOPMOST};
  ws_send(WSD_WINDOW_CREATE, &wc, sizeof(wc), "dxmt_triangle via wsd", 21, -1);
  wsd_window_flag show = {1, 1};
  ws_send(WSD_WINDOW_SHOW, &show, sizeof(show), nullptr, 0, -1);
  wsd_window_flag rate = {1, hz};
  ws_send(WSD_SET_FRAME_RATE, &rate, sizeof(rate), nullptr, 0, -1);

  static uint64_t render[4096], presented[4096], latency[4096];
  int nr = 0, np = 0, nzero = 0, errors = 0, starved = 0;
  uint64_t period = 1000000000ull / hz, t0 = up_ns() + 100000000ull;
  auto pump = [&](int timeout_ms) {
    uint32_t t = ws_event(ev, &evlen, timeout_ms);
    if (t == WSD_EV_RELEASE) {
      const wsd_ev_release *r = (const wsd_ev_release *)ev;
      if (r->buffer_id >= 1 && r->buffer_id <= NB) busy[r->buffer_id - 1] = 0;
    } else if (t == WSD_EV_PRESENTED) {
      const wsd_ev_presented *p = (const wsd_ev_presented *)ev;
      if (!p->presented_ns) nzero++;
      else if (np < 4096) { presented[np] = p->presented_ns; latency[np] = p->presented_ns - p->client_time_ns; np++; }
    } else if (t == WSD_EV_ERROR) {
      const wsd_ev_error *e = (const wsd_ev_error *)ev;
      printf("wsd error: request %u code %d\n", e->request_type, e->code);
      errors++;
    }
    return t;
  };
  for (int f = 0; f < frames && f < 4096; f++) {
    uint64_t deadline = t0 + (uint64_t)f * period;
    while (up_ns() < deadline) {
      int ms = (int)((deadline - up_ns()) / 1000000);
      if (!pump(ms)) { if (ms == 0) break; }
    }
    while (pump(0)) {}
    int b = -1;
    for (int i = 0; i < NB; i++) if (!busy[(f + i) % NB]) { b = (f + i) % NB; break; }
    if (b < 0) { starved++; continue; }
    uint64_t r0 = up_ns();
    /* Rotate the triangle around the origin. */
    D3D11_MAPPED_SUBRESOURCE m = {};
    HR(d.ctx->Map(vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m));
    Vertex v[3];
    memcpy(v, kTri, sizeof(v));
    float a = f * 0.05f, c = cosf(a), s = sinf(a);
    for (int i = 0; i < 3; i++) {
      float x = kTri[i].pos[0], y = kTri[i].pos[1];
      v[i].pos[0] = x * c - y * s;
      v[i].pos[1] = x * s + y * c;
    }
    memcpy(m.pData, v, sizeof(v));
    d.ctx->Unmap(vb, 0);
    d.ctx->ClearRenderTargetView(rtv[b], kClear);
    d.ctx->OMSetRenderTargets(1, &rtv[b], nullptr);
    D3D11_VIEWPORT vp = {0, 0, W, H, 0, 1};
    d.ctx->RSSetViewports(1, &vp);
    UINT stride = sizeof(Vertex), off = 0;
    d.ctx->IASetVertexBuffers(0, 1, &vb, &stride, &off);
    d.ctx->IASetInputLayout(il);
    d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d.ctx->VSSetShader(vs, nullptr, 0);
    d.ctx->PSSetShader(ps, nullptr, 0);
    d.ctx->Draw(3, 0);
    d.ctx->End(q);
    d.ctx->Flush();
    BOOL done = FALSE;
    int spins = 0;
    while (d.ctx->GetData(q, &done, sizeof(done), 0) != S_OK || !done) {
      if (++spins > 20000) { printf("EVENT query never completed\nRESULT FAIL\n"); return 1; }
      usleep(50);
    }
    render[nr++] = up_ns() - r0;
    busy[b] = 1;
    wsd_present p = {1, (uint32_t)b + 1, (uint64_t)f + 1, up_ns()};
    ws_send(WSD_PRESENT, &p, sizeof(p), nullptr, 0, -1);
  }
  uint64_t end = up_ns() + 300000000ull;
  while (up_ns() < end) pump(20);

  printf("dxmt->wsd at %u Hz: frames=%d rendered=%d starved=%d presented=%d presented_time_zero=%d errors=%d\n", hz,
         frames, nr, starved, np, nzero, errors);
  static uint64_t iv[4096];
  int ni = 0;
  for (int i = 1; i < np; i++) iv[ni++] = presented[i] - presented[i - 1];
  pct("DXMT render+GPU complete per frame", render, nr);
  pct("interval between presentedTime", iv, ni);
  pct("latency PRESENT->presentedTime", latency, np);
  wsd_window wd = {1};
  ws_send(WSD_WINDOW_DESTROY, &wd, sizeof(wd), nullptr, 0, -1);
  close(g_ws);
  for (int i = 0; i < NB; i++) { rtv[i]->Release(); tex[i]->Release(); }
  q->Release(); vb->Release(); il->Release(); ps->Release(); vs->Release();
  d.ctx->Release(); d.dev->Release();
  int fail = errors || np < frames * 8 / 10;
  printf("RESULT %s\n", fail ? "FAIL" : "PASS");
  return fail;
}

int main(int argc, char **argv) {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  if (argc >= 3 && !strcmp(argv[2], "consumer")) return mode_consumer();
  if (argc < 3) {
    fprintf(stderr, "usage: %s <dxmt-native> offscreen|shared|wsd <socket> [frames<=4096] [hz]\n", argv[0]);
    return 2;
  }
  @autoreleasepool {
    if (!strcmp(argv[2], "offscreen")) return mode_offscreen(argv[1]);
    if (!strcmp(argv[2], "shared")) return mode_shared(argv[1], argv[0]);
    if (!strcmp(argv[2], "wsd") && argc >= 4) {
      int frames = argc > 4 ? atoi(argv[4]) : 240;
      uint32_t hz = argc > 5 ? (uint32_t)atoi(argv[5]) : 60;
      if (frames < 1 || frames > 4096) frames = 240;
      if (hz < 1 || hz > 240) hz = 60;
      return mode_wsd(argv[1], argv[3], frames, hz);
    }
  }
  return 2;
}
