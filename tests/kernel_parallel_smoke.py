"""Validate kernel lock separation on disposable MyFS images."""
from pathlib import Path
import subprocess,tempfile,argparse
from hardware_inventory_smoke import VM,login
import hardware_smoke
B=Path('build')
def run(cpus):
    with tempfile.TemporaryDirectory(prefix='myos-kernel-parallel-') as temp:
        root=Path(temp)/'root.img'
        with root.open('wb') as stream:stream.truncate(32*1048576)
        def fs(*args):subprocess.run([str(B/'myfs'),str(root),*map(str,args)],check=True,stdout=subprocess.DEVNULL)
        for directory in ['/bin','/lib','/etc','/root','/home','/tmp']:fs('mkdir',directory)
        for name in ['sh','kernelstat','smpbench','klogcheck']:fs('put',B/'user'/name,'/bin/'+name)
        fs('put',B/'kernel-parallel-check','/bin/kparallel')
        for name,source in [('ld.so',B/'ld.so'),('libc.so',B/'user/libc.so'),('libgfx.so',B/'user/libgfx.so')]:fs('put',source,'/lib/'+name)
        vm=VM('kernel-parallel-'+str(cpus),B/'os.img',['-drive',f'format=raw,file={root},index=1,snapshot=on'],cpus)
        try:
            login(vm);vm.send('kparallel','KERNEL PARALLEL PASS')
            vm.send('klogcheck','cursor checks: PASS');vm.send('smpbench','IPC BENCH')
            vm.send('kernelstat','still use the BKL.')
            text=hardware_smoke.clean(vm.log)
            assert 'KERNEL PARALLEL FAIL' not in text and 'PANIC' not in text,text[-8000:]
            for line in text.splitlines():
                if line.startswith(('KERNEL PARALLEL PASS','SYSCALL BENCH','IPC BENCH')):print(line,flush=True)
        finally:vm.stop()
if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--cpus',type=int,nargs='+',choices=(1,2,4,8),default=[1,2,4,8]);args=parser.parse_args()
    for cpus in args.cpus:run(cpus)
