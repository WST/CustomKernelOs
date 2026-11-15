"""Measure wakeup latency on disposable disks, including a pre-change baseline."""
from pathlib import Path
import argparse
import subprocess
import tempfile
from hardware_inventory_smoke import VM,login
import hardware_smoke as hardware
B=Path('build')


def run(cpus,bound,label):
    with tempfile.TemporaryDirectory(prefix='myos-scheduler-') as temporary:
        root=Path(temporary)/'root.img'
        with root.open('wb') as stream:stream.truncate(32*1048576)
        def fs(*args):subprocess.run([str(B/'myfs'),str(root),*map(str,args)],check=True,stdout=subprocess.DEVNULL)
        for directory in ['/bin','/lib','/etc','/root','/home','/tmp']:fs('mkdir',directory)
        fs('put',B/'user/sh','/bin/sh');fs('put',B/'scheduler-probe','/bin/schedprobe')
        if bound:
            for name in ['dragcheck','schedstat']:
                fs('put',B/'user'/name,'/bin/'+name)
            fs('put',B/'pthread-native','/bin/pthreadcheck')
            for source,target in [(B/'ld.so','ld.so'),(B/'user/libc.so','libc.so'),(B/'user/libgfx.so','libgfx.so'),(B/'user/libpthread.so','libpthread.so')]:
                fs('put',source,'/lib/'+target)
        vm=VM('scheduler-'+label+'-'+str(cpus),B/'os.img',[
            '-drive',f'format=raw,file={root},index=1,snapshot=on'],cpus)
        try:
            login(vm);vm.send('schedprobe '+str(bound)+(' gui' if bound else ''),'SCHED PROBE PASS')
            if bound:
                vm.send('pthreadcheck','pthread: ok')
                vm.send('schedstat','timer resolution 10 ms')
            text=hardware.clean(vm.log)
            assert 'SCHED PROBE FAIL' not in text and 'PANIC' not in text,text[-8000:]
            print(f'{label}, {cpus} CPU(s): '+next(line for line in text.splitlines() if line.startswith('SCHED METRICS')),flush=True)
        finally:vm.stop()


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--baseline',action='store_true')
    parser.add_argument('--cpus',type=int,nargs='+',choices=(1,2,4,8),default=[1,2,4,8])
    parser.add_argument('--repeat',type=int,default=1)
    args=parser.parse_args()
    if args.baseline:run(1,0,'round-robin')
    else:
        for _ in range(args.repeat):
            for cpus in args.cpus:run(cpus,60,'mlfq')
