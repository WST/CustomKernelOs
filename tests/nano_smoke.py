"""Install the real nano recipe with guest cc, then exercise the editor."""
import hashlib,pathlib,subprocess,tempfile,time,re,sys,tarfile
from hardware_smoke import save_png
network='--network' in sys.argv
B=pathlib.Path('build');archive=B/'nano-port/nano-9.3.tar.gz'
assert hashlib.sha256(archive.read_bytes()).hexdigest()=='3208f3c459c64e4a4e0ef242d2653572a56ca9a6cfbdc4de5af4714594ae8836'
def run(*args,**kwargs):return subprocess.run(list(args),check=True,**kwargs)
with tempfile.TemporaryDirectory(prefix='myos-nano-') as folder:
    folder=pathlib.Path(folder);disk=folder/'data.img';log=folder/'serial';log.write_text('')
    with disk.open('wb') as f:f.truncate(128*1048576)
    run('make','-j4','build/os.img','build/user/nanocheck','install','DISK='+str(disk),'EXT_AUTOINSTALL_PACKAGES=','SSH_AUTHORIZED_KEY=','SSH_AUTHORIZED_KEY_FILE=',stdout=subprocess.DEVNULL)
    def fs(*args):return run('build/myfs',str(disk),*args,stdout=subprocess.DEVNULL)
    fs('mkdir','/tmp/ext/nano')
    if not network:fs('put',str(archive),'/tmp/ext/nano/nano-9.3.tar.gz')
    fs('put','build/user/nanocheck','/bin/nanocheck')
    recipe=folder/'setup.inst';recipe.write_text('\n'.join('# preloaded verified archive' if s.startswith('fetch ') else s for s in pathlib.Path('ext/instructions/nano/setup.inst').read_text().splitlines())+'\n')
    if not network:fs('put',str(recipe),'/ext/instructions/nano/setup.inst')
    q=subprocess.Popen(['qemu-system-i386','-m','512M','-display','none','-no-reboot','-nic','user,model=e1000' if network else 'none','-drive','cache=unsafe,format=raw,file=build/os.img,index=0','-drive','cache=unsafe,format=raw,file='+str(disk)+',index=1','-serial','file:'+str(log),'-monitor','stdio'],stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,text=True)
    def text():return re.sub(r'\x1b\[[0-9;?]*[A-Za-z]','',log.read_text(errors='replace')).replace('\r','')
    def wait(marker,after=0,timeout=180):
        end=time.monotonic()+timeout
        while time.monotonic()<end:
            if marker in text()[after:]:return
            if q.poll() is not None:raise AssertionError(q.stderr.read()+text()[-3000:])
            time.sleep(.1)
        raise AssertionError(marker+' missing:\n'+text()[-4000:])
    def key(code):q.stdin.write('sendkey '+code+' 10\n');q.stdin.flush();time.sleep(.06)
    def send(s,marker,timeout=180):
        start=len(text())
        for c in s+'\n':key({'\n':'ret',' ':'spc','/':'slash','.':'dot','-':'minus'}.get(c,c))
        if marker:wait(marker,start,timeout)
    try:
        wait('New password:');send('nanotest123','Confirm password:');send('nanotest123','Login [root]:');send('','Password:');send('nanotest123','root:/')
        send('ext install nano','installed nano',600 if network else 300)
        send('nano --version','GNU nano, version 9.3')
        send('nano /tmp/nano-console.txt',None);time.sleep(1)
        ppm=B/'nano-console.ppm';ppm.unlink(missing_ok=True)
        q.stdin.write('screendump '+str(ppm.resolve())+'\n');q.stdin.flush()
        deadline=time.monotonic()+10
        while True:
            try:
                save_png(ppm,B/'nano-console.png');break
            except (AssertionError,ValueError,OSError):
                if time.monotonic()>=deadline:raise
                time.sleep(.05)
        start=len(text());key('ctrl-x');wait('root:/',start)
        send('nanocheck','NANO EDIT PASS',60)
        send('ext install nano','already installed nano')
        send('ext uninstall nano','removed nano')
        send('ls /tmp','nano-edit-check.txt')
    finally:
        q.terminate()
        try:q.wait(timeout=5)
        except subprocess.TimeoutExpired:q.kill();q.wait()
        (B/'nano-native.log').write_text(text())
    if network:
        # myfs cat prints C strings and cannot export a binary gzip stream.
        # Compare complete, unmodified text members of the downloaded source.
        with tarfile.open(archive) as upstream:
            for name in ['src/definitions.h','src/chars.c','COPYING']:
                downloaded=subprocess.run(['build/myfs',str(disk),'cat','/tmp/ext/nano/nano-9.3/'+name],capture_output=True,check=True).stdout
                assert downloaded==upstream.extractfile('nano-9.3/'+name).read(),name

    check=subprocess.run(['build/myfs',str(disk),'stat','/ext/bin/nano'],capture_output=True);assert check.returncode!=0
print(('HTTPS fetch verified; ' if network else '')+'PASS: ext compiles nano inside MyOS; console rendering, editor operations, reinstall skip and uninstall')
