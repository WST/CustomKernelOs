"""Verify clipboard, editor, terminal and modified keys in real MyOS."""
import pathlib
import re
import shutil
import subprocess
import time

subprocess.run(['make', 'build/myfs', 'build/os.img', 'build/user/clipboardcheck'], check=True)
build = pathlib.Path('build')
executable = build / 'user/clipboardcheck'
elf = executable.read_bytes()
assert elf[:5] == b'\x7fELF\x01' and elf[18:20] == b'\x03\x00'
image = build / 'clipboard-smoke.img'
with image.open('wb') as fresh:
    fresh.truncate(64 * 1024 * 1024)
subprocess.run(['make', 'install', 'DISK=' + str(image)], check=True)
subprocess.run(['build/myfs', str(image), 'put', str(executable), '/bin/clipboardcheck'], check=True)
log = build / 'clipboard-smoke.log'
log.write_text('')
process = subprocess.Popen([
    'qemu-system-i386', '-m', '256M', '-display', 'none', '-no-reboot',
    '-drive', 'format=raw,file=build/os.img,index=0',
    '-drive', f'format=raw,file={image},index=1',
    '-serial', f'file:{log}', '-monitor', 'stdio',
], stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)

def text():
    try:
        value = log.read_text(errors='replace')
    except OSError as error:
        if error.errno != 61:
            raise
        return ''
    return re.sub(r'\x1b\[[0-9;?]*[A-Za-z]', '', value).replace('\r', '')

def wait(marker, after=0):
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        if marker in text()[after:]:
            return
        if process.poll() is not None:
            raise AssertionError(process.stderr.read())
        time.sleep(.1)
    raise AssertionError(marker + ' missing\n' + text()[-2500:])

def send(value, marker):
    start = len(text())
    for character in value + '\n':
        key = {'\n': 'ret', '-': 'minus'}.get(character, character)
        process.stdin.write(f'sendkey {key} 10\n')
        process.stdin.flush()
        time.sleep(.04)
    wait(marker, start)

try:
    wait('New password:')
    send('port123', 'Confirm password:')
    send('port123', 'Login [root]:')
    send('', 'Password:')
    send('port123', 'root:/')
    send('clipboardcheck', 'CLIPCHECK KEYS READY')
    for key in ['shift-right', 'ctrl-shift-c', 'ctrl-shift-v', 'shift-insert']:
        process.stdin.write(f'sendkey {key} 10\n')
        process.stdin.flush()
        time.sleep(.15)
    wait('CLIPCHECK PASS')
    assert 'CLIPCHECK EDITOR PASS' in text() and 'CLIPCHECK TERMINAL PASS' in text(), text()[-2500:]
    assert 'PANIC' not in text() and 'kernel stack overflow' not in text(), text()[-2500:]
    print('Clipboard: PASS in QEMU')

finally:
    process.terminate()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
