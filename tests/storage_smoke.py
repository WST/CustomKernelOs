"""Boot the real AHCI driver and verify closed file data across a restart."""
import pathlib,re,shutil,subprocess,tempfile,time
ROOT=pathlib.Path(__file__).resolve().parents[1]
B=ROOT/'build'

def run(*args):
    subprocess.run([str(a) for a in args],cwd=ROOT,check=True,stdout=subprocess.DEVNULL)

with tempfile.TemporaryDirectory(prefix='myos-storage-') as directory:
    directory=pathlib.Path(directory);disk=directory/'data.img';boot=directory/'boot.img';log=directory/'serial'
    with disk.open('wb') as stream:stream.truncate(64*1048576)
    run('make','-j4','build/os.img','build/user/storagecheck','install','DISK='+str(disk))
    run(B/'myfs',disk,'put',B/'user/storagecheck','/bin/storagecheck')
    shutil.copyfile(B/'os.img',boot)
    q=None
    def text():return re.sub(r'\x1b\[[0-9;?]*[A-Za-z]','',log.read_text(errors='replace')).replace('\r','')
    def wait(marker,start=0):
        deadline=time.monotonic()+120
        while time.monotonic()<deadline:
            if marker in text()[start:]:return
            if q.poll() is not None:raise AssertionError(q.stderr.read()+text()[-4000:])
            time.sleep(.1)
        raise AssertionError(marker+' missing: '+text()[-4000:])
    def send(command,marker):
        start=len(text())
        for char in command+'\n':
            key={'\n':'ret',' ':'spc'}.get(char,char)
            q.stdin.write('sendkey '+key+' 10\n');q.stdin.flush();time.sleep(.04)
        wait(marker,start)
    def stop():
        q.terminate()
        try:q.wait(timeout=5)
        except subprocess.TimeoutExpired:q.kill();q.wait()
    try:
        for iteration in range(2):
            log.write_text('')
            q=subprocess.Popen(['qemu-system-i386','-cpu','max','-m','512M','-display','none','-no-reboot','-nic','none',
                '-drive','format=raw,file='+str(boot)+',index=0',
                '-drive','if=none,id=sysdisk,format=raw,file='+str(disk),
                '-device','ich9-ahci,id=ahci','-device','ide-hd,drive=sysdisk,bus=ahci.0',
                '-serial','file:'+str(log),'-monitor','stdio'],stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,text=True)
            wait('AHCI disk:')
            if not iteration:
                wait('New password:');send('storage123','Confirm password:');send('storage123','Login [root]:')
            else:wait('Login [root]:')
            send('','Password:');send('storage123','root:/')
            send('storagecheck'+(' verify' if iteration else ''),'STORAGE DATA PASS')
            (B/('storage-ahci-'+str(iteration)+'.log')).write_text(text())
            stop();q=None
    finally:
        if q is not None:
            (B/'storage-ahci-failure.log').write_text(text());stop()
print('AHCI QEMU PASS: write, close, read, cache pressure and persistence after restart')
