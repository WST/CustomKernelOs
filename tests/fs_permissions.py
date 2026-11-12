"""Exercise new volumes and byte-built, unextended MyFS v1/v2 images."""
from pathlib import Path
import struct,subprocess,tempfile
r=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='myos-permissions-') as tmp:
 for version,inode_count,mb in [(0,256,2),(1,256,2),(2,256,2),(2,8192,8)]:
  p=Path(tmp)/f'v{version}-{inode_count}.img'
  with p.open('wb') as f:f.truncate(mb*1024*1024)
  if version:
   sectors=mb*2048;bitmap_blocks=(sectors+4095)//4096;inode_start=1+bitmap_blocks;inode_blocks=inode_count//8;data_start=inode_start+inode_blocks
   legacy_inode=4097 if inode_count>4096 else 2
   data=bytearray((data_start+2)*512)
   struct.pack_into('<9I',data,0,0x5346594d,version,sectors,1,bitmap_blocks,inode_start,inode_blocks,inode_count,data_start)
   for block in range(data_start+2):data[512+block//8]|=1<<(block%8)
   entries=b''
   for ino,name in [(1,b'.'),(1,b'..'),(legacy_inode,b'legacy')]:
    if version==1:entries+=struct.pack('<I28s',ino,name)
    else:
     n=(8+len(name)+3)&~3;entries+=struct.pack('<IHBB',ino,n,len(name),0)+name+b'\0'*(n-8-len(name))
   struct.pack_into('<HHI14I',data,inode_start*512+64,2,1,len(entries),data_start,*([0]*13))
   struct.pack_into('<HHI14I',data,inode_start*512+legacy_inode*64,1,1,8,data_start+1,*([0]*13))
   data[data_start*512:data_start*512+len(entries)]=entries;data[(data_start+1)*512:(data_start+1)*512+8]=b'legacy!\n'
   with p.open('r+b') as f:f.write(data)
  subprocess.run([str(r/'build/fs-permissions-test'),str(p)],check=True)
  paths=['/','/private','/private/secret','/shared','/shared/kept','/shared/persisted']+(['/legacy'] if version else [])
  def attributes(path):return subprocess.check_output([str(r/'build/myfs'),str(p),'stat',path],text=True).split(' size=')[0]
  meta={path:attributes(path) for path in paths}
  assert 'mode=0640 uid=11 gid=11' in meta['/shared/kept'],meta
  assert 'mode=0777 uid=11 gid=11' in meta['/shared/persisted'],meta
  subprocess.run([str(r/'build/myfs'),str(p),'grow',str(mb+1)],check=True,stdout=subprocess.DEVNULL)
  for path in paths:assert attributes(path)==meta[path],(path,attributes(path),meta[path])
  print(f'MyFS {version or "new"}, {inode_count} inodes: DAC, remount, grow and symlink attributes PASS')

 # Apply the actual installation migration rule to deliberately unsafe old keys.
 for path in ['/root','/tmp','/etc/ssh','/root/.ssh']:
  subprocess.run([str(r/'build/myfs'),str(p),'mkdir',path],check=True,stdout=subprocess.DEVNULL)
 key=Path(tmp)/'old-key';key.write_bytes(b'legacy private key fixture\n')
 for path in ['/etc/ssh/ssh_host_ed25519_key','/root/.ssh/id_ed25519']:
  subprocess.run([str(r/'build/myfs'),str(p),'put',str(key),path],check=True,stdout=subprocess.DEVNULL)
  subprocess.run([str(r/'build/myfs'),str(p),'chown','11','11',path],check=True)
 subprocess.run(['make','--no-print-directory','install-permissions','DISK='+str(p)],cwd=r,check=True)
 for path in ['/etc/ssh/ssh_host_ed25519_key','/root/.ssh/id_ed25519']:
  assert 'mode=0600 uid=0 gid=0' in attributes(path)
  assert subprocess.check_output([str(r/'build/myfs'),str(p),'cat',path])==key.read_bytes()
 assert 'mode=0700 uid=0 gid=0' in attributes('/root')
 assert 'mode=1777 uid=0 gid=0' in attributes('/tmp')
 print('Install migration: private key owner/mode and contents preserved PASS')
