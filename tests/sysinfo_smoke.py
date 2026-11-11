"""Boot and ABI regression for kernel system information."""
import pathlib
import re
import subprocess
import time

b = pathlib.Path('build')
b.mkdir(exist_ok=True)
image = b / 'sysinfo-smoke.img'
with image.open('wb') as fresh: fresh.truncate(32 * 1024 * 1024)
subprocess.run(['make', 'build/os.img', 'build/user/sysinfocheck', 'install', 'DISK=' + str(image)], check=True)
subprocess.run(['build/myfs', str(image), 'put', 'build/user/sysinfocheck', '/bin/sysinfocheck'], check=True)
log = b / 'sysinfo-smoke.log'
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
    send('sysinfocheck', 'SYSINFO ABI PASS')
    send('sysinfo', 'Network: e1000')
    assert 'CPU: QEMU' in text(), text()[-3000:]
    assert 'CPU base (nominal): unknown' in text(), text()[-3000:]
    commit = re.search(r'MYOS_COMMIT "([^"]+)"', (b/'build_info.h').read_text()).group(1)
    assert 'Commit: ' + commit in text(), text()[-3000:]
    assert 'Block: ' in text() and 'Graphics: ' in text(), text()[-3000:]
    assert 'SYSINFO FAIL:' not in text() and 'PANIC' not in text(), text()[-3000:]
    print('SYSINFO boot, CPU identification and ABI: PASS')
finally:
    p.terminate()
    try: p.wait(timeout=3)
    except subprocess.TimeoutExpired: p.kill(); p.wait()
