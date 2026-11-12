"""Cross-identity IPC and real stack faults on independent 1/4-CPU QEMU disks."""
from pathlib import Path
import re,subprocess,shutil,tempfile,time
r=Path(__file__).resolve().parents[1];b=r/'build'
def run(*args):subprocess.run([str(x) for x in args],cwd=r,check=True,stdout=subprocess.DEVNULL)
run('make','-j4','build/os.img','build/myfs','build/user/sh','build/mlib/libc.a','build/ccroot/.stamp')
sysroot=b/'isolation-root';shutil.copytree(b/'ccroot/include',sysroot/'include',dirs_exist_ok=True);(sysroot/'lib').mkdir(exist_ok=True);shutil.copyfile(b/'mlib/libc.a',sysroot/'lib/libc.a')
run(b/'cc-host','-B',sysroot,r/'tests/isolation_check.c','-o',b/'isolation-check')
for cpus in [1,4]:
 with tempfile.TemporaryDirectory(prefix='myos-isolation-') as tmp:
  d=Path(tmp);image=d/'disk.img';boot=d/'boot.img';shutil.copyfile(b/'os.img',boot)
  with image.open('wb') as f:f.truncate(32*1024*1024)
  for path in ['/bin','/etc','/root','/home','/tmp']:run(b/'myfs',image,'mkdir',path)
  run(b/'myfs',image,'put',b/'user/sh','/bin/sh');run(b/'myfs',image,'put',b/'isolation-check','/bin/isolation-check')
  log=b/f'isolation-{cpus}.log';log.write_text('')
  q=subprocess.Popen(['qemu-system-i386','-cpu','max','-smp',str(cpus),'-m','256M','-display','none','-no-reboot','-nic','none','-drive',f'format=raw,file={boot},index=0','-drive',f'format=raw,file={image},index=1','-serial',f'file:{log}','-monitor','stdio'],stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,text=True)
  def text():
   try:content=log.read_text(errors='replace')
   except OSError as error:
    if error.errno!=61:raise
    return ''  # transient WSL/Windows sharing while QEMU flushes the serial log
   return re.sub(r'\x1b\[[0-9;?]*[A-Za-z]','',content).replace('\r','')
  def wait(marker,start=0):
   end=time.monotonic()+90
   while time.monotonic()<end:
    s=text()[start:]
    if marker in s:return
    if 'ISOLATION FAIL' in s:raise AssertionError(s[-8000:])
    if q.poll() is not None:raise AssertionError(q.stderr.read())
    time.sleep(.1)
   raise AssertionError(marker+' missing\n'+text()[-8000:])
  def send(command,marker):
   start=len(text())
   for ch in command+'\n':
    key={'\n':'ret',' ':'spc','-':'minus','/':'slash'}.get(ch,ch)
    q.stdin.write(f'sendkey {key} 10\n');q.stdin.flush();time.sleep(.04)
   wait(marker,start)
  try:
   wait('New password:');send('review123','Confirm password:');send('review123','Login [root]:');send('','Password:');send('review123','root:/');send('isolation-check','ISOLATION PASS')
   assert 'ISOLATION FAIL' not in text(),text()[-8000:]
  finally:
   q.terminate()
   try:q.wait(timeout=4)
   except subprocess.TimeoutExpired:q.kill();q.wait()
  print(f'IPC ownership and stack guards, {cpus} CPU(s): PASS',flush=True)
