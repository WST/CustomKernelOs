"""Build the ext OpenSSH recipe with MyOS cc, never the host libc.

Run from the repo root: python3 tests/openssh_build.py
The archive checksum is from https://www.openssh.org/txt/release-10.6p2.
"""
from pathlib import Path
import concurrent.futures, hashlib, shutil, subprocess, sys, tarfile, urllib.request
ROOT=Path(__file__).resolve().parents[1]
PACKAGE=ROOT/'ext/instructions/openssh'
BUILD=ROOT/'build/openssh-native'
VERSION='10.6p2'
SHA256='9ae44872090cf2f06b3cf9ff147e7f16f309671340763ed2f99248398ae29d5b'

def run(args, **kw):
    return subprocess.run([str(a) for a in args],check=True,**kw)

def main():
    BUILD.mkdir(exist_ok=True)
    archive=ROOT/'build/openssh-port/openssh.tar.gz'
    if not archive.exists():
        archive=BUILD/f'openssh-{VERSION}.tar.gz'
        if not archive.exists():
            urllib.request.urlretrieve(f'https://cdn.openbsd.org/pub/OpenBSD/OpenSSH/portable/openssh-{VERSION}.tar.gz',archive)
    assert hashlib.sha256(archive.read_bytes()).hexdigest()==SHA256,'OpenSSH source checksum mismatch'
    src=BUILD/f'openssh-{VERSION}'
    if not src.exists():
        with tarfile.open(archive) as tf:
            tf.extractall(BUILD,filter='data')
    restored=set()
    for line in (PACKAGE/'setup.inst').read_text().splitlines():
        a=line.split()
        if not a:continue
        if a[0]=='copy' and not a[2].startswith('/'):
            dest=src/a[2];dest.parent.mkdir(parents=True,exist_ok=True)
            data=(PACKAGE/a[1]).read_bytes()
            if not dest.exists() or dest.read_bytes()!=data:dest.write_bytes(data)
        elif a[0]=='patch':
            # Restore patched inputs so the recipe is repeatable.
            patch=(PACKAGE/a[-1]).read_text()
            import re
            files=re.findall(r'^--- a/(.+)$',patch,re.M)
            with tarfile.open(archive) as tf:
                for name in files:
                    if name not in restored:
                        (src/name).write_bytes(tf.extractfile(f'openssh-{VERSION}/{name}').read())
                        restored.add(name)
            run(['patch','-p1','-i',PACKAGE/a[-1]],cwd=src)
    target=BUILD/'sysroot';(target/'lib').mkdir(parents=True,exist_ok=True)
    shutil.copytree(ROOT/'build/ccroot/include',target/'include',dirs_exist_ok=True)
    shutil.copyfile(ROOT/'build/mlib/libc.a',target/'lib/libc.a')
    cc=[ROOT/'build/cc-host','-B',target,'-DHAVE_CONFIG_H','-I.','-Imyos-include','-Iopenbsd-compat']
    import re
    make=(PACKAGE/'Makefile').read_text()
    objects=[(o,o[:-2]+'.c') for o in dict.fromkeys(re.findall(r'[A-Za-z0-9_][A-Za-z0-9_./-]*\.o\b',make))]
    # Every object rebuilds when a port header or patch changes.
    port_headers=list(PACKAGE.glob('*.h'))+list((ROOT/'user/include').rglob('*.h'))+[ROOT/'user/lib/myos.h']
    port_time=max(f.stat().st_mtime for f in port_headers)
    def compile_one(pair):
        obj,c=pair;dest=src/obj;log=BUILD/(obj.replace('/','_')+'.log')
        if dest.exists() and dest.stat().st_mtime>max(port_time,(src/c).stat().st_mtime):return None
        with log.open('w') as out:
            p=subprocess.run([str(a) for a in cc+['-c',c,'-o',obj]],cwd=src,stdout=out,stderr=subprocess.STDOUT)
        return (obj,log.read_text()) if p.returncode else None
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        errors=[e for e in pool.map(compile_one,objects) if e]
    if errors:
        for obj,error in errors:print(obj+'\n'+error[:2000])
        raise SystemExit(f'{len(errors)} of {len(objects)} translation units failed')
    print(f'Compiled {len(objects)} OpenSSH translation units',flush=True)
    # Archives and link commands are taken directly from the installed recipe.
    bins=BUILD/'bin';bins.mkdir(exist_ok=True)
    for line in make.splitlines():
        if line.startswith('\tar rc '):run(line.strip().split(),cwd=src,stdout=subprocess.DEVNULL)
    for line in make.splitlines():
        if line.startswith('\t$(CC) -o /ext/bin/'):
            args=line.strip().split()[1:];args[1]=str(bins/Path(args[1]).name)
            run(cc+args,cwd=src)
    run([ROOT/'build/cc-host','-B',target,'install.c','-o',bins/'openssh-setup'],cwd=src)
    print('OpenSSH native client and server build: PASS')

if __name__=='__main__':main()
