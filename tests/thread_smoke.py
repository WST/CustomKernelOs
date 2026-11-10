"""Native threads on disposable disks: oversubscription, SMP, lifetime and TLB."""
import pathlib,re,shutil,subprocess,tempfile,time
ROOT=pathlib.Path(__file__).resolve().parent.parent
B=ROOT/'build'
def run(cpus,extra=()):
    suffix=f'{cpus}'+('-fallback' if extra else '')
    with tempfile.TemporaryDirectory(prefix='myos-threads-') as temp:
        temp=pathlib.Path(temp); image=temp/'disk.img'
        shutil.copyfile(B/'thread-base.img',image)
        log=temp/'serial.log';log.touch()
        with (B/f'thread-{suffix}.stderr').open('w') as err:
            p=subprocess.Popen(['qemu-system-i386','-smp',str(cpus),'-m','256M','-display','none',
                '-drive','format=raw,file=build/os.img,index=0,snapshot=on',
                '-drive',f'format=raw,file={image},index=1','-nic','none',
                '-serial',f'file:{log}','-monitor','stdio','-no-reboot',*extra],
                cwd=ROOT,stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=err,text=True)
            def text(): return re.sub(r'\x1b\[[0-9;?]*[A-Za-z]','',log.read_text(errors='replace')).replace('\r','')
            def wait(marker,after=0,timeout=100):
                end=time.monotonic()+timeout
                while time.monotonic()<end:
                    s=text()
                    if any(x in s for x in ('PANIC','THREAD FAIL','SMP FAIL','Segmentation fault')): raise AssertionError(s[-5000:])
                    if marker in s[after:]:return
                    if p.poll() is not None:raise AssertionError('QEMU exited: '+s[-4000:])
                    time.sleep(.05)
                raise AssertionError(marker+' missing: '+text()[-5000:])
            def send(s,marker):
                start=len(text())
                for c in s+'\n':
                    key={'\n':'ret',' ':'spc','/':'slash','-':'minus'}.get(c,c)
                    p.stdin.write('sendkey '+key+' 10\n');p.stdin.flush();time.sleep(.04)
                wait(marker,start)
            try:
                wait('New password:');send('smp123','Confirm password:')
                send('smp123','Login [root]:');send('','Password:');send('smp123','root:/')
                expected=1 if extra else cpus
                send(f'threadcheck {expected}',f'THREAD PASS count=100 cpus={expected}')
                send('smpcheck '+str(expected),f'SMP PASS online={expected} executed={expected}')
                print(f'Threads {suffix} CPUs: PASS',flush=True)
            finally:
                p.terminate()
                try:p.wait(timeout=5)
                except subprocess.TimeoutExpired:p.kill();p.wait()
                (B/f'thread-{suffix}.log').write_text(text())
if __name__=='__main__':
    subprocess.run(['make','-j4','build/os.img','build/user/threadcheck','build/user/smpcheck','install','DISK=build/thread-base.img'],cwd=ROOT,check=True,stdout=subprocess.DEVNULL)
    for name in ('threadcheck','smpcheck'):
        subprocess.run(['build/myfs','build/thread-base.img','put','build/user/'+name,'/bin/'+name],cwd=ROOT,check=True)
    subprocess.run(['build/myfs','build/thread-base.img','rm','/etc/accounts'],cwd=ROOT,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    for cpus in (1,2,4,8):run(cpus)
    run(4,('-machine','pc,acpi=off'))
