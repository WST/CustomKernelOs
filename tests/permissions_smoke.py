"""Real syscall/DAC checks and account lifecycle on an independent QEMU disk."""
from pathlib import Path
import os,re,shutil,subprocess,tempfile,time,socket,json,tarfile,io,struct
r=Path(__file__).resolve().parents[1];b=r/'build'
def run(*a):subprocess.run([str(x) for x in a],check=True,stdout=subprocess.DEVNULL)
run('make','-j4','build/os.img','build/myfs','build/user/sh','build/user/cat','build/user/echo','build/user/cc','build/user/chmod','build/user/chown','build/user/stat','build/user/ls','build/user/cp','build/user/tar','build/user/libc.so','build/user/libgfx.so','build/ld.so','build/ccroot/.stamp','build/mlib/libc.a')
sysroot=b/'permissions-root';shutil.copytree(b/'ccroot/include',sysroot/'include',dirs_exist_ok=True);(sysroot/'lib').mkdir(exist_ok=True)
shutil.copyfile(b/'mlib/libc.a',sysroot/'lib/libc.a')
run(b/'cc-host','-B',sysroot,r/'tests/permissions_check.c','-o',b/'permissions-check')
with tempfile.TemporaryDirectory(prefix='myos-dac-') as tmp:
 d=Path(tmp);image=d/'disk.img';boot=d/'boot.img';shutil.copyfile(b/'os.img',boot)
 with image.open('wb') as f:f.truncate(64*1024*1024)
 for name in ['/bin','/lib','/etc','/home','/root','/tmp','/run']:run(b/'myfs',image,'mkdir',name)
 run(b/'myfs',image,'chmod','1777','/tmp');run(b/'myfs',image,'chmod','0700','/root')
 for name in ['sh','cat','echo','cc','chmod','chown','stat','ls','cp','tar']:run(b/'myfs',image,'put',b/'user'/name,'/bin/'+name)
 for name in ['libc.so','libgfx.so']:run(b/'myfs',image,'put',b/'user'/name,'/lib/'+name)
 run(b/'myfs',image,'put',b/'ld.so','/lib/ld.so');run(b/'myfs',image,'put',b/'mlib/libc.a','/lib/libc.a')
 run(b/'myfs',image,'put',b/'permissions-check','/bin/permissions-check')
 archive=d/'modes.tar'
 with tarfile.open(archive,'w') as tf:
  for name,mode,data in [('archive-private',0o700,None),('archive-private/secret',0o600,b'ARCHIVE_SECRET'),('archive-exec',0o755,(b/'user/echo').read_bytes())]:
   info=tarfile.TarInfo(name);info.mode=mode
   if data is None:info.type=tarfile.DIRTYPE;tf.addfile(info)
   else:info.size=len(data);tf.addfile(info,io.BytesIO(data))
 run(b/'myfs',image,'put',archive,'/tmp/modes.tar')
 source=d/'native.c';source.write_text('int main(void){return 0;}\n');run(b/'myfs',image,'put',source,'/tmp/native.c')
 orphan=None
 def disk_inode(ino):
  with image.open('rb') as f:
   f.seek(20);start=struct.unpack('<I',f.read(4))[0];f.seek(start*512+ino*64);return struct.unpack('<HH',f.read(4))
 for phase in ['first','reboot']:
  log=b/('permissions-'+phase+'.log');log.write_text('')
  q=subprocess.Popen(['qemu-system-i386','-cpu','max','-m','256M','-display','none','-no-reboot','-drive',f'format=raw,file={boot},index=0','-drive',f'format=raw,file={image},index=1','-qmp',f'unix:{d}/qmp.sock,server=on,wait=off','-serial',f'file:{log}','-monitor','stdio'],stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,text=True)
  def text():return re.sub(r'\x1b\[[0-9;?]*[A-Za-z]','',log.read_text(errors='replace')).replace('\r','')
  def wait(marker,start=0,prompt=False):
   until=time.monotonic()+40
   while time.monotonic()<until:
    tail=text()[start:]
    if tail.rstrip().endswith(marker.rstrip()) if prompt else marker in tail:return tail
    if q.poll() is not None:raise AssertionError(q.stderr.read())
    time.sleep(.1)
   with socket.socket(socket.AF_UNIX) as channel:
    channel.connect(str(d/'qmp.sock'));stream=channel.makefile('rwb');stream.readline()
    for command in [{'execute':'qmp_capabilities'},{'execute':'human-monitor-command','arguments':{'command-line':'info registers'}},{'execute':'human-monitor-command','arguments':{'command-line':'x /32wx $esp'}}]:
     stream.write((json.dumps(command)+'\n').encode());stream.flush()
     while True:
      response=json.loads(stream.readline())
      if 'return' in response or 'error' in response:print(response,flush=True);break
   raise AssertionError(marker+' missing\n'+text()[-6000:])
  def send(command,marker,prompt=False):
   start=len(text())
   for c in command+'\n':
    key={'\n':'ret',' ':'spc','/':'slash','.':'dot','-':'minus','_':'shift-minus','>':'shift-dot'}.get(c,c)
    if c.isupper():key='shift-'+c.lower()
    q.stdin.write(f'sendkey {key} 10\n');q.stdin.flush();time.sleep(.035)
   return wait(marker,start,prompt)
  try:
   if phase=='first':wait('New password:');send('review123','Confirm password:');send('review123','Login [root]:')
   else:wait('Login [root]:')
   send('','Password:');send('review123','root:/')
   if phase=='first':
    send('permissions-check','PERMISSIONS ABI PASS')
    send('cc /tmp/native.c -o /tmp/native','\nroot:/root$ ',True)
    result=send('stat /tmp/native','\nroot:/root$ ',True);assert 'mode=0755' in result,result
    send('/tmp/native','\nroot:/root$ ',True)
    send('cp /bin/echo /tmp/copied','\nroot:/root$ ',True)
    send('/tmp/copied COPY_EXEC_PASS','COPY_EXEC_PASS')
    send('tar -xf /tmp/modes.tar -C /tmp','\nroot:/root$ ',True)
    result=send('stat /tmp/archive-private','\nroot:/root$ ',True);assert 'mode=0700' in result,result
    result=send('stat /tmp/archive-private/secret','\nroot:/root$ ',True);assert 'mode=0600' in result,result
    send('/tmp/archive-exec ARCHIVE_EXEC_PASS','ARCHIVE_EXEC_PASS')
    result=send('ls -l /tmp/archive-private','\nroot:/root$ ',True);assert '-rw------- 0 0' in result,result
    send('useradd alice','New password:');send('alice123','Confirm password:');send('alice123','\nroot:/root$ ',True)
    send('echo ROOT_SECRET > /root/secret','\nroot:/root$ ',True)
    send('logout','Login [root]:');send('alice','Password:');send('alice123','alice:/')
    result=send('stat /home/alice','\nalice:/home/alice$ ',True);assert 'mode=0700 uid=1 gid=1' in result,result
    send('cat /root/secret','permission denied')
    send('cat /tmp/archive-private/secret','permission denied')
    send('umask 077','\nalice:/home/alice$ ',True);send('echo own > mine','\nalice:/home/alice$ ',True)
    result=send('stat mine','\nalice:/home/alice$ ',True);assert 'mode=0600 uid=1 gid=1' in result,result
    send('chmod 640 mine','\nalice:/home/alice$ ',True)
    send('passwd','New password:');send('changed123','Confirm password:');send('changed123','\nalice:/home/alice$ ',True)
    result=send('permissions-check orphan','ORPHAN_PENDING');orphan=int(re.search(r'ORPHAN_PENDING inode=(\d+)',result)[1])
   else:
    send('permissions-check verify','PERMISSIONS REBOOT PASS')
    send('logout','Login [root]:');send('alice','Password:');send('changed123','alice:/')
    result=send('stat mine','\nalice:/home/alice$ ',True);assert 'mode=0640 uid=1 gid=1' in result,result
    send('cat /root/secret','permission denied')
    send('cat /tmp/archive-private/secret','permission denied')
  finally:
   q.terminate()
   try:q.wait(timeout=4)
   except subprocess.TimeoutExpired:q.kill();q.wait()
  assert disk_inode(orphan)==((1,0) if phase=='first' else (0,0)),disk_inode(orphan)
  print('MyOS DAC '+phase+' and orphan recovery: PASS',flush=True)
