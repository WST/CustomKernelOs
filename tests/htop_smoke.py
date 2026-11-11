"""Exercise the actual htop binary in QEMU on a disposable disk."""
import pathlib, re, subprocess, time, tempfile, os, shutil
ROOT = pathlib.Path(__file__).resolve().parent.parent
scratch = tempfile.TemporaryDirectory(prefix='myos-htop-')
log = pathlib.Path(scratch.name) / 'serial.log'
image = 'build/htop-test.img'
subprocess.run(['build/myfs', image, 'rm', '/etc/accounts'], cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
subprocess.run(['make', 'build/user/cpuinfocheck'], cwd=ROOT, check=True)
subprocess.run(['build/myfs',image,'put','build/user/cpuinfocheck','/bin/cpuinfocheck'],cwd=ROOT,check=True)
shutil.copyfile(ROOT/'build/os.img',pathlib.Path(scratch.name)/'os.img')
shutil.copyfile(ROOT/image,pathlib.Path(scratch.name)/'disk.img')
log.write_text('')
p = subprocess.Popen(['qemu-system-i386', '-smp', os.environ.get('HTOP_CPUS','2'), '-m', '256M', '-display', 'none', '-no-reboot',
    '-drive', 'format=raw,file='+str(pathlib.Path(scratch.name)/'os.img')+',index=0,snapshot=on',
    '-drive', 'format=raw,file='+str(pathlib.Path(scratch.name)/'disk.img')+',index=1,snapshot=on', '-nic', 'none',
    '-serial', 'file:'+str(log), '-monitor', 'stdio'], cwd=ROOT,
    stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
def text():
    return re.sub(r'\x1b\[[0-9;?]*[A-Za-z]', '', log.read_text(errors='replace')).replace('\r','')
def wait(marker, after=0):
    end = time.monotonic()+40
    while time.monotonic()<end:
        s=text()
        assert 'PANIC' not in s and 'Segmentation fault' not in s, s[-3000:]
        if marker in s[after:]: return
        if p.poll() is not None: raise AssertionError(p.stderr.read())
        time.sleep(.1)
    raise AssertionError(marker+' missing: '+text()[-3000:])
def key(k):
    p.stdin.write('sendkey '+k+' 10\n'); p.stdin.flush(); time.sleep(.12)
def send(s, marker):
    start=len(text())
    for c in s+'\n': key({'\n':'ret',' ':'spc','/':'slash','-':'minus'}.get(c,c))
    wait(marker,start)
def check(k, marker):
    start=len(text()); key(k); wait(marker,start)
try:
    wait('New password:'); send('htop123','Confirm password:')
    send('htop123','Login [root]:'); send('','Password:'); send('htop123','root:/')
    send('cpuinfocheck','CPUINFO PASS')
    send('htop --help','Interactive MyOS process monitor.')
    send('htop','Sort: CPU%')
    cpus=int(os.environ.get('HTOP_CPUS','2'))
    for i in range(cpus): wait(f'CPU{i}[')
    assert f'CPU{cpus}[' not in text()
    check('shift-m','Sort: MEM')
    check('t','Tree')
    check('p','PAUSED')
    check('h','htop for MyOS - press h')
    check('h','Sort: MEM')
    check('backslash','Filter:')
    for c in 'htop': key(c)
    check('ret','htop')
    # Only htop remains in the filter; killing self must be refused.
    check('delete','Select a user process other than htop.')
    check('q','root:/')
    send('echo htop-restored','htop-restored\n')
    print('HTOP PASS: boot, help, sorting, tree, pause, filter, kill guard, TTY restore')
finally:
    (ROOT / 'build/htop-smoke.log').write_bytes(log.read_bytes())
    p.terminate()
    try: p.wait(timeout=3)
    except subprocess.TimeoutExpired: p.kill(); p.wait()
    scratch.cleanup()


