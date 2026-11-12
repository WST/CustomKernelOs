"""Validate the complete ext build graph with the actual MyOS make parser."""
from pathlib import Path
import re,subprocess,tempfile
r=Path(__file__).resolve().parents[1]
p=r/'ext/instructions/openssh'
make=(p/'Makefile').read_text()
objects=list(dict.fromkeys(re.findall(r'[A-Za-z0-9_][A-Za-z0-9_./-]*\.o\b',make)))
with tempfile.TemporaryDirectory(prefix='myos-openssh-recipe-') as tmp:
 d=Path(tmp)
 for name in [o[:-2]+'.c' for o in objects]+['config.h','myos-posix.h','install.c']:
  f=d/name;f.parent.mkdir(parents=True,exist_ok=True);f.touch()
 (d/'Makefile').write_text(make)
 result=subprocess.run([str(r/'build/make-linux'),'-n'],cwd=d,capture_output=True,text=True,check=True)
 lines=result.stdout.splitlines()
 assert sum(' -c ' in line for line in lines)==len(objects),(len(objects),result.stdout[-3000:])
 assert sum(' -o /ext/bin/' in line for line in lines)==10,result.stdout[-3000:]
 for line in lines:
  assert len(line)<256,('kernel argv/sh limit exceeded',line)
  assert len(line.split())<=64,('kernel argument count exceeded',line)
 for name in objects+re.findall(r'^([^\s:]+\.a):',make,re.M):
  (d/name).touch()
 again=subprocess.run([str(r/'build/make-linux'),'-n'],cwd=d,capture_output=True,text=True,check=True)
 assert sum(' -c ' in line for line in again.stdout.splitlines())==len(objects),'Reinstall reused stale objects'
 print(f'OpenSSH ext recipe: {len(objects)} sources, 10 programs, clean/reinstall native make: PASS')
