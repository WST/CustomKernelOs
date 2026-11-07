"""Boot a disposable disk twice; exercise the real manager and persistent enable."""
import pathlib, re, subprocess, time
b = pathlib.Path('build'); image = b/'services-test.img'
with image.open('wb') as f: f.truncate(4 * 1024 * 1024)
subprocess.run(['make', '-j4', 'build/os.img', 'build/user/servicecheck', 'install', 'DISK='+str(image)], check=True)
subprocess.run(['build/myfs', str(image), 'put', 'build/user/servicecheck', '/bin/servicecheck'], check=True)
def boot(first):
    log = b/('services-first.log' if first else 'services-boot.log'); log.write_text('')
    p = subprocess.Popen(['qemu-system-i386', '-m', '256M', '-display', 'none', '-no-reboot',
        '-drive', 'format=raw,file=build/os.img,index=0', '-drive', f'format=raw,file={image},index=1',
        '-nic', 'user,model=e1000', '-serial', f'file:{log}', '-monitor', 'stdio'],
        stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    def text():
        return re.sub(r'\x1b\[[0-9;?]*[A-Za-z]', '', log.read_text(errors='replace')).replace('\r','')
    def wait(marker, after=0):
        end = time.monotonic()+35
        while time.monotonic()<end:
            if marker in text()[after:]: return
            if p.poll() is not None: raise AssertionError(p.stderr.read())
            time.sleep(.1)
        raise AssertionError(marker+'\n'+text()[-4000:])
    def send(s, marker):
        start = len(text())
        for ch in s+'\n':
            key = {'\n':'ret',' ':'spc','/':'slash','.':'dot','-':'minus'}.get(ch,ch)
            p.stdin.write(f'sendkey {key} 10\n'); p.stdin.flush(); time.sleep(.04)
        wait(marker,start)
    try:
        if first:
            wait('New password:'); send('review123','Confirm password:'); send('review123','Login [root]:')
        else: wait('Login [root]:')
        send('', 'Password:'); send('review123','root:/')
        send('servicecheck' if first else 'servicecheck boot', 'SERVICE NATIVE PASS' if first else 'SERVICE BOOT PASS')
        assert 'SERVICE FAIL' not in text() and 'PANIC' not in text(), text()[-4000:]
        send('systemctl status httpd', 'disabled')
        print('Service lifecycle: PASS' if first else 'Persistent boot autostart: PASS')
    finally:
        p.terminate()
        try: p.wait(timeout=3)
        except subprocess.TimeoutExpired: p.kill(); p.wait()
boot(True)
boot(False)

# Verify graphical autostart and return to the console after stopping desktop.
for name, text in {
    'desktop.service': '[Service]\nType=simple\nExecStart=/bin/desktop\nStandardInput=tty\n',
    'desktopcheck.service': '[Service]\nExecStart=/bin/servicecheck desktop\n',
}.items():
    fixture=b/name; fixture.write_text(text)
    subprocess.run(['build/myfs',str(image),'put',str(fixture),'/etc/services/'+name],check=True)
    empty=b/'service-enabled'; empty.write_text('')
    subprocess.run(['build/myfs',str(image),'put',str(empty),'/etc/services/enabled/'+name],check=True)
log=b/'services-desktop.log'; log.write_text('')
p=subprocess.Popen(['qemu-system-i386','-m','256M','-vga','vmware','-display','none','-no-reboot',
    '-drive','format=raw,file=build/os.img,index=0','-drive',f'format=raw,file={image},index=1',
    '-nic','user,model=e1000','-serial',f'file:{log}','-monitor','stdio'],
    stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,text=True)
try:
    time.sleep(3)
    assert 'Login [root]:' not in log.read_text(errors='replace'), 'Console stole desktop input'
    p.stdin.write('screendump build/services-desktop.ppm\n');p.stdin.flush()
    deadline=time.monotonic()+30
    while 'Login [root]:' not in log.read_text(errors='replace') and time.monotonic()<deadline:
        time.sleep(.1)
    assert 'Login [root]:' in log.read_text(errors='replace'), log.read_text(errors='replace')[-3000:]
finally:
    p.terminate()
    try: p.wait(timeout=3)
    except subprocess.TimeoutExpired: p.kill();p.wait()
result=subprocess.run(['build/myfs',str(image),'cat','/var/log/desktopcheck.service'],capture_output=True,text=True,check=True)
assert 'SERVICE DESKTOP PASS' in result.stdout and 'SERVICE FAIL' not in result.stdout,result.stdout
print('Graphical autostart and console recovery: PASS')
