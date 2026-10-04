/*
 * sailover-input.c - SailOver's input service: native arm64 process that reads the Mac's gamepads
 * with SDL3 and serves them to Wine's SailOver bus over <dir>/input.sock.
 * Wire format: sailover_input_protocol.h.
 *
 * One thread: poll() on the listening socket and the clients, then SDL's events. SDL reads the
 * gamepads' reports on its own and the loop collects their state, every millisecond while a gamepad
 * is open and every 50 ms otherwise (to notice new ones). A gamepad's state goes to the clients only
 * when it changed. The service ends on SIGTERM, SIGINT or SIGHUP, and when the program that started
 * it is gone.
 *
 * Usage: sailover-input --dir <private-dir>
 */
#include <SDL3/SDL.h>
#include <CoreFoundation/CoreFoundation.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "sailover_input_protocol.h"

#define LOG(...) do { fprintf(stderr, "sailover-input: " __VA_ARGS__); fputc('\n', stderr); } while (0)

#define MAX_CLIENTS 8
#define MAX_GAMEPADS 16
/* A client that lets this many bytes of events pile up is disconnected. */
#define OUT_CAP 65536

struct client {
  int fd; /* -1 when the slot is free */
  pid_t pid;
  int ready; /* sent HELLO */
  size_t in_len, out_len;
  uint8_t in[sizeof(struct si_hdr) + SI_MAX_PAYLOAD];
  uint8_t out[OUT_CAP];
};

struct gamepad {
  SDL_Gamepad *pad; /* NULL when the slot is free */
  uint32_t caps;
  struct si_ev_added added;
  struct si_ev_state state;
};

static struct client clients[MAX_CLIENTS];
static struct gamepad gamepads[MAX_GAMEPADS];
static volatile sig_atomic_t quit_signal;

static void on_signal(int sig) { quit_signal = sig; }

static int make_listener(const char *dir, char *path_out, size_t path_cap) {
  if (mkdir(dir, 0700) && errno != EEXIST) { LOG("mkdir %s: %s", dir, strerror(errno)); return -1; }
  struct stat st;
  if (lstat(dir, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 0077)) {
    LOG("refusing %s: must be a directory owned by uid %d with mode 0700", dir, getuid());
    return -1;
  }
  struct sockaddr_un sa = {.sun_family = AF_UNIX};
  int n = snprintf(sa.sun_path, sizeof(sa.sun_path), "%s/input.sock", dir);
  if (n < 0 || (size_t)n >= sizeof(sa.sun_path)) { LOG("socket path too long (limit %zu)", sizeof(sa.sun_path) - 1); return -1; }
  unlink(sa.sun_path);
  int s = socket(AF_UNIX, SOCK_STREAM, 0);
  if (s < 0 || bind(s, (struct sockaddr *)&sa, sizeof(sa)) || chmod(sa.sun_path, 0600) || listen(s, 8)) {
    LOG("listen %s: %s", sa.sun_path, strerror(errno));
    return -1;
  }
  fcntl(s, F_SETFL, O_NONBLOCK);
  snprintf(path_out, path_cap, "%s", sa.sun_path);
  return s;
}

static void client_close(struct client *c, const char *why) {
  LOG("client pid %d gone (%s)", c->pid, why);
  close(c->fd);
  c->fd = -1;
}

static void client_flush(struct client *c) {
  while (c->out_len) {
    ssize_t n = write(c->fd, c->out, c->out_len);
    if (n < 0) {
      if (errno == EAGAIN || errno == EINTR) return;
      client_close(c, strerror(errno));
      return;
    }
    memmove(c->out, c->out + n, c->out_len - (size_t)n);
    c->out_len -= (size_t)n;
  }
}

static void client_send(struct client *c, uint32_t type, const void *payload, uint32_t len) {
  struct si_hdr hdr = {type, len};
  if (c->fd < 0) return;
  if (c->out_len + sizeof(hdr) + len > sizeof(c->out)) {
    client_close(c, "not reading its events");
    return;
  }
  memcpy(c->out + c->out_len, &hdr, sizeof(hdr));
  memcpy(c->out + c->out_len + sizeof(hdr), payload, len);
  c->out_len += sizeof(hdr) + len;
  client_flush(c);
}

static void broadcast(uint32_t type, const void *payload, uint32_t len) {
  for (int i = 0; i < MAX_CLIENTS; i++)
    if (clients[i].fd >= 0 && clients[i].ready) client_send(&clients[i], type, payload, len);
}

static struct gamepad *find_gamepad(uint32_t id) {
  for (int i = 0; i < MAX_GAMEPADS; i++)
    if (gamepads[i].pad && gamepads[i].added.gamepad_id == id) return &gamepads[i];
  return NULL;
}

static int gamepad_count(void) {
  int n = 0;
  for (int i = 0; i < MAX_GAMEPADS; i++) n += gamepads[i].pad != NULL;
  return n;
}

static void read_state(struct gamepad *g, struct si_ev_state *state) {
  memset(state, 0, sizeof(*state));
  state->gamepad_id = g->added.gamepad_id;
  for (int b = 0; b < SI_BUTTON_COUNT && b < SDL_GAMEPAD_BUTTON_COUNT; b++)
    if (SDL_GetGamepadButton(g->pad, (SDL_GamepadButton)b)) state->buttons |= 1u << b;
  for (int a = 0; a < SI_AXIS_COUNT; a++) state->axes[a] = SDL_GetGamepadAxis(g->pad, (SDL_GamepadAxis)a);
}

static void gamepad_added(SDL_JoystickID which) {
  if (find_gamepad(which)) return;
  struct gamepad *g = NULL;
  for (int i = 0; i < MAX_GAMEPADS && !g; i++)
    if (!gamepads[i].pad) g = &gamepads[i];
  if (!g) { LOG("ignoring gamepad %u: %d already open", which, MAX_GAMEPADS); return; }

  SDL_Gamepad *pad = SDL_OpenGamepad(which);
  if (!pad) { LOG("cannot open gamepad %u: %s", which, SDL_GetError()); return; }
  memset(g, 0, sizeof(*g));
  g->pad = pad;
  SDL_PropertiesID props = SDL_GetGamepadProperties(pad);
  if (SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false)) g->caps |= SI_CAP_RUMBLE;
  if (SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false)) g->caps |= SI_CAP_TRIGGER_RUMBLE;

  struct si_ev_added *a = &g->added;
  a->gamepad_id = which;
  a->vendor = SDL_GetGamepadVendor(pad);
  a->product = SDL_GetGamepadProduct(pad);
  a->version = SDL_GetGamepadProductVersion(pad);
  switch (SDL_GetGamepadConnectionState(pad)) {
  case SDL_JOYSTICK_CONNECTION_WIRED: a->bus = SI_BUS_USB; break;
  case SDL_JOYSTICK_CONNECTION_WIRELESS: a->bus = SI_BUS_BLUETOOTH; break;
  default: a->bus = SI_BUS_UNKNOWN; break;
  }
  a->caps = g->caps;
  const char *name = SDL_GetGamepadName(pad), *serial = SDL_GetGamepadSerial(pad);
  snprintf(a->name, sizeof(a->name), "%s", name ? name : "Gamepad");
  snprintf(a->serial, sizeof(a->serial), "%s", serial ? serial : "");
  read_state(g, &g->state);
  LOG("gamepad %u added: %s %04x:%04x%s", which, a->name, a->vendor, a->product,
      g->caps & SI_CAP_RUMBLE ? ", rumble" : "");
  broadcast(SI_EV_ADDED, a, sizeof(*a));
  broadcast(SI_EV_STATE, &g->state, sizeof(g->state));
}

static void gamepad_removed(SDL_JoystickID which) {
  struct gamepad *g = find_gamepad(which);
  if (!g) return;
  struct si_gamepad msg = {which};
  LOG("gamepad %u removed", which);
  SDL_CloseGamepad(g->pad);
  g->pad = NULL;
  broadcast(SI_EV_REMOVED, &msg, sizeof(msg));
}

static void send_changed_states(void) {
  for (int i = 0; i < MAX_GAMEPADS; i++) {
    struct gamepad *g = &gamepads[i];
    struct si_ev_state now;
    if (!g->pad) continue;
    read_state(g, &now);
    if (!memcmp(&now, &g->state, sizeof(now))) continue;
    g->state = now;
    broadcast(SI_EV_STATE, &now, sizeof(now));
  }
}

static void handle_message(struct client *c, const struct si_hdr *hdr, const uint8_t *payload) {
  if (hdr->type == SI_HELLO && hdr->len >= sizeof(struct si_hello)) {
    const struct si_hello *hello = (const void *)payload;
    struct si_hello reply = {SI_PROTOCOL_VERSION, (uint32_t)getpid()};
    if (hello->version != SI_PROTOCOL_VERSION) {
      LOG("client pid %d speaks version %u, this service %u", c->pid, hello->version, SI_PROTOCOL_VERSION);
      client_close(c, "protocol version");
      return;
    }
    client_send(c, SI_EV_HELLO, &reply, sizeof(reply));
    c->ready = 1;
    for (int i = 0; i < MAX_GAMEPADS && c->fd >= 0; i++) {
      if (!gamepads[i].pad) continue;
      client_send(c, SI_EV_ADDED, &gamepads[i].added, sizeof(gamepads[i].added));
      if (c->fd >= 0) client_send(c, SI_EV_STATE, &gamepads[i].state, sizeof(gamepads[i].state));
    }
  } else if (hdr->type == SI_RUMBLE && hdr->len >= sizeof(struct si_rumble)) {
    const struct si_rumble *r = (const void *)payload;
    struct gamepad *g = find_gamepad(r->gamepad_id);
    if (!g) return;
    if (g->caps & SI_CAP_RUMBLE) SDL_RumbleGamepad(g->pad, r->low_frequency, r->high_frequency, r->duration_ms);
    if (g->caps & SI_CAP_TRIGGER_RUMBLE)
      SDL_RumbleGamepadTriggers(g->pad, r->left_trigger, r->right_trigger, r->duration_ms);
  }
}

static void client_read(struct client *c) {
  ssize_t n = read(c->fd, c->in + c->in_len, sizeof(c->in) - c->in_len);
  if (n == 0) { client_close(c, "closed"); return; }
  if (n < 0) {
    if (errno != EAGAIN && errno != EINTR) client_close(c, strerror(errno));
    return;
  }
  c->in_len += (size_t)n;
  while (c->fd >= 0 && c->in_len >= sizeof(struct si_hdr)) {
    struct si_hdr hdr;
    memcpy(&hdr, c->in, sizeof(hdr));
    if (hdr.len > SI_MAX_PAYLOAD) { client_close(c, "message too long"); return; }
    size_t total = sizeof(hdr) + hdr.len;
    if (c->in_len < total) return;
    uint8_t payload[SI_MAX_PAYLOAD] __attribute__((aligned(8)));
    memcpy(payload, c->in + sizeof(hdr), hdr.len);
    memmove(c->in, c->in + total, c->in_len - total);
    c->in_len -= total;
    handle_message(c, &hdr, payload);
  }
}

static void accept_one(int ls) {
  int fd = accept(ls, NULL, NULL);
  if (fd < 0) return;
  uid_t uid; gid_t gid;
  pid_t pid = -1;
  socklen_t pl = sizeof(pid);
  getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &pid, &pl);
  if (getpeereid(fd, &uid, &gid) || uid != getuid()) {
    LOG("rejecting peer pid %d uid %d (service uid %d)", pid, (int)uid, (int)getuid());
    close(fd);
    return;
  }
  for (int i = 0; i < MAX_CLIENTS; i++) {
    struct client *c = &clients[i];
    if (c->fd >= 0) continue;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    fcntl(fd, F_SETFL, O_NONBLOCK);
    c->fd = fd; c->pid = pid; c->ready = 0; c->in_len = c->out_len = 0;
    LOG("client pid %d accepted", pid);
    return;
  }
  LOG("rejecting peer pid %d: %d clients already", pid, MAX_CLIENTS);
  close(fd);
}

int main(int argc, char **argv) {
  const char *dir = NULL;
  for (int i = 1; i < argc; i++)
    if (!strcmp(argv[i], "--dir") && i + 1 < argc) dir = argv[++i];
  if (!dir) { fprintf(stderr, "usage: sailover-input --dir <private-dir>\n"); return 2; }
  setvbuf(stderr, NULL, _IOLBF, 0);

  static char path[128];
  int ls = make_listener(dir, path, sizeof(path));
  if (ls < 0) return 1;
  for (int i = 0; i < MAX_CLIENTS; i++) clients[i].fd = -1;
  int sigs[] = {SIGTERM, SIGINT, SIGHUP};
  for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) signal(sigs[i], on_signal);
  signal(SIGPIPE, SIG_IGN);
  pid_t parent = getppid();

  /* The game's window belongs to another process: this one never comes to the front. The Steam
   * Controller's driver is SDL's default on macOS; it is named so a later default cannot drop it. */
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_STEAM, "1");
  if (!SDL_Init(SDL_INIT_GAMEPAD)) { LOG("SDL_Init: %s", SDL_GetError()); unlink(path); return 1; }
  LOG("serving %s with SDL %d.%d.%d", path, SDL_VERSIONNUM_MAJOR(SDL_GetVersion()),
      SDL_VERSIONNUM_MINOR(SDL_GetVersion()), SDL_VERSIONNUM_MICRO(SDL_GetVersion()));

  while (!quit_signal && getppid() == parent) {
    struct pollfd fds[1 + MAX_CLIENTS];
    struct client *owner[1 + MAX_CLIENTS];
    nfds_t n = 0;
    fds[n].fd = ls; fds[n].events = POLLIN; owner[n++] = NULL;
    for (int i = 0; i < MAX_CLIENTS; i++) {
      if (clients[i].fd < 0) continue;
      fds[n].fd = clients[i].fd;
      fds[n].events = POLLIN | (clients[i].out_len ? POLLOUT : 0);
      owner[n++] = &clients[i];
    }
    if (poll(fds, n, gamepad_count() ? 1 : 50) > 0) {
      for (nfds_t i = 1; i < n; i++) {
        if (owner[i]->fd < 0) continue;
        if (fds[i].revents & POLLOUT) client_flush(owner[i]);
        if (owner[i]->fd >= 0 && fds[i].revents & (POLLIN | POLLHUP | POLLERR)) client_read(owner[i]);
      }
      if (fds[0].revents & POLLIN) accept_one(ls);
    }

    /* GameController.framework, which SDL uses for some gamepads, reports on the main run loop. */
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true);
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      if (ev.type == SDL_EVENT_GAMEPAD_ADDED) gamepad_added(ev.gdevice.which);
      else if (ev.type == SDL_EVENT_GAMEPAD_REMOVED) gamepad_removed(ev.gdevice.which);
    }
    send_changed_states();
  }

  if (quit_signal) LOG("signal %d, exiting", (int)quit_signal);
  else LOG("parent %d gone, exiting", parent);
  for (int i = 0; i < MAX_GAMEPADS; i++)
    if (gamepads[i].pad) SDL_CloseGamepad(gamepads[i].pad);
  SDL_Quit();
  unlink(path);
  return 0;
}
