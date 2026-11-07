"""Exercise the production greeter and account syscall in QEMU on disposable disks."""
from pathlib import Path
import subprocess, time, shutil
b=Path('build')
subprocess.run(['make','-j4','build/os.img','build/user/libmyos.a'],check=True)
subprocess.run(['gcc','-m32','-ffreestanding','-fno-pie','-fno-pic','-fno-stack-protector','-fno-builtin','-nostdlib','-nostdinc','-mno-sse','-mno-mmx','-O2','-Wall','-std=gnu11','-Iuser/include','-Iuser/lib','-ffunction-sections','-fdata-sections','-c','tests/desktop_logincheck.c','-o','build/desktop-logincheck.o'],check=True)
subprocess.run(['ld','-m','elf_i386','-T','user/user.ld','-nostdlib','-s','--gc-sections','build/desktop-logincheck.o','build/user/libmyos.a','-o','build/desktop-logincheck'],check=True)
def run(cancel=False, desktop=False):
    tag='desktop' if desktop else ('cancel' if cancel else 'user')
    image=b/f'login-{tag}.img'
    with image.open('wb') as f: f.truncate(4*1024*1024)
    subprocess.run(['make','install','DISK='+str(image)],check=True,stdout=subprocess.DEVNULL)
    def put(src,dst): subprocess.run(['build/myfs',str(image),'put',str(src),dst],check=True,stdout=subprocess.DEVNULL)
    put(b/'desktop-logincheck','/bin/desktop-logincheck')
    unit=b/'logincheck.service'; unit.write_text('[Service]\nExecStart=/bin/desktop-logincheck '+('desktop' if desktop else ('cancel' if cancel else 'user'))+'\nStandardInput=tty\n')
    put(unit,'/etc/services/logincheck.service')
    empty=b/'login-enabled'; empty.write_text(''); put(empty,'/etc/services/enabled/logincheck.service')
    boot=b/f'login-{tag}-boot.img'; shutil.copyfile(b/'os.img',boot)
    serial=b/f'login-{tag}.serial'; serial.write_text('')
    p=subprocess.Popen(['qemu-system-i386','-m','256M','-vga','vmware','-display','none','-no-reboot','-drive',f'format=raw,file={boot},index=0','-drive',f'format=raw,file={image},index=1','-serial',f'file:{serial}','-monitor','stdio'],stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,text=True)
    def log():
        x=subprocess.run(['build/myfs',str(image),'cat','/var/log/logincheck.service'],capture_output=True,text=True)
        return x.stdout
    def wait(marker):
        deadline=time.monotonic()+30
        while time.monotonic()<deadline:
            if p.poll() is not None: raise AssertionError(p.stderr.read())
            s=log()
            if marker in s: return s
            if 'LOGIN FAIL' in s: raise AssertionError(s)
            time.sleep(.2)
        raise AssertionError(log()+serial.read_text(errors='replace'))
    def key(k): p.stdin.write('sendkey '+k+' 10\n'); p.stdin.flush(); time.sleep(.12)
    def word(s):
        for c in s: key(c)
        key('ret')
    try:
        wait('DESKTOP READY' if desktop else 'LOGIN READY'); time.sleep(.5)
        p.stdin.write(f'screendump build/login-{tag}.ppm\n');p.stdin.flush()
        if desktop:
            key('down'); word('alice123'); wait('GUI SIGNED IN alice'); time.sleep(.5)
            def monitor(command): p.stdin.write(command+'\n');p.stdin.flush();time.sleep(.2)
            monitor('mouse_move 488 -368')
            monitor('mouse_button 1'); monitor('mouse_button 0')
            monitor('mouse_move -100 138')
            monitor('mouse_button 1'); monitor('mouse_button 0')
            wait('GUI LOGGED OUT'); time.sleep(.4)
            monitor('mouse_move -388 246')
            monitor('mouse_button 1'); monitor('mouse_button 0')
            monitor('screendump build/login-focus.ppm')
            word('review123'); wait('GUI SIGNED IN root')
            print('Desktop logout and second account: PASS')
            return
        if cancel: key('esc')
        else:
            word('wrong'); time.sleep(.7)
            assert 'LOGIN PASS' not in log(), 'Wrong password accepted'
            key('down'); word('alice123')
        result=wait('LOGIN PASS'); assert 'LOGIN FAIL' not in result,result
        print('Login '+tag+': PASS')
    finally:
        p.terminate()
        try: p.wait(timeout=3)
        except subprocess.TimeoutExpired: p.kill();p.wait()
run()
run(True)

run(desktop=True)
