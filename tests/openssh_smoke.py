"""Boot a fresh test disk; native SSH ABI and OpenSSH client/server validation."""
from pathlib import Path
import subprocess,re,time,shutil,os,tempfile,sys,select
ROOT=Path(__file__).resolve().parents[1]
b=ROOT/'build';resume='--resume-ext' in sys.argv;ext_mode='--ext' in sys.argv or resume
image=Path((b/'openssh-ext-image.txt').read_text().strip()) if resume else (Path(tempfile.mkdtemp(prefix='myos-openssh-disk-'))/'disk.img' if ext_mode else b/'openssh-test.img')
bins=b/'openssh-native/bin'
port=os.environ.get('MYOS_SSH_TEST_PORT','22222')
if ext_mode:(b/'openssh-ext-image.txt').write_text(str(image))
def run(*args):return subprocess.run([str(a) for a in args],check=True,stdout=subprocess.DEVNULL)
if not resume:
 with image.open('wb') as f:f.truncate(128*1024*1024)
 run('make','-j4','build/os.img','install','DISK='+str(image))
 def put(local,remote):run(b/'myfs',image,'put',local,remote)
 for d in ['/etc/ssh','/var/empty','/root/.ssh']:run(b/'myfs',image,'mkdir',d)
 if ext_mode:
  for d in ['/tmp/ext','/tmp/ext/openssh']:run(b/'myfs',image,'mkdir',d)
  source=ROOT/'build/openssh-port/openssh.tar.gz'
  if not source.exists():source=b/'openssh-native/openssh-10.6p2.tar.gz'
  put(source,'/tmp/ext/openssh/openssh-10.6p2.tar.gz')
  # Offline fixture changes only transport; source has already passed SHA256 validation.
  recipe=b/'openssh-offline.inst'
  recipe.write_text('\n'.join('# preloaded verified source' if line.startswith('fetch ') else line for line in (ROOT/'ext/instructions/openssh/setup.inst').read_text().splitlines())+'\n')
  put(recipe,'/ext/instructions/openssh/setup.inst')
 else:
  for f in bins.iterdir():put(f,'/ext/bin/'+f.name)
 put(b/'openssh-native/sshcheck','/bin/sshcheck')
 config=b/'openssh-test.conf';config.write_text('''Port 22
 AddressFamily inet
 ListenAddress 0.0.0.0
 HostKey /etc/ssh/ssh_host_ed25519_key
 PermitRootLogin yes
 PasswordAuthentication yes
 KbdInteractiveAuthentication no
 LoginGraceTime 0
 MaxStartups 2
 LogLevel INFO
 Subsystem sftp /ext/bin/sftp-server
 ''');put(config,'/etc/ssh/sshd_config')
 boot=image.with_name('boot.img') if ext_mode else b/'openssh-boot.img';shutil.copyfile(b/'os.img',boot)
 testdir=tempfile.TemporaryDirectory(prefix='myos-openssh-')
 identity=Path(testdir.name)/'identity'
 if not identity.exists():run('ssh-keygen','-q','-t','ed25519','-N','','-f',identity)
 put(identity,'/root/.ssh/id_ed25519')
 run(b/'myfs',image,'chmod','0600','/root/.ssh/id_ed25519')
 put(str(identity)+'.pub','/root/.ssh/authorized_keys')
else:
 run('make','build/os.img')
 boot=image.with_name('boot.img');shutil.copyfile(b/'os.img',boot)
 testdir=tempfile.TemporaryDirectory(prefix='myos-openssh-')
 identity=Path(testdir.name)/'identity'
 identity.write_bytes(subprocess.run([str(b/'myfs'),str(image),'cat','/root/.ssh/id_ed25519'],capture_output=True,check=True).stdout);identity.chmod(0o600)
 recipe=b/'openssh-resume.inst'
 recipe.write_text('\n'.join('# verified source already extracted' if line.startswith(('fetch ','tar ','patch ')) else line for line in (ROOT/'ext/instructions/openssh/setup.inst').read_text().splitlines())+'\n')
 run(b/'myfs',image,'put',recipe,'/ext/instructions/openssh/setup.inst')
log=image.with_suffix('.log') if ext_mode else b/'openssh-test.log' ;log.write_text('')
q=subprocess.Popen(['qemu-system-i386','-cpu','max','-m','512M','-display','none','-no-reboot',
 '-drive',f'cache=unsafe,format=raw,file={boot},index=0','-drive',f'cache=unsafe,format=raw,file={image},index=1',
 '-nic',f'user,model=e1000,hostfwd=tcp:127.0.0.1:{port}-:22', '-serial',f'file:{log}','-monitor','stdio'],
 stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,text=True)
last_text=''
def text():
 global last_text
 try:last_text=re.sub(r'\x1b\[[0-9;?]*[A-Za-z]','',log.read_text(errors='replace')).replace('\r','')
 except OSError:pass
 return last_text
def wait(marker,start=0,timeout=40):
 end=time.monotonic()+timeout
 while time.monotonic()<end:
  if marker in text()[start:]:return
  if q.poll() is not None:raise AssertionError(q.stderr.read())
  time.sleep(.1)
 raise AssertionError(marker+' missing\n'+text()[-5000:])
def send(s,marker,timeout=40):
 start=len(text())
 for c in s+'\n':
  key={'\n':'ret',' ':'spc','/':'slash','.':'dot','-':'minus','_':'shift-minus',':':'shift-semicolon','>':'shift-dot','"':'shift-apostrophe'}.get(c,c)
  if c.isupper():key='shift-'+c.lower()
  q.stdin.write(f'sendkey {key} 10\n');q.stdin.flush();time.sleep(.03)
 wait(marker,start,timeout)
try:
 if not resume:
  wait('New password:');send('review123','Confirm password:');send('review123','Login [root]:')
 else:wait('Login [root]:')
 send('','Password:');send('review123','root:/')
 send('useradd sshuser','New password:')
 send('user123','Confirm password:');send('user123','root:/')
 send('sshcheck','SSH ABI PASS')
 if ext_mode:send('ext install openssh','OpenSSH installed.',7200)
 send('ssh -V','OpenSSH_10.6p2')
 if not ext_mode:send('ssh-keygen -q -t ed25519 -N "" -f /etc/ssh/ssh_host_ed25519_key','\nroot:/root$ ',120)
 send('/ext/bin/sshd -t','\nroot:/root$ ')
 send('/ext/bin/sshd -D -e','Server listening on',60)
 print('Native OpenSSH ABI, key generation and daemon startup: PASS',flush=True)
 result=subprocess.run(['ssh-keyscan','-t','ed25519','-T','30','-p',port,'127.0.0.1'],capture_output=True,text=True,timeout=40)
 assert 'ssh-ed25519' in result.stdout,result.stderr+'\n'+text()[-5000:]
 print('OpenSSH host key exchange: PASS',flush=True)
 options=['-F','/dev/null','-p',port,'-o','StrictHostKeyChecking=no','-o','UserKnownHostsFile=/dev/null','-o','ConnectTimeout=15']
 keyopts=options+['-i',str(identity),'-o','BatchMode=yes']
 def ssh(command,opts=keyopts):
  result=subprocess.run(['ssh']+opts+['root@127.0.0.1',command],capture_output=True,text=True,timeout=45)
  return result
 result=ssh('echo SSH_SERVER_PASS')
 assert result.returncode==0 and 'SSH_SERVER_PASS' in result.stdout,result.stderr+'\n'+text()[-7000:]
 print('Remote command and public key authentication: PASS',flush=True)
 result=ssh('ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o BatchMode=yes root@127.0.0.1 echo SSH_CLIENT_PASS')
 assert result.returncode==0 and 'SSH_CLIENT_PASS' in result.stdout,result.stderr+'\n'+result.stdout+'\n'+text()[-7000:]
 print('Native SSH client to native SSH server: PASS',flush=True)
 result=ssh('exit 7');assert result.returncode==7,(result.returncode,result.stderr,text()[-7000:])
 print('Remote exit status: PASS',flush=True)
 askpass=Path(testdir.name)/'askpass.sh';askpass.write_text('#!/bin/sh\nprintf "%s\n" "$MYOS_SSH_TEST_PASSWORD"\n');askpass.chmod(0o700)
 for password,allowed in [('wrong-password',False),('review123',True)]:
  env=dict(os.environ,SSH_ASKPASS=str(askpass),SSH_ASKPASS_REQUIRE='force',DISPLAY=':myos-test',MYOS_SSH_TEST_PASSWORD=password)
  result=subprocess.run(['ssh']+options+['-o','PubkeyAuthentication=no','-o','PreferredAuthentications=password','-o','NumberOfPasswordPrompts=1','root@127.0.0.1','echo SSH_PASSWORD_PASS'],env=env,start_new_session=True,capture_output=True,text=True,timeout=45)
  assert (result.returncode==0)==allowed,(password,result.stderr,text()[-7000:])
  if allowed:assert 'SSH_PASSWORD_PASS' in result.stdout
 print('Valid and invalid password authentication: PASS',flush=True)
 def interactive_login(user,opts,env=None):
  session=subprocess.Popen(['ssh']+opts+['-tt',user+'@127.0.0.1'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,env=env)
  output=''
  def received(marker):
   nonlocal output
   deadline=time.monotonic()+45
   while time.monotonic()<deadline:
    if marker in output:return
    ready,_,_=select.select([session.stdout],[],[],.1)
    if ready:
     chunk=os.read(session.stdout.fileno(),4096)
     if not chunk:raise AssertionError('SSH session closed: '+output+'\n'+text()[-5000:])
     output+=re.sub(r'\x1b\[[0-9;?]*[A-Za-z]','',chunk.decode(errors='replace')).replace('\r','')
    elif session.poll() is not None:raise AssertionError('SSH exited: '+output)
   raise AssertionError(marker+' missing in SSH session: '+output+'\n'+text()[-5000:])
  try:
   home='/root' if user=='root' else '/home/'+user
   received(user+':'+home+'$ ')
   assert 'Login [root]:' not in output and 'Password:' not in output,output
   output='';session.stdin.write(b'whoami\n');session.stdin.flush();received('\n'+user+'\n')
   # Explicit logout must still require fresh credentials.
   output='';session.stdin.write(b'logout\n');session.stdin.flush();received('Login [root]:')
  finally:
   session.terminate()
   try:session.wait(timeout=5)
   except subprocess.TimeoutExpired:session.kill();session.wait()
 interactive_login('root',keyopts)
 passwordopts=options+['-o','PubkeyAuthentication=no','-o','PreferredAuthentications=password','-o','NumberOfPasswordPrompts=1']
 env=dict(os.environ,SSH_ASKPASS=str(askpass),SSH_ASKPASS_REQUIRE='force',DISPLAY=':myos-test',MYOS_SSH_TEST_PASSWORD='user123')
 interactive_login('sshuser',passwordopts,env)
 print('Interactive SSH root/user sessions inherit identity without a second login; logout still prompts: PASS',flush=True)

 payload=b/'openssh-upload.bin';download=b/'openssh-download.bin';payload.write_bytes(bytes(range(256))*12)
 batch=b/'openssh-sftp.batch';batch.write_text(f'put {payload} /tmp/openssh-upload.bin\nchmod 600 /tmp/openssh-upload.bin\nget /tmp/openssh-upload.bin {download}\n')
 # sftp uses uppercase -P for the port.
 result=subprocess.run(['sftp','-F','/dev/null','-P',port,'-i',str(identity),'-o','BatchMode=yes','-o','StrictHostKeyChecking=no','-o','UserKnownHostsFile=/dev/null','-b',str(batch),'root@127.0.0.1'],capture_output=True,text=True,timeout=45)
 assert result.returncode==0,result.stderr+'\n'+text()[-7000:]
 assert download.read_bytes()==payload.read_bytes()
 result=ssh('stat /tmp/openssh-upload.bin');assert result.returncode==0 and 'mode=0600 uid=0 gid=0' in result.stdout,(result.stdout,result.stderr)
 print('SFTP upload/download and persistent chmod metadata: PASS',flush=True)
finally:
 q.terminate()
 try:q.wait(timeout=3)
 except subprocess.TimeoutExpired:q.kill();q.wait()
