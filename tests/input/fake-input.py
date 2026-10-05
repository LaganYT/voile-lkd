#!/usr/bin/env python3
"""Stands in for obelisk-input (../../input): serves one scripted gamepad on DIR/input.sock and logs what clients
send. With DIR bound to the guest's /run/obelisk, xinputtest.exe sees the gamepad through Wine's Obelisk bus.

Usage: fake-input.py DIR SECONDS. Every 3 s cycle: A down then up, left stick right, D-pad up, right trigger,
guide; the 4th cycle unplugs and replugs the gamepad.
"""
import os, select, socket, struct, sys, time

DIR, SECONDS = sys.argv[1], float(sys.argv[2])
PATH = os.path.join(DIR, "input.sock")
T0 = time.time()

def log(*a):
    print(f"[{time.time() - T0:7.3f}] fake-input:", *a, flush=True)

def msg(kind, payload):
    return struct.pack("<II", kind, len(payload)) + payload

GAMEPAD = 7
def added():
    name = b"Steam Controller".ljust(128, b"\0")
    serial = b"FAKE0001".ljust(64, b"\0")
    return msg(102, struct.pack("<IHHHHI", GAMEPAD, 0x28DE, 0x1302, 0x0307, 1, 1) + name + serial)

class state:
    """Built when sent, for the gamepad id of that moment."""
    def __init__(self, buttons=0, axes=(0, 0, 0, 0, 0, 0)):
        self.buttons, self.axes = buttons, axes
    def bytes(self):
        return msg(104, struct.pack("<II6h", GAMEPAD, self.buttons, *self.axes))

STEPS = [
    (0.0, state(1 << 0)),                                # A down
    (0.3, state()),                                      # A up
    (0.6, state(axes=(32767, 0, 0, 0, 0, 0))),           # left stick right
    (0.9, state(axes=(0, -32768, 0, 0, 0, 0))),          # left stick up (SDL: Y negative is up)
    (1.2, state(1 << 11)),                               # D-pad up
    (1.5, state(axes=(0, 0, 0, 0, 0, 32767))),           # right trigger
    (1.8, state(1 << 5)),                                # guide
    (2.1, state()),
]

os.makedirs(DIR, mode=0o700, exist_ok=True)
if os.path.exists(PATH):
    os.unlink(PATH)
server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
server.bind(PATH)
server.listen(8)
clients, buffers = [], {}
cycle_start, cycle, plugged = None, 0, True

def broadcast(data):
    for c in list(clients):
        try:
            c.sendall(data)
        except OSError as e:
            log("client lost", e)
            clients.remove(c)

next_step = 0
while time.time() - T0 < SECONDS:
    readable, _, _ = select.select([server] + clients, [], [], 0.01)
    for s in readable:
        if s is server:
            c, _ = server.accept()
            clients.append(c)
            buffers[c] = b""
            log("client connected")
            continue
        data = s.recv(4096)
        if not data:
            log("client closed")
            clients.remove(s)
            continue
        buffers[s] += data
        while len(buffers[s]) >= 8:
            kind, size = struct.unpack("<II", buffers[s][:8])
            if len(buffers[s]) < 8 + size:
                break
            payload, buffers[s] = buffers[s][8:8 + size], buffers[s][8 + size:]
            if kind == 1:
                version, pid = struct.unpack("<II", payload[:8])
                log(f"HELLO version {version} pid {pid}")
                s.sendall(msg(101, struct.pack("<II", 1, os.getpid())))
                if plugged:
                    s.sendall(added() + state().bytes())
                if cycle_start is None:
                    cycle_start = time.time() + 2
            elif kind == 2:
                gid, low, high, lt, rt, ms = struct.unpack("<IHHHHI", payload[:16])
                log(f"RUMBLE gamepad {gid} low {low} high {high} triggers {lt}/{rt} for {ms} ms")
            else:
                log("unknown message", kind)
    if cycle_start is None or time.time() < cycle_start:
        continue
    t = time.time() - cycle_start
    if next_step < len(STEPS) and t >= STEPS[next_step][0]:
        if plugged:
            broadcast(STEPS[next_step][1].bytes())
        next_step += 1
    if t >= 3.0:
        cycle += 1
        cycle_start, next_step = time.time(), 0
        if cycle == 4:
            log("unplugging the gamepad")
            broadcast(msg(103, struct.pack("<I", GAMEPAD)))
            plugged = False
        elif cycle == 5:
            log("plugging it back as a new gamepad")
            GAMEPAD += 1
            broadcast(added() + state().bytes())
            plugged = True
log("done")
