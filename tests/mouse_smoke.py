"""Check pointer direction, absolute mapping, wheel/buttons, resize and PS/2 fallback."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
sys.dont_write_bytecode = True
sys.path.insert(0, str(Path('tools').resolve()))
from run_qemu import select

parser = argparse.ArgumentParser()
parser.add_argument('--backend', choices=('gpu','tablet','virtio','ps2','all'),default='all')
parser.add_argument('--latency-only',action='store_true',help='measure input wakeups without interactive pointer sampling')
parser.add_argument('--boot',help='compare an older compatible kernel image')
args = parser.parse_args()
b = Path('build/mouse-test')
subprocess.run(['make', '-j4','BUILD='+str(b),str(b/'os.img'),str(b/'myfs'),str(b/'user/mousecheck'),str(b/'user/inputlatency')],check=True,stdout=subprocess.DEVNULL)
original = Path(os.environ.get('MYOS_TEST_DISK','disk.img'))
for backend in (('gpu','tablet','virtio','ps2') if args.backend=='all' else (args.backend,)):
    image=b/(backend+'.img'); boot=b/(backend+'-boot.img'); log=b/(backend+'.log')
    shutil.copyfile(original,image); shutil.copyfile(args.boot or b/'os.img',boot)
    for name in ('/etc/accounts','/etc/services/enabled/desktop.service'):
        subprocess.run([str(b/'myfs'),str(image),'rm',name],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    subprocess.run([str(b/'myfs'),str(image),'put',str(b/'user/mousecheck'),'/bin/mousecheck'],check=True,stdout=subprocess.DEVNULL)
    subprocess.run([str(b/'myfs'),str(image),'put',str(b/'user/inputlatency'),'/bin/inputlatency'],check=True,stdout=subprocess.DEVNULL)
    env=dict(os.environ)
    if backend=='gpu':
        video,env,reason=select('qemu-system-i386','on'); print(reason,flush=True)
    else:
        video=['-vga','virtio' if backend=='virtio' else 'std','-display','none']
        if backend in ('tablet','virtio'): video+=['-device','virtio-tablet-pci']
    with tempfile.TemporaryDirectory(prefix='myos-mouse-') as temporary:
        endpoint=Path(temporary)/'qmp'
        error=(b/(backend+'.err')).open('w')
        p=subprocess.Popen(['qemu-system-i386','-smp','4','-m','256M',*video,
            '-drive',f'format=raw,file={boot},index=0','-drive',f'format=raw,file={image},index=1',
            '-trace',f'enable=virtio_gpu_update_cursor,file={b / (backend+'.cursor.trace')}',
            '-serial',f'file:{log}','-qmp',f'unix:{endpoint},server=on,wait=off','-monitor','none','-no-reboot'],
            env=env,stdout=subprocess.DEVNULL,stderr=error)
        def output():
            try: return re.sub(r'\x1b\[[0-9;?]*[A-Za-z]', '', log.read_text(errors='replace')).replace('\r','')
            except OSError: return ''
        def wait(marker):
            end=time.monotonic()+35
            while time.monotonic()<end:
                data=output()
                assert 'LATENCY FAIL' not in data and 'MOUSECHECK FAIL' not in data and 'PANIC' not in data,data[-2000:]
                if marker in data: return
                assert p.poll() is None,data[-2000:]
                time.sleep(.05)
            raise AssertionError('waiting for '+marker+'\n'+output()[-2500:])
        try:
            end=time.monotonic()+10
            while not endpoint.exists():
                assert time.monotonic()<end,'QMP not ready'; time.sleep(.05)
            connection=socket.socket(socket.AF_UNIX); connection.settimeout(10); connection.connect(str(endpoint))
            stream=connection.makefile('rwb')
            json.loads(stream.readline())
            def qmp(command,arguments=None):
                packet={'execute':command}
                if arguments is not None: packet['arguments']=arguments
                stream.write(json.dumps(packet).encode()+b'\n'); stream.flush()
                while True:
                    result=json.loads(stream.readline())
                    if 'event' in result: continue
                    assert 'error' not in result,result
                    return result.get('return')
            qmp('qmp_capabilities')
            def key(k):
                qmp('human-monitor-command',{'command-line':'sendkey '+k+' 40'}); time.sleep(.10)
            def send(text):
                for ch in text: key({'\n':'ret',' ':'spc','/':'slash','-':'minus'}.get(ch,ch))
            def inject(events):
                qmp('input-send-event',{'events':events}); time.sleep(.06)
            def move(x,y):
                kind='rel' if backend=='ps2' else 'abs'
                inject([{'type':kind,'data':{'axis':'x','value':x}}, {'type':kind,'data':{'axis':'y','value':y}}])
            def button(name,down): inject([{'type':'btn','data':{'button':name,'down':down}}])
            def state():
                before=len(re.findall(r'MOUSECHECK STATE',output())); key('n')
                end=time.monotonic()+5
                while time.monotonic()<end:
                    records=re.findall(r'MOUSECHECK STATE (\d+) (\d+) (\d+) (\d+)',output())
                    if len(records)>before: return tuple(map(int,records[-1]))
                    time.sleep(.05)
                raise AssertionError('missing state\n'+output()[-1000:])
            wait('New password:'); send('port123\n'); wait('Confirm password:'); send('port123\n')
            wait('Login [root]:'); send('\n'); wait('Password:'); send('port123\n')
            wait('root:/')
            if not args.latency_only:
                send('mousecheck\n'); wait('MOUSECHECK READY')
                if backend!='ps2':
                    assert 'Input: VirtIO absolute pointer' in output(),output()[:2000]
                    mice=qmp('query-mice')
                    assert any(m['current'] and m['absolute'] for m in mice),mice
                    move(8000,16000); left=state(); move(24000,16000); right=state()
                    assert right[0]>left[0] and right[1]==left[1],(left,right)
                    move(8000,16000); assert state()[:2]==left[:2]
                    move(8000,24000); down=state(); assert down[1]>left[1],(left,down)
                    move(8000,8000); assert state()[1]<down[1]
                    move(0,0); assert state()[:2]==(0,0)
                    move(32767,32767); assert state()[:2]==(799,599)
                    key('r'); wait('MOUSECHECK RESIZED'); time.sleep(.3)
                    move(0,0); assert state()[:2]==(0,0)
                    move(32767,32767); assert state()[:2]==(1279,719)
                    for i in range(100): move((i*719)%32768,(i*317)%32768)
                    final=state(); x=(99*719)%32768; y=(99*317)%32768
                    assert final[:2]==(x*1279//32767,y*719//32767),final
                else:
                    initial=state(); move(40,0); right=state(); assert right[0]>initial[0],(initial,right)
                    move(-40,0); left=state(); assert left[0]<right[0],(left,right)
                    move(0,30); down=state(); assert down[1]>left[1],(left,down)
                    move(0,-30); assert state()[1]<down[1]
                button('left',True); assert state()[2]==1
                button('left',False); button('right',True); assert state()[2]==2
                button('right',False); assert state()[2]==0
                start=state()[3]; button('wheel-up',True); button('wheel-up',False); assert state()[3]==(start+1)&0xffffffff
                button('wheel-down',True); button('wheel-down',False); assert state()[3]==start
                key('q'); wait('MOUSECHECK PASS')
                print('Pointer '+backend+': PASS (axes, buttons, wheel, bounds'+(', resize, queue reuse' if backend!='ps2' else '')+')',flush=True)
            time.sleep(.2); key('ctrl-u'); send('inputlatency'+(' wake' if not args.boot else '')+'\n'); wait('LATENCY READY')
            if not args.boot and backend != 'ps2': assert '(interrupts)' in output(),output()[:2000]
            if not args.boot and backend in ('gpu','virtio'): assert 'LATENCY HARDWARE CURSOR' in output(),output()[-1500:]
            time.sleep(.3)  # settle host window-resize events before measuring
            samples=[]; submitted_samples=[]
            for i in range(24):
                if backend=='ps2': continue
                x=6000+i*700; y=9000+i*300
                expected='LATENCY SAMPLE %d %d' % (x*799//32767,y*599//32767)
                started=time.monotonic()
                qmp('input-send-event',{'events':[
                    {'type':'abs','data':{'axis':'x','value':x}},
                    {'type':'abs','data':{'axis':'y','value':y}}]})
                submitted=time.monotonic(); submitted_samples.append((submitted-started)*1000)
                deadline=started+2
                while expected not in output():
                    assert time.monotonic()<deadline,'owner not woken: '+expected+'\n'+output()[-1500:]
                    time.sleep(.001)
                samples.append((time.monotonic()-started)*1000)
                time.sleep(.013)
            key('q'); wait('LATENCY PASS')
            if samples:
                samples.sort()
                print('Input latency '+backend+': median %.2f ms, p95 %.2f ms (host injection to owner)' % (
                    samples[len(samples)//2],samples[int(len(samples)*.95)]),flush=True)
                submitted_samples.sort()
                print('QMP injection overhead: median %.2f ms' % submitted_samples[len(submitted_samples)//2],flush=True)
            if not args.boot and backend in ('gpu','virtio'):
                trace=(b/(backend+'.cursor.trace')).read_text()
                assert 'move' in trace and 'update' in trace,trace[-1500:]

        finally:
            p.terminate()
            try: p.wait(timeout=5)
            except subprocess.TimeoutExpired: p.kill(); p.wait()
            error.close()
