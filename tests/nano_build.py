"""Compile the real nano sources and port using the MyOS native C compiler."""
import hashlib,pathlib,shutil,subprocess,tarfile,urllib.request
ROOT=pathlib.Path(__file__).resolve().parents[1]
PORT=ROOT/'ext/instructions/nano';BUILD=ROOT/'build/nano-port'
SHA='3208f3c459c64e4a4e0ef242d2653572a56ca9a6cfbdc4de5af4714594ae8836'
def main():
    BUILD.mkdir(parents=True,exist_ok=True);archive=BUILD/'nano-9.3.tar.gz'
    if not archive.exists() or not archive.stat().st_size:
        with urllib.request.urlopen('https://ftp.gnu.org/gnu/nano/nano-9.3.tar.gz',timeout=45) as source:archive.write_bytes(source.read())
    if hashlib.sha256(archive.read_bytes()).hexdigest()!=SHA:raise SystemExit('nano source SHA256 mismatch')
    source=BUILD/'nano-9.3'
    with tarfile.open(archive) as stream:
        if not source.exists():stream.extractall(BUILD,filter='data')
        for name in ['files.c','nano.c']:
            (source/'src'/name).write_bytes(stream.extractfile('nano-9.3/src/'+name).read())
    for line in (PORT/'setup.inst').read_text().splitlines():
        words=line.split()
        if words and words[0]=='copy':
            target=source/words[2];target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes((PORT/words[1]).read_bytes())
    subprocess.run(['patch','-p1','-i',str(PORT/'myos.patch')],cwd=source,check=True,stdout=subprocess.DEVNULL)
    target=BUILD/'sysroot';(target/'lib').mkdir(parents=True,exist_ok=True)
    shutil.copytree(ROOT/'build/ccroot/include',target/'include',dirs_exist_ok=True)
    shutil.copyfile(ROOT/'build/mlib/libc.a',target/'lib/libc.a')
    cc=[str(ROOT/'build/cc-host'),'-B',str(target),'-DHAVE_CONFIG_H','-I.','-Imyos-include','-Isrc']
    objects=[]
    for name in 'browser chars color cut files global help history move nano prompt rcfile search text utils winio myos-posix myos-curses'.split():
        src=name+'.c' if name.startswith('myos-') else 'src/'+name+'.c';out=name+'.o';objects.append(out)
        result=subprocess.run(cc+['-c',src,'-o',out],cwd=source,capture_output=True,text=True)
        if result.returncode:raise SystemExit(name+':\n'+result.stdout+result.stderr)
    result=subprocess.run(cc+objects+['-o',str(BUILD/'nano')],cwd=source,capture_output=True,text=True)
    if result.returncode:raise SystemExit(result.stdout+result.stderr)
    print('PASS: GNU nano 9.3 compiled and linked by MyOS cc (18 translation units)')
if __name__=='__main__':main()
