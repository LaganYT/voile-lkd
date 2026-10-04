/*
 * sailover_input_protocol.h - wire format of SailOver's input service (sailover-input).
 *
 * The service reads the Mac's gamepads with SDL3 and serves them on <dir>/input.sock (AF_UNIX,
 * SOCK_STREAM, mode 0600, peers of another uid refused). Wine's SailOver bus (winebus.sys) is the
 * client: it sends HELLO, then receives HELLO, an ADDED and a STATE for every gamepad already
 * connected, and from then on ADDED, REMOVED and STATE as they happen. Several clients may be
 * connected; each receives every gamepad.
 *
 * Every message: struct si_hdr followed by `len` payload bytes (len <= SI_MAX_PAYLOAD). All fields
 * little-endian, fixed width, naturally aligned; the same layout holds for a Linux x86-64 client
 * (checked by the static asserts below). A message of an unknown type is skipped.
 *
 * A gamepad is described as SDL3 sees it, with its own vendor and product ids. Its buttons and axes
 * follow SDL3's SDL_GamepadButton and SDL_GamepadAxis order: sticks from -32768 to 32767 with Y
 * growing downwards, triggers from 0 to 32767.
 */
#ifndef SAILOVER_INPUT_PROTOCOL_H
#define SAILOVER_INPUT_PROTOCOL_H

#include <stdint.h>

#define SI_PROTOCOL_VERSION 1u
#define SI_MAX_PAYLOAD 1024u

struct si_hdr {
  uint32_t type;
  uint32_t len; /* payload bytes after this header */
};

/* client -> server */
enum {
  SI_HELLO = 1,  /* si_hello */
  SI_RUMBLE = 2, /* si_rumble: replaces the gamepad's current rumble */
};

/* server -> client */
enum {
  SI_EV_HELLO = 101,   /* si_hello */
  SI_EV_ADDED = 102,   /* si_ev_added */
  SI_EV_REMOVED = 103, /* si_gamepad */
  SI_EV_STATE = 104,   /* si_ev_state: the gamepad's whole state after a change */
};

enum { SI_BUS_UNKNOWN = 0, SI_BUS_USB = 1, SI_BUS_BLUETOOTH = 2 };
enum { SI_CAP_RUMBLE = 1u << 0, SI_CAP_TRIGGER_RUMBLE = 1u << 1 };

/* Bit n of si_ev_state.buttons is SDL_GamepadButton n. */
enum {
  SI_BUTTON_SOUTH, SI_BUTTON_EAST, SI_BUTTON_WEST, SI_BUTTON_NORTH,
  SI_BUTTON_BACK, SI_BUTTON_GUIDE, SI_BUTTON_START,
  SI_BUTTON_LEFT_STICK, SI_BUTTON_RIGHT_STICK, SI_BUTTON_LEFT_SHOULDER, SI_BUTTON_RIGHT_SHOULDER,
  SI_BUTTON_DPAD_UP, SI_BUTTON_DPAD_DOWN, SI_BUTTON_DPAD_LEFT, SI_BUTTON_DPAD_RIGHT,
  SI_BUTTON_MISC1,
  SI_BUTTON_RIGHT_PADDLE1, SI_BUTTON_LEFT_PADDLE1, SI_BUTTON_RIGHT_PADDLE2, SI_BUTTON_LEFT_PADDLE2,
  SI_BUTTON_TOUCHPAD,
  SI_BUTTON_MISC2, SI_BUTTON_MISC3, SI_BUTTON_MISC4, SI_BUTTON_MISC5, SI_BUTTON_MISC6,
  SI_BUTTON_COUNT
};

enum {
  SI_AXIS_LEFTX, SI_AXIS_LEFTY, SI_AXIS_RIGHTX, SI_AXIS_RIGHTY,
  SI_AXIS_LEFT_TRIGGER, SI_AXIS_RIGHT_TRIGGER,
  SI_AXIS_COUNT
};

struct si_hello { uint32_t version, pid; };
struct si_gamepad { uint32_t gamepad_id; };
struct si_ev_added {
  uint32_t gamepad_id; /* nonzero, not reused while the service runs */
  uint16_t vendor, product, version;
  uint16_t bus;        /* SI_BUS_* */
  uint32_t caps;       /* SI_CAP_* */
  char name[128];      /* UTF-8, NUL-terminated */
  char serial[64];     /* UTF-8, NUL-terminated, empty when the gamepad has none */
};
struct si_ev_state {
  uint32_t gamepad_id;
  uint32_t buttons;              /* 1u << SI_BUTTON_* */
  int16_t axes[SI_AXIS_COUNT];   /* SI_AXIS_* */
};
struct si_rumble {
  uint32_t gamepad_id;
  uint16_t low_frequency, high_frequency; /* the two motors, 0 to 65535 */
  uint16_t left_trigger, right_trigger;   /* with SI_CAP_TRIGGER_RUMBLE */
  uint32_t duration_ms;                   /* 0 stops */
};

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define SI_ASSERT_SIZE(t, n) _Static_assert(sizeof(struct t) == (n), #t " layout")
#elif defined(__cplusplus)
#define SI_ASSERT_SIZE(t, n) static_assert(sizeof(struct t) == (n), #t " layout")
#else
#define SI_ASSERT_SIZE(t, n)
#endif
SI_ASSERT_SIZE(si_hdr, 8);
SI_ASSERT_SIZE(si_hello, 8);
SI_ASSERT_SIZE(si_gamepad, 4);
SI_ASSERT_SIZE(si_ev_added, 208);
SI_ASSERT_SIZE(si_ev_state, 20);
SI_ASSERT_SIZE(si_rumble, 16);

#endif
