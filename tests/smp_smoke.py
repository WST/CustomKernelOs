"""Boot 1/2/4/8 CPUs and exercise real ring-3 workers on disposable images."""
import pathlib, re, shutil, subprocess, tempfile, time

ROOT=pathlib.Path(__file__).resolve().parent.parent
B=ROOT/'build'

def run(cpus, extra=()):
    with tempfile.TemporaryDirectory(prefix='myos-smp-') as temp:
        temp=pathlib.Path(temp)
        image=temp/'disk.img'
        shutil.copyfile(B/'smp-base.img',image)
        log=temp/'serial.log'; log.touch()
        err=(B/f'smp-{cpus}{"-fallback" if extra else ""}.stderr').open('w')
        p=subprocess.Popen(['qemu-system-i386','-smp',str(cpus),'-m','256M','-display','none',
            '-drive','format=raw,file=build/os.img,index=0,snapshot=on',
            '-drive',f'format=raw,file={image},index=1','-nic','none',
            '-serial',f'file:{log}','-monitor','stdio','-no-reboot',*extra],
            cwd=ROOT,stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=err,text=True)
        def text(): return re.sub(r'\x1b\[[0-9;?]*[A-Za-z]','',log.read_text(errors='replace')).replace('\r','')
        def wait(marker,after=0,timeout=45):
            end=time.monotonic()+timeout
            while time.monotonic()<end:
                s=text()
                if 'PANIC' in s or 'SMP FAIL' in s or 'Segmentation fault' in s: raise AssertionError(s[-4000:])
                if marker in s[after:]: return
                if p.poll() is not None:
                    err.flush()
                    raise AssertionError('QEMU exited: '+pathlib.Path(err.name).read_text()+s[-3000:])
                time.sleep(.05)
            raise AssertionError(marker+' missing: '+text()[-4000:])
        def send(s,marker):
            start=len(text())
            for c in s+'\n':
                key={'\n':'ret',' ':'spc','/':'slash','-':'minus','|':'shift-backslash'}.get(c,c)
                p.stdin.write('sendkey '+key+' 10\n');p.stdin.flush();time.sleep(.04)
            wait(marker,start)
        try:
            wait('New password:');send('smp123','Confirm password:')
            send('smp123','Login [root]:');send('','Password:');send('smp123','root:/')
            expected=1 if extra else cpus
            send('smpbench','IPC BENCH')
            send(f'smpcheck {expected}',f'SMP PASS online={expected} executed={expected}')
            send('echo smp-pipe | cat','smp-pipe\n')
            send('sysinfo',f'CPUs online: {expected}')
            print(f'{cpus} CPUs {extra}: PASS',flush=True)
        finally:
            p.terminate()
            try:p.wait(timeout=5)
            except subprocess.TimeoutExpired:p.kill();p.wait()
            (B/f'smp-{cpus}{"-fallback" if extra else ""}.log').write_text(text())
            err.close()

if __name__=='__main__':
    subprocess.run(['make','-j4','build/os.img','build/user/smpcheck','build/user/smpbench','install','DISK=build/smp-base.img'],cwd=ROOT,check=True,stdout=subprocess.DEVNULL)
    subprocess.run(['build/myfs','build/smp-base.img','put','build/user/smpcheck','/bin/smpcheck'],cwd=ROOT,check=True)
    subprocess.run(['build/myfs','build/smp-base.img','put','build/user/smpbench','/bin/smpbench'],cwd=ROOT,check=True)
    # Always start at first-boot password setup, regardless of prior test runs.
    subprocess.run(['build/myfs','build/smp-base.img','rm','/etc/accounts'],cwd=ROOT,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    for cpus in (1,2,4,8):run(cpus)
    run(4,('-machine','pc,acpi=off'))
