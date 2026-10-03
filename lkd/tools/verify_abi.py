#!/usr/bin/env python3
"""Compare every numeric LX_<NAME> constant of include/lkd/abi_x64.h with the Linux v6.12 uapi header
define of <NAME> (tools/data/uapi-v6.12/, downloaded from github.com/torvalds/linux at tag v6.12).

Exit status 1 on any mismatch. Constants without a #define counterpart (enums, function-like macros,
LKD-specific values) are counted as unverified and listed with --list-unverified.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UAPI = os.path.join(ROOT, "tools", "data", "uapi-v6.12")
M64 = (1 << 64) - 1
M32 = (1 << 32) - 1

DEF = re.compile(r"^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)\s+(.+?)\s*$")


def strip_comments(s):
    s = re.sub(r"/\*.*?\*/", " ", s, flags=re.S)
    return re.sub(r"//[^\n]*", " ", s)


def join_continuations(s):
    return re.sub(r"\\\n", " ", s)


def load_defs(paths):
    defs = {}
    ambiguous = set()
    for p in paths:
        txt = join_continuations(strip_comments(open(p, errors="replace").read()))
        for line in txt.splitlines():
            m = DEF.match(line)
            if not m:
                continue
            name, val = m.group(1), m.group(2)
            if "(" in name or re.match(r"^\(", val) is None and re.match(r"^[A-Za-z_]\w*\(", name):
                continue
            if name in defs and defs[name] != val:
                ambiguous.add(name)
            defs.setdefault(name, val)
    return defs, ambiguous


def to_py(expr):
    expr = expr.strip()
    # octal 0777 -> 0o777, decimal/hex suffixes dropped
    expr = re.sub(r"\b0[xX]([0-9a-fA-F]+)[uUlL]*\b", lambda m: "0x" + m.group(1), expr)
    expr = re.sub(r"\b0([0-7]+)[uUlL]*\b", lambda m: "0o" + m.group(1), expr)
    expr = re.sub(r"\b([0-9]+)[uUlL]+\b", lambda m: m.group(1), expr)
    return expr


class Eval:
    def __init__(self, defs, prefix=""):
        self.defs = defs
        self.cache = {}
        self.busy = set()
        self.prefix = prefix

    def value(self, name):
        if name in self.cache:
            return self.cache[name]
        if name in self.busy or name not in self.defs:
            raise KeyError(name)
        self.busy.add(name)
        try:
            v = self.expr(self.defs[name])
        finally:
            self.busy.discard(name)
        self.cache[name] = v
        return v

    def expr(self, text):
        text = to_py(text)
        if re.search(r"[{};]|sizeof|\(\w+\s*\*\)|_IO|_BITUL|_AC|__u|long|int\b", text):
            raise ValueError("unsupported: " + text)

        def repl(m):
            return str(self.value(m.group(0)))
        text = re.sub(r"\b[A-Za-z_][A-Za-z0-9_]*\b", lambda m: m.group(0) if m.group(0).startswith(("0x", "0o")) else repl(m), text)
        return eval(text, {"__builtins__": {}}, {})


ALT = {"WNOTHREAD": "__WNOTHREAD", "WALL": "__WALL", "WCLONE": "__WCLONE"}
# uapi NSIG is the legacy 32; the kernel _NSIG (64, arch/x86/include/asm/signal.h) is not a uapi header.
SKIP = {"NSIG"}


def main():
    paths = []
    for dp, _, fn in os.walk(UAPI):
        for f in fn:
            if f.endswith(".h"):
                paths.append(os.path.join(dp, f))
    kdefs, ambiguous = load_defs(sorted(paths))
    mine, _ = load_defs([os.path.join(ROOT, "include", "lkd", "abi_x64.h")])
    kev = Eval(kdefs)
    mev = Eval(mine)
    ok = bad = 0
    unverified = []
    for name in sorted(mine):
        if not name.startswith("LX_"):
            continue
        base = name[3:]
        if base in SKIP:
            unverified.append(name + " (checked by hand: _NSIG = 64)")
            continue
        try:
            mv = mev.value(name)
        except Exception:
            unverified.append(name + " (mine not evaluable)")
            continue
        cands = [ALT.get(base, ""), base, "__" + base]
        kname = next((c for c in cands if c and c in kdefs), None)
        if kname is None:
            unverified.append(name)
            continue
        if kname in ambiguous:
            unverified.append(name + " (ambiguous in uapi)")
            continue
        try:
            kv = kev.value(kname)
        except Exception:
            unverified.append(name + " (kernel value not evaluable)")
            continue
        if isinstance(mv, int) and isinstance(kv, int) and ((mv & M64) == (kv & M64) or (mv & M32) == (kv & M32)):
            ok += 1
        else:
            bad += 1
            print("MISMATCH %s = %r, Linux %s = %r" % (name, mv, kname, kv))
    print("verify_abi: %d verified, %d mismatches, %d unverified" % (ok, bad, len(unverified)))
    if "--list-unverified" in sys.argv:
        print("\n".join(unverified))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
