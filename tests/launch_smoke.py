"""Direct spawn correctness, large-parent benchmark and real GUI launches."""
import pathlib,re,shutil,subprocess,tempfile,time
ROOT=pathlib.Path(__file__).resolve().parent.parent
B=ROOT/'build'
def myfs(image,*args):
    return subprocess.run(['build/myfs',str(image),*map(str,args)],cwd=ROOT,check=True,stdout=subprocess.DEVNULL)

def run(cpus,gui=False):
    tag=f'launch-{cpus}'+('-gui' if gui else '')
    with tempfile.TemporaryDirectory(prefix='myos-launch-') as directory:
        temp=pathlib.Path(directory);image=temp/'disk.img';log=temp/'serial.log';log.touch()
        shutil.copyfile(B/'launch-base.img',image)
        if gui:
            unit=temp/'launchcheck.service'
            unit.write_text('[Service]\nExecStart=/bin/desktop_launchcheck\nStandardInput=tty\n')
            enabled=temp/'enabled';enabled.touch()
            myfs(image,'put',unit,'/etc/services/launchcheck.service')
            myfs(image,'put',enabled,'/etc/services/enabled/launchcheck.service')
        err=(B/(tag+'.stderr')).open('w')
        p=subprocess.Popen(['qemu-system-i386','-smp',str(cpus),'-m','256M','-display','none','-vga','vmware',
            '-drive','format=raw,file=build/os.img,index=0,snapshot=on',
            '-drive',f'format=raw,file={image},index=1','-nic','none',
            '-serial',f'file:{log}','-monitor','stdio','-no-reboot'],cwd=ROOT,
            stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=err,text=True)
        def text():return re.sub(r'\x1b\[[0-9;?]*[A-Za-z]','',log.read_text(errors='replace')).replace('\r','')
        def journal():
            r=subprocess.run(['build/myfs',str(image),'cat','/var/log/launchcheck.service'],cwd=ROOT,capture_output=True,text=True)
            return r.stdout
        def wait(marker,after=0,service=False):
            deadline=time.monotonic()+45
            while time.monotonic()<deadline:
                s=journal() if service else text()
                if 'PANIC' in text() or 'SPAWN FAIL' in s or 'Segmentation fault' in s:raise AssertionError(s+text())
                if marker in s[after:]:return s
                if p.poll() is not None:raise AssertionError('QEMU exited: '+text())
                time.sleep(.1)
            raise AssertionError(marker+' missing:\n'+s+text())
        def send(command,marker):
            start=len(text())
            for char in command+'\n':
                key={'\n':'ret',' ':'spc','/':'slash','-':'minus'}.get(char,char)
                p.stdin.write('sendkey '+key+' 10\n');p.stdin.flush();time.sleep(.04)
            return wait(marker,start)
        try:
            if gui:
                result=wait('GUI LAUNCH PASS',service=True)
                p.stdin.write(f'screendump build/{tag}.ppm\n');p.stdin.flush();time.sleep(.2)
                print(result,flush=True)
            else:
                wait('New password:');send('launch123','Confirm password:')
                send('launch123','Login [root]:');send('','Password:');send('launch123','root:/')
                send('spawncheck','SPAWN PASS')
                result=send('launchbench','SPAWN LAUNCH BENCH')
                print('\n'.join(line for line in result.splitlines() if 'LAUNCH BENCH' in line),flush=True)
            print(tag+': PASS',flush=True)
        finally:
            p.terminate()
            try:p.wait(timeout=5)
            except subprocess.TimeoutExpired:p.kill();p.wait()
            (B/(tag+'.log')).write_text(text())
            if gui:(B/(tag+'.service.log')).write_text(journal())
            err.close()

if __name__=='__main__':
    subprocess.run(['make','-j4','build/os.img','build/user/spawncheck','build/user/launchbench',
        'build/user/desktop_launchcheck','install','DISK=build/launch-base.img'],cwd=ROOT,check=True,stdout=subprocess.DEVNULL)
    for name in ('spawncheck','launchbench','desktop_launchcheck'):
        myfs(B/'launch-base.img','put',B/'user'/name,'/bin/'+name)
    subprocess.run(['build/myfs','build/launch-base.img','rm','/etc/accounts'],cwd=ROOT,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    for cpus in (1,4):run(cpus)
    for cpus in (1,4):run(cpus,gui=True)
