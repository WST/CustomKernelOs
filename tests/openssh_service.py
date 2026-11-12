"""Check an authenticated SSH daemon, PTY, restart and ext uninstall."""
from pathlib import Path
import os,re,shutil,subprocess,tempfile,time
r=Path(__file__).resolve().parents[1];b=r/'build';image=Path((b/'openssh-ext-image.txt').read_text().strip())
port=os.environ.get('MYOS_SSH_TEST_PORT','22222')
def run(*a,**kw):
 try:return subprocess.run([str(v) for v in a],check=True,**kw)
 except subprocess.CalledProcessError as e:
  print(e.stdout,e.stderr,flush=True);raise
def readfile(name):return run(b/'myfs',image,'cat',name,capture_output=True).stdout
before={n:readfile(n) for n in ['/etc/ssh/sshd_config','/etc/ssh/ssh_host_ed25519_key']}
run(b/'myfs',image,'put',r/'ext/instructions/openssh/install.c','/tmp/openssh-install.c',stdout=subprocess.DEVNULL)
run(b/'myfs',image,'put',r/'ext/instructions/openssh/remove.c','/ext/instructions/openssh/remove.c',stdout=subprocess.DEVNULL)
# Refresh the terminal adapter in a resumed native-build fixture.
src=b/'openssh-native/openssh-10.6p2'
for name in ['config.h','myos-posix.h']:
 run(b/'myfs',image,'put',src/name,'/tmp/ext/openssh/openssh-10.6p2/'+name,stdout=subprocess.DEVNULL)
for name in ['myos-posix.c','readpass.c','openbsd-compat/readpassphrase.c','sshpty.c','monitor_fdpass.c']:
 run(b/'myfs',image,'put',src/name,'/tmp/ext/openssh/openssh-10.6p2/'+name,stdout=subprocess.DEVNULL)
cc='cc -DHAVE_CONFIG_H -I. -Imyos-include -Iopenbsd-compat'
commands=[cc+' -c '+n+' -o '+n[:-2]+'.o' for n in ['myos-posix.c','readpass.c','openbsd-compat/readpassphrase.c','sshpty.c','monitor_fdpass.c']]
commands+=['ar rc s1.a readpass.o monitor_fdpass.o','ar rc c1.a openbsd-compat/readpassphrase.o','ar rc p-sshd-session.a sshpty.o','ar rc p-sshd-auth.a sshpty.o']
commands += [line.strip().replace('$(CC)',cc) for line in (r/'ext/instructions/openssh/Makefile').read_text().splitlines() if line.startswith('\t$(CC) -o /ext/bin/')]
update=b/'openssh-tty.mk';update.write_text('all: '+ ' '.join('step'+str(i) for i in range(len(commands)))+'\n\techo SSH_TTY_UPDATE_PASS\n'+''.join('step'+str(i)+':\n\t'+line+'\n' for i,line in enumerate(commands)))
run(b/'myfs',image,'put',update,'/tmp/openssh-tty.mk',stdout=subprocess.DEVNULL)

with tempfile.TemporaryDirectory(prefix='myos-ssh-service-') as tmp:
 identity=Path(tmp)/'identity';identity.write_bytes(readfile('/root/.ssh/id_ed25519'));identity.chmod(0o600)
 boot=b/'openssh-service-boot.img';shutil.copyfile(b/'os.img',boot)
 log=b/'openssh-service.log';log.write_text('')
 q=subprocess.Popen(['qemu-system-i386','-cpu','max','-m','512M','-display','none','-no-reboot','-drive',f'cache=unsafe,format=raw,file={boot},index=0','-drive',f'cache=unsafe,format=raw,file={image},index=1','-nic',f'user,model=e1000,hostfwd=tcp:127.0.0.1:{port}-:22','-serial',f'file:{log}','-monitor','stdio'],stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,text=True)
 last=''
 def text():
  global last
  try:last=re.sub(r'\x1b\[[0-9;?]*[A-Za-z]','',log.read_text(errors='replace')).replace('\r','')
  except OSError:pass
  return last
 def wait(marker,start=0):
  end=time.monotonic()+600
  while time.monotonic()<end:
   tail=text()[start:]
   if (tail.rstrip().endswith(marker.rstrip()) if marker.startswith('\nroot:') else marker in tail):return
   time.sleep(.1)
  raise AssertionError(marker+'\n'+text()[-6000:])
 def send(s,marker):
  start=len(text())
  for c in s+'\n':
   key={'\n':'ret',' ':'spc','/':'slash','-':'minus','.':'dot','&':'shift-7','\b':'backspace','=':'equal','@':'shift-2'}.get(c,c)
   if c.isupper():key='shift-'+c.lower()
   q.stdin.write(f'sendkey {key} 10\n');q.stdin.flush();time.sleep(.03)
  wait(marker,start)
  return text()[start:]
 try:
  wait('Login [root]:');send('','Password:');send('review123','root:/')
  send('cc /tmp/openssh-install.c -o /ext/bin/openssh-setup','\nroot:/root$ ')
  send('/ext/bin/openssh-setup','OpenSSH installed.')
  send('cd /tmp/ext/openssh/openssh-10.6p2','\nroot:/tmp/ext/openssh/openssh-10.6p2$ ')
  send('make -f /tmp/openssh-tty.mk','SSH_TTY_UPDATE_PASS')
  send('cd /root','\nroot:/root$ ')
  def start():
   send('/ext/bin/sshd -D -E /var/log/ssh-test.log &','\nroot:/root$ ');time.sleep(1)
   output=send('cat /run/sshd.pid','\nroot:/root$ ')
   return int(re.findall(r'(?m)^\s*(\d+)\s*$',output)[-1])
  send('useradd sshtest','New password:')
  send('user123','Confirm password:');send('user123','\nroot:/root$ ')
  pid=start()
  send('ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null sshtest@127.0.0.1 whoami','password:')
  reply=send('user12x\b3','\nroot:/root$ ')
  assert 'sshtest' in reply and 'user12' not in reply,reply
  print('Native SSH client password prompt and hidden input: PASS',flush=True)
  opts=['-F','/dev/null','-p',port,'-i',str(identity),'-o','BatchMode=yes','-o','StrictHostKeyChecking=no','-o','UserKnownHostsFile=/dev/null']
  p=run('ssh',*opts,'-tt','root@127.0.0.1','echo SSH_PTY_PASS',capture_output=True,text=True,timeout=45)
  assert 'SSH_PTY_PASS' in p.stdout,(p.stdout,p.stderr,text()[-6000:])
  print('Authenticated server startup and remote PTY: PASS',flush=True)
  askpass=Path(tmp)/'askpass';askpass.write_text('#!/bin/sh\nprintf "%s\\n" user123\n');askpass.chmod(0o700)
  env=dict(os.environ,SSH_ASKPASS=str(askpass),SSH_ASKPASS_REQUIRE='force',DISPLAY=':myos-test')
  p=run('ssh','-F','/dev/null','-p',port,'-o','PubkeyAuthentication=no','-o','PreferredAuthentications=password','-o','StrictHostKeyChecking=no','-o','UserKnownHostsFile=/dev/null','sshtest@127.0.0.1','whoami',env=env,start_new_session=True,capture_output=True,text=True,timeout=45)
  assert 'sshtest' in p.stdout,(p.stdout,p.stderr,text()[-6000:])
  print('Non-root password login and session identity: PASS',flush=True)
  send('kill '+str(pid),'\nroot:/root$ ')
  pid=start()
  run('ssh',*opts,'root@127.0.0.1','echo SSH_RESTART_PASS',capture_output=True,text=True,timeout=45)
  send('kill '+str(pid),'\nroot:/root$ ')
  send('ext uninstall openssh','removed openssh')
 finally:
  q.terminate()
  try:q.wait(timeout=3)
  except subprocess.TimeoutExpired:q.kill();q.wait()
for n,data in before.items():assert readfile(n)==data,n+' changed on uninstall'
for name in ['ssh','sshd','sshd-session','sshd-auth','ssh-keygen','ssh-keyscan','scp','sftp','sftp-server','openssh-setup']:
 p=subprocess.run([str(b/'myfs'),str(image),'cat','/ext/bin/'+name],capture_output=True)
 assert p.returncode!=0,name+' survived uninstall'
print('Server restart/stop, ext uninstall and preserved keys/configuration: PASS')
