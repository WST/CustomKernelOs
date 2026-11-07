"""Run the real MyOS browser on an isolated disk against compressed HTTP fixtures."""
import gzip
import http.server
import pathlib
import re
import shutil
import subprocess
import threading
import time
import zlib

build = pathlib.Path('build')
seen = []
pages = {
    '/': ('text/html', 'gzip', b'<title>Browser smoke</title><link rel="stylesheet" href="/style.css">'
          b'<h1>Browser smoke</h1><div class="row"><div>Alpha</div><div>Beta</div></div>'
          b'<p id="result"></p><script type="module" src="/app.js"></script>'),
    '/style.css': ('text/css', 'deflate', b'.row{display:flex;gap:16px}.row>div{width:50%;padding:12px;box-sizing:border-box;background:#abcdef}'),
    '/app.js': ('application/javascript', 'gzip', b'import {value,inc} from "./counter.js"; inc();'
               b'if(value!==42 || document.currentScript!==null) throw new Error("module context failed");'
               b'async function* seq(){yield Promise.resolve(20);yield await Promise.resolve(22);}'
               b'async function verify(){const controller=new AbortController();let aborted=0;'
               b'controller.signal.addEventListener("abort",()=>aborted++);controller.abort("test");'
               b'if(!controller.signal.aborted || aborted!==1)throw new Error("abort failed");'
               b'try{await fetch("/must-not-fetch",{signal:controller.signal});throw new Error("not aborted");}catch(e){if(e!=="test")throw e;}'
               b'if(await new Response(new Blob(["hello"])).text()!=="hello")throw new Error("response failed");'
               b'let sum=0;for await(const n of seq()) sum+=n;'
               b'if(sum!==42)throw new Error("async generator failed");'
               b'document.getElementById("result").innerHTML=\'<a href="/ok">JSOK</a>\';} verify();'
               b'import("./lazy.js").then(m=>{if(m.default!==7) throw new Error("dynamic failed");});'),
    '/counter.js': ('application/javascript', 'gzip', b'export let value=41;export function inc(){value++;}'),
    '/lazy.js': ('application/javascript', 'gzip', b'export default 7;'),
}

class Server(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def do_GET(self):
        seen.append(self.path)
        ctype, encoding, data = pages.get(self.path, ('text/plain', 'gzip', b'not found'))
        compressed = gzip.compress(data) if encoding == 'gzip' else zlib.compress(data)
        self.send_response(200)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Encoding', encoding.upper())
        self.send_header('Transfer-Encoding', 'Chunked')
        self.send_header('Connection', 'keep-alive')
        self.end_headers()
        for start in range(0, len(compressed), 23):
            chunk = compressed[start:start + 23]
            self.wfile.write(f'{len(chunk):X};test=1\r\n'.encode() + chunk + b'\r\n')
            self.wfile.flush()
        self.wfile.write(b'0\r\nX-Test: done\r\n\r\n')
        self.wfile.flush()
        self.close_connection = False

server = http.server.ThreadingHTTPServer(('0.0.0.0', 0), Server)
threading.Thread(target=server.serve_forever, daemon=True).start()
image = build / 'browser-smoke.img'
shutil.copyfile('disk.img', image)
subprocess.run(['build/myfs', str(image), 'rm', '/etc/accounts'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
subprocess.run(['build/myfs', str(image), 'put', 'build/user/browser', '/bin/browser'], check=True)
script = build / 'browser-smoke.sh'
script.write_text(f'desktop &\nsleep 2\nbrowser -v http://10.0.2.2:{server.server_port}/\n')
subprocess.run(['build/myfs', str(image), 'put', str(script), '/browser-smoke.sh'], check=True)
log = build / 'browser-smoke.serial.log'
log.write_text('')
proc = subprocess.Popen([
    'qemu-system-i386', '-m', '256M', '-vga', 'vmware', '-display', 'none',
    '-drive', 'format=raw,file=build/os.img,index=0',
    '-drive', f'format=raw,file={image},index=1', '-nic', 'user,model=e1000',
    '-serial', f'file:{log}', '-monitor', 'stdio', '-no-reboot',
], stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)

def read_log():
    try:
        text = log.read_text(errors='replace')
    except OSError as error:
        if error.errno != 61:
            raise
        return ''  # transient WSL/Windows file sharing while QEMU flushes serial
    return re.sub(r'\x1b\[[0-9;?]*[A-Za-z]', '', text).replace('\r', '')

def wait(marker, after=0, seconds=30):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        text = read_log()
        if marker in text[after:]:
            return
        if proc.poll() is not None:
            raise AssertionError(proc.stderr.read())
        time.sleep(.1)
    raise AssertionError(f'{marker!r} missing\n' + read_log()[-3000:])

def send(text, marker):
    start = len(read_log())
    for ch in text + '\n':
        key = {' ': 'spc', '/': 'slash', '.': 'dot', '-': 'minus', '\n': 'ret'}.get(ch, ch)
        proc.stdin.write(f'sendkey {key} 10\n')
        proc.stdin.flush()
        time.sleep(.04)
    wait(marker, start)

try:
    wait('New password:')
    send('test123', 'Confirm password:')
    send('test123', 'Login [root]:')
    send('', 'Password:')
    send('test123', 'root:/')
    send('sh /browser-smoke.sh', 'Done:')
    text = read_log()
    assert '1 links - JS: 1 script' in text, text[-2000:]
    assert 'error' not in text[text.rfind('Done:'):], text[-2000:]
    assert all(path in seen for path in pages), seen
    assert 'PANIC' not in text and 'kernel stack overflow' not in text, text[-2000:]
    proc.stdin.write('screendump build/browser-smoke.ppm\n')
    proc.stdin.flush()
    time.sleep(.3)
    print('browser QEMU smoke: OK (chunked keep-alive, gzip HTML/JS, deflate CSS, DOM update)')
finally:
    proc.terminate()
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
    server.shutdown()
