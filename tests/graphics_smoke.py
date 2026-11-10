import sys
sys.dont_write_bytecode = True
"""Exercise the real GPU queue and scanout, using only a disposable data disk."""
import argparse
import http.server
import threading
import os
import pathlib
import re
import shutil
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument('--backend', choices=('gpu', 'std', 'virtio'), default='gpu')
parser.add_argument('--reuse', action='store_true', help='reuse this test disk/account for a local rerun')
parser.add_argument('--build', default='build/graphics-test', help='isolated build directory')
args = parser.parse_args()
b = pathlib.Path(args.build)
make = ['make', 'BUILD=' + str(b)]
subprocess.run(make + [str(b / 'os.img'), str(b / 'myfs'), str(b / 'user/gpucheck'), str(b / 'user/desktop'), str(b / 'user/gpuscene')], check=True, stdout=subprocess.DEVNULL)
image = b / ('graphics-' + args.backend + '.img')
if not args.reuse:
    with image.open('wb') as f:
        f.truncate(16 * 1024 * 1024)
    subprocess.run(make + ['-j4', 'install', 'DISK=' + str(image)], check=True, stdout=subprocess.DEVNULL)
else:
    subprocess.run([str(b / 'myfs'), str(image), 'put', str(b / 'user/desktop'), '/bin/desktop'], check=True, stdout=subprocess.DEVNULL)
subprocess.run([str(b / 'myfs'), str(image), 'rm', '/etc/services/enabled/desktop.service'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
subprocess.run([str(b / 'myfs'), str(image), 'put', str(b / 'user/gpucheck'), '/bin/gpucheck'], check=True, stdout=subprocess.DEVNULL)
subprocess.run([str(b / 'myfs'), str(image), 'put', str(b / 'user/gpuscene'), '/bin/gpuscene'], check=True, stdout=subprocess.DEVNULL)
server = None
requests = []
if args.backend == 'gpu':
    page = ('<title>GPU browser smoke</title><style>body{margin:20px;background:#f5f5f5}'
            '.cards{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:16px}'
            'article{padding:20px;background:#ff8a20;color:white;border-radius:8px}</style>'
            '<h1>GPU browser smoke</h1><div class="cards">' +
            ''.join('<article><h2>Service %d</h2><p>Retained GPU window texture</p></article>' % i for i in range(60)) + '</div>').encode()
    class Page(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args): pass
        def do_GET(self):
            requests.append(self.path)
            self.send_response(200); self.send_header('Content-Type', 'text/html')
            self.send_header('Content-Length', str(len(page))); self.end_headers(); self.wfile.write(page)
    server = http.server.ThreadingHTTPServer(('0.0.0.0', 0), Page)
    threading.Thread(target=server.serve_forever, daemon=True).start()
script = b / 'graphics-desktop.sh'
script.write_text('desktop &\nsleep 2\nbrowser -v http://10.0.2.2:%d/\n' % (server.server_port if server else 1))
subprocess.run([str(b / 'myfs'), str(image), 'put', str(script), '/graphics-desktop.sh'], check=True, stdout=subprocess.DEVNULL)
log = b / ('graphics-' + args.backend + '.log')
log.write_text('')
env = dict(os.environ)
if args.backend == 'gpu':
    import sys
    sys.path.insert(0, str(pathlib.Path('tools').resolve()))
    from run_qemu import select
    video, env, reason = select('qemu-system-i386', 'on')
    print(reason, flush=True)
else:
    video = ['-vga', args.backend, '-display', 'none']
boot = b / ('graphics-' + args.backend + '-boot.img')
shutil.copyfile(b / 'os.img', boot)
p = subprocess.Popen(['qemu-system-i386', '-smp', '4', '-m', '256M', *video, '-nic', 'user,model=e1000',
    '-drive', f'format=raw,file={boot},index=0', '-drive', f'format=raw,file={image},index=1',
    '-serial', f'file:{log}', '-monitor', 'stdio', '-no-reboot'], env=env, stdin=subprocess.PIPE,
    stdout=subprocess.DEVNULL, stderr=(b / ('graphics-' + args.backend + '.err')).open('w'), text=True)

def output():
    # Concurrent reads of a QEMU log on WSL/DrvFS can transiently return ENODATA.
    for attempt in range(5):
        try:
            text = log.read_text(errors='replace')
            return re.sub(r'\x1b\[[0-9;?]*[A-Za-z]', '', text).replace('\r', '')
        except OSError as exc:
            if exc.errno != 61 or attempt == 4: raise
            time.sleep(.05)


milestones = {}

def wait(marker):
    end = time.monotonic() + (120 if marker == 'GPUSCENE PASS' else 35)
    while time.monotonic() < end:
        data = output()
        for name in ('SOFTWARE BEGIN', 'SOFTWARE END', 'GPU BEGIN', 'GPU END'):
            if 'GPUSCENE ' + name in data and name not in milestones: milestones[name] = time.monotonic()
        if 'GPUSCENE FAIL' in data or 'GPUCHECK FAIL' in data or 'PAGE FAULT' in data or 'PANIC' in data:
            raise AssertionError(data[-2500:])
        if marker in data:
            return
        if p.poll() is not None:
            raise AssertionError('QEMU exited\n' + data[-2500:])
        time.sleep(.1)
    raise AssertionError('waiting for ' + marker + '\n' + output()[-2500:])

def monitor(command):
    p.stdin.write(command + '\n'); p.stdin.flush()

def key(k):
    monitor('sendkey ' + k + ' 40'); time.sleep(.10)

def send(text):
    for ch in text:
        key({'\n': 'ret', '/': 'slash', ' ': 'spc', '-': 'minus', '.': 'dot'}.get(ch, ch))

def shot(name):
    path = b / ('graphics-' + args.backend + '-' + name + '.ppm')
    if path.exists(): path.unlink()
    if args.backend == 'gpu':
        match = re.search(r'GPUCHECK PIXELS ' + name.upper() + r' ([0-9 ]+)\n', output())
        assert match, 'missing GPU readback: ' + name
        values = list(map(int, match.group(1).split()))
        w, h, hash_value = values[:3]
        points = [(10,10), (50,50), (70,70), (120,120), (220,120),
                  (58,58), (78,78), (108,108), (128,128), (100,100), (50,90), (w-1,h-1)]
        assert len(values) == 3 + len(points), values
        samples = dict(zip(points, values[3:]))
        return w, h, {'hash': hash_value, 'samples': samples}
    else:
        monitor('screendump ' + str(path))
    deadline = time.monotonic() + 10
    while not path.exists() or path.stat().st_size < 100:
        assert time.monotonic() < deadline, 'screendump timed out'
        time.sleep(.1)
    time.sleep(.2)
    parts = path.read_bytes().split(maxsplit=4)
    assert parts[0] == b'P6' and parts[3] == b'255'
    w, h, data = int(parts[1]), int(parts[2]), parts[4]
    assert len(data) == w * h * 3
    return w, h, data

def pixel(frame, x, y):
    w, h, data = frame
    if isinstance(data, dict):
        p = data['samples'][x,y]
        return p >> 16 & 255, p >> 8 & 255, p & 255
    return tuple(data[(y * w + x) * 3:(y * w + x) * 3 + 3])

def close_to(actual, expected, tolerance=2):
    assert max(abs(a - b) for a, b in zip(actual, expected)) <= tolerance, (actual, expected)

try:
    if not args.reuse:
        wait('New password:'); send('port123\n')
        wait('Confirm password:'); send('port123\n')
    wait('Login [root]:'); send('\n')
    wait('Password:'); send('port123\n')
    wait('root:/'); send('gpucheck\n')
    if args.backend != 'gpu':
        wait('GPUCHECK SOFTWARE')
        frame = shot('software')
        assert frame[:2] == (800, 600)
        close_to(pixel(frame, 50, 50), (200, 40, 20), 0)
        close_to(pixel(frame, 799, 599), (20, 40, 60), 0)
        key('n'); wait('GPUCHECK PASS software')
    else:
        wait('GPUCHECK COMPOSITE')
        frame = shot('composite')
        assert frame[:2] == (800, 600)
        close_to(pixel(frame, 10, 10), (20, 40, 60), 0)
        close_to(pixel(frame, 50, 50), (200, 40, 20), 0)
        close_to(pixel(frame, 70, 70), (100, 120, 60))
        close_to(pixel(frame, 120, 120), (110, 70, 30))
        close_to(pixel(frame, 220, 120), (10, 120, 80))
        close_to(pixel(frame, 50, 90), (40, 20, 200), 0)
        key('n'); wait('GPUCHECK HIDDEN')
        hidden = shot('hidden')
        assert hidden == frame, 'unfinished GPU frame became visible'
        key('n'); wait('GPUCHECK COMMITTED')
        committed = shot('committed')
        close_to(pixel(committed, 220, 120), (200, 40, 20), 0)
        key('n'); wait('GPUCHECK COPY')
        copied = shot('copy')
        for x, y in ((50, 50), (70, 70), (100, 100), (120, 120)):
            close_to(pixel(copied, x + 8, y + 8), pixel(committed, x, y), 0)
        key('n'); wait('GPUCHECK RESIZED')
        resized = shot('resized')
        assert resized[:2] == (1280, 720)
        close_to(pixel(resized, 1279, 719), (40, 20, 200), 0)
        key('n'); wait('GPUCHECK PASS GPU')
        time.sleep(.3); key('ctrl-u'); send('gpuscene\n'); wait('GPUSCENE PASS')
        print(re.search(r'GPUSCENE BENCH[^\n]+', output()).group(0), flush=True)
        print('GPUSCENE HOST WALL: software %.3f s, GPU %.3f s' % (
            milestones['SOFTWARE END'] - milestones['SOFTWARE BEGIN'],
            milestones['GPU END'] - milestones['GPU BEGIN']), flush=True)
        # Ensure the actual desktop, including ARGB decorations, runs afterward.
        time.sleep(.3); key('ctrl-u')
        send('sh /graphics-desktop.sh\n'); time.sleep(7)
        wait('desktop: GPU/VirGL compositor'); wait('desktop: independent hardware cursor'); wait('Done:')
        assert '/' in requests, requests
        key('pgdn'); time.sleep(.3); key('pgup'); time.sleep(.3)
        assert 'GPU surface failed' not in output(), output()[-2500:]
        assert 'PAGE FAULT' not in output() and 'PANIC' not in output()
    print('Graphics ' + args.backend + ': PASS', flush=True)
finally:
    p.terminate()
    try: p.wait(timeout=5)
    except subprocess.TimeoutExpired: p.kill(); p.wait()
    if server: server.shutdown()
