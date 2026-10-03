#!/usr/bin/env python3
"""Runs the x86-64 probes of the earlier HyperLinux campaign (EAC ptrace/proc contract, expected values from real Linux)
under fexdarwin in LKD mode, one at a time, read-only, with a timeout. Usage: run-probes.py [HOST] [filter]"""
import json, os, re, shutil, subprocess, sys, tempfile, time, pathlib
EXP = pathlib.Path(__file__).resolve().parents[2]
PROBES = EXP.parent / 'rocket-hyperlinux-20260928' / 'probes'
HOST = sys.argv[1] if len(sys.argv) > 1 and sys.argv[1] else str(EXP / 'build-lead' / 'host' / 'fexdarwin-run')
FILT = sys.argv[2] if len(sys.argv) > 2 else ''
TO = str(EXP / 'src' / 'fexdarwin' / 'scripts' / 'run-with-timeout.sh')
OUT = EXP / 'evidence' / 'eac-probes' / time.strftime('%Y%m%dT%H%M%S'); OUT.mkdir(parents=True, exist_ok=True)
summary = []

def run_one(name, p, t0):
    (OUT / f'{name}.stdout').write_bytes(p.stdout); (OUT / f'{name}.stderr').write_bytes(p.stderr)
    checks = []
    for line in p.stdout.decode(errors='replace').splitlines():
        try: e = json.loads(line)
        except Exception: continue
        if e.get('event') == 'check': checks.append(e)
    fails = [c['name'] for c in checks if not c.get('pass')]
    left = int(subprocess.run(['pgrep', '-f', 'fexdarwin-run'], capture_output=True).stdout.count(b'\n'))
    row = dict(probe=name, rc=p.returncode, checks=len(checks), fail=len(fails), first_fails=fails[:6], secs=round(time.time()-t0, 1), leftover=left)
    summary.append(row); print(json.dumps(row), flush=True)

for d in sorted(p.parent for p in PROBES.glob('*/probe-x86_64')):
    if FILT and FILT not in d.name: continue
    runner = (d / 'run-hl.py').read_text() if (d / 'run-hl.py').exists() else ''
    m = re.search(r"--mode', default='([a-z-]+)'", runner); mode = m.group(1) if m else None
    target = 'target-flags-x86_64' if "'target-flags-' + args.arch" in runner else ('target-x86_64' if (d / 'target-x86_64').exists() else None)
    # The probe's own argv as run-hl.py builds it (after str(probe)): a target only when it passes str(target), the
    # mode only when it passes args.mode, the phase of a phased runner ('pre' then 'post' are separate runs).
    am = re.search(r"argv = \[[^\]]*?str\(probe\)\s*,?([^\]]*)\]", runner)
    tail = am.group(1) if am else 'str(target), args.mode'
    phases = [None]
    if 'phase' in tail and 'choices=("pre", "post")' in runner: phases = ['pre', 'post']
    for phase in phases:
        args_ = (['/' + target] if target and 'target' in tail else []) + ([mode] if mode and 'args.mode' in tail else []) + ([phase] if phase else [])
        # A writable private copy of the probe directory: probes create owned marker files in their working
        # directory (run-hl.py runs them in a fresh 'work' directory); the guest cwd is '/'.
        work = pathlib.Path(tempfile.mkdtemp(prefix='eacprobe-'))
        for f in d.iterdir():
            if f.is_file() and (f.name.startswith('probe-') or f.name.startswith('target')) and not f.name.endswith('.o'): shutil.copy2(f, work / f.name)
        argv = [TO, '40', HOST, '/probe-x86_64'] + args_
        env = dict(os.environ, FEXDARWIN_LKD='1', FEXDARWIN_ROOT=str(work), FEXDARWIN_ROOT_RO='0')
        if phase: env['HL_POKE_TEST_DELAY'] = phase
        name = d.name + ('-' + phase if phase else '')
        t0 = time.time()
        p = subprocess.run(argv, env=env, capture_output=True, timeout=120)
        shutil.rmtree(work, ignore_errors=True)
        run_one(name, p, t0)
(OUT / 'summary.json').write_text(json.dumps(summary, indent=1))
tot = sum(r['checks'] for r in summary); bad = sum(r['fail'] for r in summary)
print(f'TOTAL probes={len(summary)} checks={tot} failed={bad} out={OUT}')
