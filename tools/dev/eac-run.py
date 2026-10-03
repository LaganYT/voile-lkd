#!/usr/bin/env python3
"""Authenticated Rocket League launch (EAC bootstrapper) on fexdarwin + LKD + our Linux Wine, with the wsd window server.

Usage: run/eac-run.py SECONDS [--exe RocketLeague_EAC.exe|Launcher.exe]

Secrets: the Epic session in EXP/.secrets is turned into a fresh launch plan by src/epic-launch (exchange code inside,
single use). No argument value is ever printed; every captured output is redacted of all plan values before it is
written to EXP/evidence/eac-runs/<stamp>/. The original game install is only read (receipt for the plan, hashes before
and after); the guest runs the private APFS clone EXP/run/game-clone.
"""
import json, os, pathlib, re, shutil, signal, subprocess, sys, time

EXP = pathlib.Path(__file__).resolve().parents[1]
SECRETS = EXP / '.secrets'
ORIG = pathlib.Path.home() / 'Library/Application Support/com.zagrios.sailover/Games/Epic/epic-52edd447ec86748fc2afecc6cd67acc7748a5461-Windows'
CLONE = EXP / 'run' / 'game-clone'
HL = EXP.parent / 'rocket-hyperlinux-20260928'
HOST = os.environ.get('HOSTRUN', str(EXP / 'build-lead' / 'host' / 'fexdarwin-run'))
TO = str(EXP / 'src' / 'fexdarwin' / 'scripts' / 'run-with-timeout.sh')
WSD = EXP / 'src' / 'wsd' / 'bin' / 'wsd'
WSD_DIR = EXP / 'run' / 'wsd-eac'
PREFIX_HOST = EXP / 'run' / 'home-user-own' / 'pfx-eac'

secs = int(sys.argv[1]) if len(sys.argv) > 1 else 150
exe = 'RocketLeague_EAC.exe'
if '--exe' in sys.argv:
    exe = sys.argv[sys.argv.index('--exe') + 1]
stamp = time.strftime('%Y%m%dT%H%M%S')
OUT = EXP / 'evidence' / 'eac-runs' / stamp
OUT.mkdir(parents=True, exist_ok=True)


def sha(p):
    return subprocess.run(['shasum', '-a', '256', str(p)], capture_output=True, text=True).stdout.split()[0]


def hashes(tag):
    rows = [f'{sha(ORIG / "Binaries/Win64" / n)}  original/{n}' for n in ('RocketLeague.exe', 'RocketLeague_EAC.exe')]
    (OUT / f'hashes-{tag}.txt').write_text('\n'.join(rows) + '\n')
    return rows


before = hashes('before')
expected = {'ca3a9075422f6c79c0cd3ef8c4edd5c9ae1cc48f51b1077d8e18fb495f9738fa', '903e7aba292448d68976597efe3f0dc6f5cc66ef1889e3c1efcaf3eaf4e614d1'}
if {r.split()[0] for r in before} != expected:
    sys.exit('original game hashes differ from the recorded ones: stop')

# 1. Fresh launch plan (prints only argument names).
p = subprocess.run([str(EXP / 'src/epic-launch/target/release/epic-launch'), str(SECRETS), str(ORIG), 'en'], capture_output=True, text=True, timeout=60)
(OUT / 'epic-launch.txt').write_text(p.stdout + p.stderr)
if p.returncode != 0:
    sys.exit('epic-launch failed, see ' + str(OUT / 'epic-launch.txt'))
plan = json.loads((SECRETS / 'launch-plan.json').read_text())
args = [a for a in plan['arguments'] if not (a.startswith('/') and a.lower().endswith('.exe'))]
secrets = sorted({v for a in plan['arguments'] for v in ([a.split('=', 1)[1]] if '=' in a else []) if len(v) >= 4}, key=len, reverse=True)
if plan.get('has_ownership_token'):
    # Ownership token file: put it in the private prefix and point -epicovt at its Windows path.
    shutil.copyfile(SECRETS / 'ownership.bin', PREFIX_HOST / 'drive_c' / 'ownership.bin')
    os.chmod(PREFIX_HOST / 'drive_c' / 'ownership.bin', 0o600)
    args = [('-epicovt=C:\\ownership.bin' if a.startswith('-epicovt=') else a) for a in args]


def redact(text):
    for v in secrets:
        text = text.replace(v, '<redacted>')
    return re.sub(r'(?i)(AUTH_PASSWORD=|refresh_token["\':= ]+|access_token["\':= ]+|exchange_code["\':= ]+)[A-Za-z0-9._\-]+', r'\1<redacted>', text)


# 2. Window server (one instance, its own socket directory, killed at the end).
WSD_DIR.mkdir(exist_ok=True)
os.chmod(WSD_DIR, 0o700)  # wsd refuses a socket directory other users could reach
wsd_log = open(OUT / 'wsd.log', 'wb')
wsd = subprocess.Popen([TO, str(secs + 30), str(WSD), '--dir', 'wsd-eac'], stdout=wsd_log, stderr=subprocess.STDOUT, cwd=EXP / 'run',
                       start_new_session=True)  # its own group: killing it must never reach this script
time.sleep(1.5)

# 2b. Neptune D3D path (M5): our d3d11/dxgi/d3d12 PE DLLs in the prefix (native override) and one vtest renderer host
# (virgl_test_server, NPT_BACKEND=dxmt) on a relative socket in the wsd directory, killed at the end.
NEP_BUILD = EXP / 'src/wine-linux/neptune/build-win64/src/virtio/neptune'
for dll in ('d3d11.dll', 'dxgi.dll', 'd3d12.dll'):
    shutil.copyfile(NEP_BUILD / dll, PREFIX_HOST / 'drive_c/windows/system32' / dll)
G = EXP / 'src/graphics-spike'
DXMT = G / 'third_party/utm-5.0.5/dxmt-native.framework/Versions/A/dxmt-native'
for stale in (WSD_DIR / 'vtest.sock',):
    if stale.exists():
        stale.unlink()
renderer_env = dict(os.environ, RENDER_SERVER_EXEC_PATH=str(G / 'prefix/libexec/virgl_render_server'), NPT_BACKEND='dxmt',
                    NPT_D3D11_LIBRARY_PATH=str(DXMT), NPT_DXGI_LIBRARY_PATH=str(DXMT), NPT_D3D12_LIBRARY_PATH=str(DXMT))
renderer_log = open(OUT / 'renderer.log', 'wb')
renderer = subprocess.Popen([TO, str(secs + 30), str(G / 'prefix/bin/virgl_test_server'), '--no-virgl', '--no-fork',
                             '--socket-path', 'wsd-eac/vtest.sock'], stdout=renderer_log, stderr=subprocess.STDOUT,
                            cwd=EXP / 'run', env=renderer_env, start_new_session=True)
for _ in range(50):
    if (WSD_DIR / 'vtest.sock').exists():
        break
    time.sleep(0.1)

# 3. The guest.
env = dict(os.environ)
env.update(FEXDARWIN_LKD='1', FEXDARWIN_ROOT=str(HL / 'linux-root/rootfs'), FEXDARWIN_ROOT_RO='1',
           FEXDARWIN_BIND=';'.join([f'/opt/wine={EXP}/src/wine-linux/install:ro', f'/home/user={EXP}/run/home-user-own',
                                    f'/games/rl={CLONE}', f'/run/sailover={WSD_DIR}']))
guest_env = ['HOME=/home/user', 'USER=user', 'LOGNAME=user', 'PATH=/opt/wine/bin:/usr/bin:/bin', 'LANG=en_US.UTF-8',
             'WINEPREFIX=/home/user/pfx-eac', 'WINEARCH=win64', 'WINEDEBUG=' + os.environ.get('GUEST_WINEDEBUG', '-all,err+all'), 'WINEDLLOVERRIDES=winemenubuilder.exe=d;d3d11,dxgi,d3d12=n',
             'VTEST_SOCKET_NAME=/run/sailover/vtest.sock', 'NPT_DEBUG=force_vtest']
# Test switches of our own components only (driver, Neptune): GUEST_EXTRA_ENV="WINE_SAILOVER_LATENCY=1;NPT_X=y".
for e in filter(None, os.environ.get('GUEST_EXTRA_ENV', '').split(';')):
    if not e.startswith(('WINE_SAILOVER_', 'NPT_')):
        sys.exit(f'GUEST_EXTRA_ENV: {e!r} is not a WINE_SAILOVER_* or NPT_* switch')
    guest_env.append(e)
argv = [TO, str(secs), HOST, '--cwd', '/games/rl/Binaries/Win64']
for e in guest_env:
    argv += ['--env', e]
# wineserver writes the registry only when it exits (about 3 s after its last client): wait for it inside the guest,
# otherwise the timeout wrapper kills it and the prefix never keeps its registry.
argv += ['/bin/sh', '-c', '/opt/wine/bin/wine "$@"; rc=$?; /opt/wine/bin/wineserver -w; exit $rc', 'sh', f'/games/rl/Binaries/Win64/{exe}'] + args
t0 = time.time()
raw_path = OUT / 'guest.raw.log'
with open(raw_path, 'wb') as raw_f:  # straight to disk: a looping trace can be gigabytes
    g = subprocess.run(argv, env=env, stdout=raw_f, stderr=subprocess.STDOUT, timeout=secs + 60)
elapsed = time.time() - t0
# Keep the first 300000 and last 10000 lines, redacted; the raw file never leaves this machine and is deleted.
from collections import deque
head, tail, total = [], deque(maxlen=10000), 0
with open(raw_path, 'r', errors='replace') as rf:
    for line in rf:
        total += 1
        if total <= 300000:
            head.append(line)
        else:
            tail.append(line)
body = ''.join(head) + (f'... {total - 300000 - len(tail)} lines dropped ...\n' if total > 300000 + len(tail) else '') + ''.join(tail)
(OUT / 'guest.log').write_text(redact(body))
raw_path.unlink()

# 4. Tear down and collect.
# run-with-timeout.sh puts the real server in its own process group and kills that group on SIGALRM: SIGTERM to the
# wrapper would only kill perl and leave the server running.
def stop(proc):
    try:
        proc.send_signal(signal.SIGALRM)
        proc.wait(timeout=10)
    except Exception:
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except Exception:
            pass


stop(wsd)
wsd.wait(timeout=30)
wsd_log.close()
stop(renderer)
subprocess.run(['pkill', '-f', 'virgl_render_server'], capture_output=True)
renderer_log.close()
(OUT / 'renderer.log').write_text(redact((OUT / 'renderer.log').read_text(errors='replace')))
for root in (PREFIX_HOST / 'drive_c', CLONE):
    for f in root.rglob('*'):
        try:
            if f.is_file() and f.stat().st_mtime >= t0 - 1 and f.suffix.lower() in ('.log', '.txt', '.json') and f.stat().st_size < 4 << 20:
                rel = f.relative_to(root.parent if root == CLONE else PREFIX_HOST)
                dst = OUT / 'files' / rel
                dst.parent.mkdir(parents=True, exist_ok=True)
                dst.write_text(redact(f.read_text(errors='replace')))
        except Exception:
            pass
after = hashes('after')
left = subprocess.run(['pgrep', '-f', 'fexdarwin-run'], capture_output=True).stdout.count(b'\n')
summary = dict(stamp=stamp, exe=exe, rc=g.returncode, seconds=round(elapsed, 1), argument_count=len(args),
               argument_names=[a.split('=')[0] for a in args], hashes_unchanged=before == after, leftover_guest_processes=left)
(OUT / 'summary.json').write_text(json.dumps(summary, indent=1))
print(json.dumps(summary))
print('evidence:', OUT)
