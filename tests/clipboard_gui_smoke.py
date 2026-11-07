"""Check the real desktop/Notepad event path and saved copy/paste result."""
import pathlib, re, shutil, subprocess, time
subprocess.run(['make', 'build/myfs', 'build/os.img', 'build/user/desktop', 'build/user/notepad'], check=True)
b = pathlib.Path('build')
image = b / 'clipboard-gui-smoke.img'
with image.open('wb') as fresh:
    fresh.truncate(4 * 1024 * 1024)
subprocess.run(['make', 'install', 'DISK=' + str(image)], check=True)
for app in ('desktop', 'notepad'):
    subprocess.run(['build/myfs', str(image), 'put', str(b / 'user' / app), '/bin/' + app], check=True)
script = b / 'clipboard-gui.sh'
script.write_text('desktop &\nsleep 1\nnotepad /tmp/clipboard-gui.txt &\nsleep 1000\n')
subprocess.run(['build/myfs', str(image), 'put', str(script), '/clipboard-gui.sh'], check=True)
log = b / 'clipboard-gui-smoke.log'
log.write_text('')
p = subprocess.Popen(['qemu-system-i386', '-m', '256M', '-display', 'none', '-vga', 'vmware', '-no-reboot', '-drive', 'format=raw,file=build/os.img,index=0', '-drive', f'format=raw,file={image},index=1', '-serial', f'file:{log}', '-monitor', 'stdio'], stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
def text():
    return re.sub(r'\x1b\[[0-9;?]*[A-Za-z]', '', log.read_text(errors='replace')).replace('\r', '')
def wait(s):
    until = time.monotonic() + 20
    while time.monotonic() < until:
        if s in text(): return
        if p.poll() is not None: raise AssertionError(p.stderr.read())
        time.sleep(.1)
    raise AssertionError(s + '\n' + text()[-2000:])
def key(k):
    p.stdin.write(f'sendkey {k} 10\n'); p.stdin.flush(); time.sleep(.12)
def send(s):
    for ch in s + '\n': key({'\n': 'ret', '/': 'slash', '-': 'minus', '.': 'dot', ' ': 'spc'}.get(ch, ch))
try:
    wait('New password:'); send('port123')
    wait('Confirm password:'); send('port123')
    wait('Login [root]:'); send('')
    wait('Password:'); send('port123')
    wait('root:/'); send('sh /clipboard-gui.sh')
    time.sleep(3)
    for ch in 'hello': key(ch)
    key('shift-left'); key('shift-left')
    p.stdin.write('screendump build/clipboard-notepad-selection.ppm\n'); p.stdin.flush(); time.sleep(.3)
    key('ctrl-c'); key('end'); key('ret'); key('ctrl-v'); key('ctrl-s')
    time.sleep(.4)
    assert 'PANIC' not in text(), text()[-2000:]
finally:
    p.terminate()
    try: p.wait(timeout=3)
    except subprocess.TimeoutExpired: p.kill(); p.wait()
saved = subprocess.check_output(['build/myfs', str(image), 'cat', '/tmp/clipboard-gui.txt'])
assert saved == b'hello\nlo\n', repr(saved)
print('Clipboard GUI: PASS (real desktop, Notepad, Shift selection, copy/paste/save)')
