"""Boot real ext auto service on a disposable disk; no network downloads."""
import pathlib,re,subprocess,tempfile,time,shutil
B=pathlib.Path('build')
def command(*args,**kwargs):return subprocess.run(list(args),check=True,**kwargs)
def main():
    with tempfile.TemporaryDirectory(prefix='myos-ext-auto-') as folder:
        folder=pathlib.Path(folder);disk=folder/'data.img'
        with disk.open('wb') as f:f.truncate(128*1048576)
        command('make','-j4','build/os.img','build/user/extautolock','install','DISK='+str(disk),'EXT_AUTOINSTALL_PACKAGES=',stdout=subprocess.DEVNULL)
        def fs(*args):return command('build/myfs',str(disk),*args,stdout=subprocess.DEVNULL)
        def put(text,target):
            src=folder/'fixture';src.write_text(text);fs('put',str(src),target)
        fs('put','build/user/extautolock','/bin/extautolock')
        for name,text in {
            'autobase':'name autobase\nworkdir /var/lib/ext/autofixture\nmkdir base\n',
            'autobad':'name autobad\ninvalid_instruction\n',
            'autochild':'name autochild\ndepend autobase\nworkdir /var/lib/ext/autofixture\nmkdir child\n',
        }.items():
            fs('mkdir','/ext/instructions/'+name);put(text,'/ext/instructions/'+name+'/setup.inst')
        put('# offline boot selection\nautobad\nautochild\nautochild # duplicate\n','/etc/ext/autoinstall.conf')
        put('','/etc/services/enabled/ext-autoinstall.service')
        def boot(first):
            log=folder/'serial';log.write_text('')
            proc=subprocess.Popen(['qemu-system-i386','-m','256M','-display','none','-no-reboot','-nic','none',
                '-drive','format=raw,file=build/os.img,index=0','-drive','format=raw,file='+str(disk)+',index=1',
                '-serial','file:'+str(log),'-monitor','stdio'],stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,text=True)
            def text():return re.sub(r'\x1b\[[0-9;?]*[A-Za-z]','',log.read_text(errors='replace')).replace('\r','')
            def wait(marker,after=0,timeout=35):
                end=time.monotonic()+timeout
                while time.monotonic()<end:
                    if marker in text()[after:]:return
                    if proc.poll() is not None:raise AssertionError(proc.stderr.read()+text()[-2000:])
                    time.sleep(.05)
                raise AssertionError(marker+' missing:\n'+text()[-4000:])
            def send(line,marker):
                start=len(text())
                for char in line+'\n':
                    if char=='&':key='shift-7'
                    else:key={'\n':'ret',' ':'spc','/':'slash','.':'dot','-':'minus'}.get(char,char)
                    proc.stdin.write('sendkey '+key+' 10\n');proc.stdin.flush();time.sleep(.05)
                wait(marker,start)
                return text()[start:]
            try:
                if first:
                    wait('New password:');send('autotest123','Confirm password:');send('autotest123','Login [root]:')
                else:wait('Login [root]:')
                send('','Password:');send('autotest123','root:/')
                send('systemctl status ext-autoinstall','enabled')
                send('cat /var/log/ext-autoinstall.service','root:/')
                result=send('ext auto status','run complete')
                if first:
                    assert 'autobad failed' in result and 'autochild installed' in result,result
                    send('ls /var/lib/ext/autofixture','child')
                    send('cc /src/ext.c -o /tmp/extnative','root:/')
                    send('/tmp/extnative auto list','Selected packages: 2')
                    send('ext auto remove autobad','saved 1 packages')
                    send('mv /etc/ext/autoinstall.conf /etc/ext/autoinstall.conf.previous','root:/')
                    send('ext auto list','Selected packages: 1')
                    send('ext auto add autochild autochild','saved 1 packages')
                    send('ext auto add unknownpackage','unknown package:')
                    send('ext auto run','autochild skipped')
                    send('extautolock &','EXT AUTO LOCK HELD')
                    send('ext install autochild','another package operation is running')
                    wait('EXT AUTO LOCK RELEASED')
                    send('ext install autochild','already installed autochild')
                    send('chmod 666 /etc/ext/autoinstall.conf','root:/')
                    send('ext auto run','must be root-owned')
                    send('chmod 644 /etc/ext/autoinstall.conf','root:/')
                    send('useradd alice','New password:');send('alice123','Confirm password:');send('alice123','root:/')
                    send('logout','Login [root]:');send('alice','Password:');send('alice123','alice:/')
                    send('ext auto add autochild','authenticated root required')
                else:
                    assert 'autochild skipped' in result,result
                    send('ext auto remove autochild','saved 0 packages; boot service disabled')
                assert 'PANIC' not in text()
            finally:
                proc.terminate()
                try:proc.wait(timeout=5)
                except subprocess.TimeoutExpired:proc.kill();proc.wait()
                diag=subprocess.run(['build/myfs',str(disk),'cat','/var/log/ext-autoinstall.service'],capture_output=True,text=True)
                (B/'ext-auto-service.log').write_text(diag.stdout+diag.stderr)
                details=subprocess.run(['build/myfs',str(disk),'cat','/var/log/ext-autoinstall.log'],capture_output=True,text=True)
                (B/'ext-auto-detail.log').write_text(details.stdout+details.stderr)
                (B/('ext-auto-first.log' if first else 'ext-auto-reboot.log')).write_text(text())
        boot(True)
        # A changed recipe must not run again when the successful install stamp exists.
        put('invalid_instruction\n','/ext/instructions/autochild/setup.inst')
        boot(False)
        log=subprocess.check_output(['build/myfs',str(disk),'cat','/var/log/ext-autoinstall.log'],text=True)
        assert 'invalid_instruction' in log and 'autochild skipped' in log and 'autochild installed exit=0' in log,log
        state=subprocess.check_output(['build/myfs',str(disk),'cat','/var/lib/ext/autoinstall.status'],text=True)
        assert state=='run complete\nautochild skipped\n',state
        fs('stat','/ext/installed/autochild')
        disabled=subprocess.run(['build/myfs',str(disk),'stat','/etc/services/enabled/ext-autoinstall.service'],capture_output=True)
        assert disabled.returncode!=0
    print('PASS: boot auto-install, dependencies, failure isolation, deduplication, skip on reboot, shared lock and root/config checks')
if __name__=='__main__':main()
