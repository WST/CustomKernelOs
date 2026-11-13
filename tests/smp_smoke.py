"""Boot CPUs with low/high ACPI tables and run ring-3 workers on disposable images."""
import pathlib, re, shutil, subprocess, tempfile, time

ROOT=pathlib.Path(__file__).resolve().parent.parent
B=ROOT/'build'

def run(cpus, extra=(), memory='256M'):
    suffix=('' if memory=='256M' else '-'+memory)+('-fallback' if extra else '')
    with tempfile.TemporaryDirectory(prefix='myos-smp-') as temp:
        temp=pathlib.Path(temp)
        image=temp/'disk.img'
        shutil.copyfile(B/'smp-base.img',image)
        log=temp/'serial.log'; log.touch()
        err=(B/f'smp-{cpus}{suffix}.stderr').open('w')
        p=subprocess.Popen(['qemu-system-i386','-smp',str(cpus),'-m',memory,'-display','none',
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
            send('echo smp-pipe | cat','\nsmp-pipe\n')
            send('sysinfo',f'CPUs online: {expected}')
            print(f'{cpus} CPUs, {memory} RAM {extra}: PASS',flush=True)
        finally:
            p.terminate()
            try:p.wait(timeout=5)
            except subprocess.TimeoutExpired:p.kill();p.wait()
            (B/f'smp-{cpus}{suffix}.log').write_text(text())
            err.close()

def create_root(path, megabytes=32):
    with path.open('wb') as stream: stream.truncate(megabytes*1048576)
    def fs(*args): subprocess.run(['build/myfs',str(path),*args],cwd=ROOT,check=True,stdout=subprocess.DEVNULL)
    for directory in ['/bin','/lib','/etc','/root','/home','/tmp']: fs('mkdir',directory)
    for name in ['sh','smpcheck','smpbench','cat','echo','sysinfo']:
        fs('put','build/user/'+name,'/bin/'+name)
    fs('put','build/ld.so','/lib/ld.so')
    fs('put','build/user/libc.so','/lib/libc.so')
    fs('put','build/user/libgfx.so','/lib/libgfx.so')


def run_live():
    from hardware_inventory_smoke import live_image, VM, login
    import hardware_smoke
    with tempfile.TemporaryDirectory(prefix='myos-smp-live-') as temporary:
        temporary=pathlib.Path(temporary)
        root=temporary/'root.img';image=temporary/'live.img'
        create_root(root,64)
        live_image(root,image,temporary)
        vm=VM('smp-live-8-2048M',image,[],8,memory='2048M')
        try:
            login(vm)
            vm.send('smpcheck 8','SMP PASS online=8 executed=8')
            vm.send('sysinfo','CPUs online: 8')
            text=hardware_smoke.clean(vm.log)
            address=re.search(r'MADT=([0-9a-fA-F]+)',text)
            assert address and int(address[1],16)>=0x40000000,text[-8000:]
            assert 'SMP FAIL' not in text and 'PANIC' not in text,text[-8000:]
            print('Live image, 8 CPUs, 2048M RAM, high MADT and real ring-3 workers: PASS',flush=True)
        finally: vm.stop()


if __name__=='__main__':
    # A minimal root isolates CPU tests from unrelated port/library rebuilds
    # and developer credentials, services and EXT autoinstall configuration.
    subprocess.run(['make','-j4','build/os.img','build/hardware/kernel.bin',
                    'build/user/smpcheck','build/user/smpbench','build/user/sh',
                    'build/user/cat','build/user/echo','build/user/sysinfo','build/user/libc.so','build/user/libgfx.so',
                    'build/ld.so','build/myfs'],cwd=ROOT,check=True,stdout=subprocess.DEVNULL)
    create_root(B/'smp-base.img')
    for cpus in (1,2,4,8): run(cpus)
    for cpus in (4,8): run(cpus,memory='2048M')
    run(4,('-machine','pc,acpi=off'))
    run_live()
