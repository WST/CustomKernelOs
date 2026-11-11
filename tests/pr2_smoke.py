"""Native regressions for protected rename, safe download and delayed HTTP."""
import http.server
import os
import pathlib
import re
import subprocess
import threading
import time

class Server(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args): pass
    def do_GET(self):
        body = b'partial' if self.path == '/short' else b'NEW'
        self.send_response(404 if self.path == '/missing' else 200)
        self.send_header('Content-Length', str(50 if self.path == '/short' else len(body)))
        self.end_headers()
        self.wfile.write(body)
        self.close_connection = True

server = http.server.ThreadingHTTPServer(('0.0.0.0', 0), Server)
threading.Thread(target=server.serve_forever, daemon=True).start()
b = pathlib.Path('build')
b.mkdir(exist_ok=True)
image = b / 'pr2-smoke.img'
# The full installation includes compiler sources and Bash instructions.
with image.open('wb') as fresh: fresh.truncate(64 * 1024 * 1024)
subprocess.run(['make', 'build/os.img', 'build/user/pr2check', 'install', 'DISK=' + str(image)], check=True)
subprocess.run(['build/myfs', str(image), 'put', 'build/user/pr2check', '/bin/pr2check'], check=True)
log = b / 'pr2-smoke.log'
log.write_text('')
p = subprocess.Popen(['qemu-system-i386', '-m', '256M', '-display', 'none', '-no-reboot',
    '-drive', 'format=raw,file=build/os.img,index=0', '-drive', f'format=raw,file={image},index=1',
    '-nic', 'user,model=e1000', '-serial', f'file:{log}', '-monitor', 'stdio'],
    stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
def text():
    pattern = re.escape(chr(27) + '[') + '[0-9;?]*[A-Za-z]'
    return re.sub(pattern, '', log.read_text(errors='replace')).replace(chr(13), '')
def wait(marker, after=0):
    end = time.monotonic() + 35
    while time.monotonic() < end:
        if marker in text()[after:]: return
        if p.poll() is not None: raise AssertionError(p.stderr.read())
        time.sleep(.1)
    raise AssertionError(marker + chr(10) + text()[-3000:])
def send(s, marker):
    start = len(text())
    for ch in s + chr(10):
        key = {chr(10): 'ret', ' ': 'spc', '/': 'slash', ':': 'shift-semicolon', '.': 'dot', '-': 'minus'}.get(ch, ch)
        p.stdin.write(f'sendkey {key} 10' + chr(10))
        p.stdin.flush(); time.sleep(.04)
    wait(marker, start)
try:
    wait('New password:'); send('review123', 'Confirm password:')
    send('review123', 'Login [root]:'); send('', 'Password:'); send('review123', 'root:/')
    send(f'pr2check http://10.0.2.2:{server.server_port}', 'PR2 NATIVE PASS')
    assert 'PR2 FAIL:' not in text() and 'PANIC' not in text(), text()[-3000:]
    print('PR2 native regressions: PASS')
finally:
    p.terminate()
    try: p.wait(timeout=3)
    except subprocess.TimeoutExpired: p.kill(); p.wait()
    server.shutdown()
subprocess.run(['python3', 'tests/browser_smoke.py'], check=True,
    env=dict(os.environ, MYOS_TEST_DISK=str(image)))
