#!/usr/bin/env python3
"""Check OWNERS.md against the generated syscall table.

Fails (exit 1) when a name is owned by two modules or is not a Linux x86-64 syscall name.
Prints the syscalls nobody owns (they answer -ENOSYS today) with --list-unowned.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def table_names():
    names = set()
    with open(os.path.join(ROOT, "tools", "data", "syscall_64.tbl.v6.12")) as f:
        for line in f:
            p = line.split()
            if len(p) >= 3 and p[0].isdigit() and p[1] in ("common", "64"):
                names.add(p[2])
    return names


def main():
    text = open(os.path.join(ROOT, "OWNERS.md")).read()
    m = re.search(r"<!-- owners:begin -->\n(.*?)<!-- owners:end -->", text, re.S)
    if not m:
        print("OWNERS.md: owners block not found")
        return 1
    owner = {}
    bad = 0
    names = table_names()
    for line in m.group(1).splitlines():
        if not line.strip():
            continue
        mod, _, rest = line.partition(":")
        for n in rest.split():
            if n not in names:
                print("OWNERS.md: %s lists unknown syscall %s" % (mod, n))
                bad += 1
            elif n in owner:
                print("OWNERS.md: %s is owned by both %s and %s" % (n, owner[n], mod))
                bad += 1
            else:
                owner[n] = mod
    unowned = sorted(names - set(owner))
    if "--list-unowned" in sys.argv:
        print("unowned (%d): %s" % (len(unowned), " ".join(unowned)))
    print("owners_check: %d syscalls owned, %d unowned, %d problems" % (len(owner), len(unowned), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
