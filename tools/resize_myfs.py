"""Copy a MyFS image into a larger image, checking every file before delivery.
Usage: python3 tools/resize_myfs.py disk.img build/larger.img 16
The source is untouched. Stop QEMU before running this tool.
"""
import pathlib, re, subprocess, sys, tempfile

def main():
    if len(sys.argv)!=4: raise SystemExit(__doc__)
    source,target=map(pathlib.Path,sys.argv[1:3]); size=int(sys.argv[3])*1024*1024
    if target.exists(): raise SystemExit('Target already exists; choose another name')
    if size<=source.stat().st_size: raise SystemExit('New size must exceed the source size')
    tool=str(pathlib.Path('build/myfs'))
    def run(image,*args): return subprocess.check_output([tool,str(image),*args])
    def entries(directory):
        for line in run(source,'ls',directory).decode().splitlines():
            match=re.fullmatch(r'\s*(<DIR>|\d+)  (.+)',line)
            if not match: raise RuntimeError('Unexpected directory entry: '+line)
            kind,name=match.groups()
            if kind=='<DIR>': name=name.removesuffix('/')
            yield kind,directory.rstrip('/')+'/'+name
    files=0
    with target.open('xb') as f: f.truncate(size)
    try:
        with tempfile.TemporaryDirectory(dir='build') as temp:
            scratch=pathlib.Path(temp)/'file'
            def copy(directory):
                nonlocal files
                run(target,'mkdir',directory)
                for kind,path in entries(directory):
                    if path=='/dev': continue  # VFS synthesizes device nodes; they are not disk files.
                    if kind=='<DIR>': copy(path)
                    else:
                        data=run(source,'cat',path); scratch.write_bytes(data)
                        run(target,'put',str(scratch),path)
                        if run(target,'cat',path)!=data: raise RuntimeError('Verification failed: '+path)
                        files+=1
            copy('/')
    except BaseException:
        target.unlink(missing_ok=True)
        raise
    print(f'Copied and verified {files} files into {target} ({size//1048576} MiB). Source unchanged.')
if __name__=='__main__': main()
