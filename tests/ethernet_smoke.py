"""Real Intel descriptor rings via QEMU e1000/e1000e on disposable RAM-root boots."""
import argparse
import re
import socket
import time
from pathlib import Path
from urllib.request import urlopen
from hardware_smoke import VM,clean,digest,IMAGE

parser=argparse.ArgumentParser()
parser.add_argument('--model',choices=('e1000','e1000e','all'),default='all')
args=parser.parse_args()
before=digest(IMAGE);original=digest(Path('disk.img'))
for model in (('e1000','e1000e') if args.model=='all' else (args.model,)):
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1',0));port=reservation.getsockname()[1]
    vm=VM('ethernet-'+model,usb=False,extra=['-S','-netdev',
        f'user,id=wire,hostfwd=tcp:127.0.0.1:{port}-:80','-device',model+',netdev=wire,id=eth0'])
    try:
        vm.qmp('set_link',{'name':'eth0','up':False});vm.qmp('cont')
        vm.wait('New password:')
        boot=clean(vm.log)
        assert 'NET: RX/TX ready' in boot and 'NET: link down;' in boot,boot[-2500:]
        assert 'NET: DHCP configured' not in boot
        assert ('82574L' if model=='e1000e' else '82540EM') in boot
        vm.send('ethernet123','Confirm password:');vm.send('ethernet123','Login [root]:')
        vm.send('','Password:');vm.send('ethernet123','root:/')
        vm.send('ifconfig','no address yet')
        vm.qmp('set_link',{'name':'eth0','up':True})
        vm.wait('NET: DHCP configured 10.0.2.15')
        vm.send('ifconfig','inet ')
        start=len(clean(vm.log));vm.send('ping -c 3 10.0.2.2','ping statistics')
        vm.wait('loss',start)
        ping=clean(vm.log)[start:]
        assert re.search(r'[1-3] received',ping),ping[-1500:]
        # Reconnect must reacquire DHCP, rather than retaining QEMU-only defaults.
        start=len(clean(vm.log));vm.qmp('set_link',{'name':'eth0','up':False})
        vm.wait('NET: link down;',start)
        vm.qmp('set_link',{'name':'eth0','up':True})
        vm.wait('NET: DHCP configured 10.0.2.15',start)
        start=len(clean(vm.log))
        vm.send('dmesg','NET: DHCP configured')
        vm.wait('root:/',start)
        log=clean(vm.log)[start:]
        assert 'NET: RX/TX ready' in log and 'Kernel log: 16 KiB memory ring' in log,log[-2000:]
        assert 'ethernet123' not in log,log[-2000:]
        vm.send('ls /var/log','root:/')
        # Explicit redirection is the only way the kernel journal becomes a file.
        vm.send('dmesg > /tmp/boot.log','root:/')
        vm.send('cat /tmp/boot.log','NET: RX/TX ready')
        vm.send('dmesg -n 1','root:/')
        start=len(clean(vm.log));vm.send('dmesg -n 0 -w','dmesg -n 0 -w')
        vm.qmp('set_link',{'name':'eth0','up':False})
        vm.wait('NET: link down;',start)
        vm.qmp('set_link',{'name':'eth0','up':True})
        vm.wait('NET: DHCP configured 10.0.2.15',start)
        # The same text first appears on the serial sink, then through dmesg.
        deadline=time.monotonic()+10
        while clean(vm.log)[start:].count('NET: DHCP configured 10.0.2.15')<2:
            assert time.monotonic()<deadline,clean(vm.log)[start:][-2500:]
            time.sleep(.1)
        vm.qmp('send-key',{'keys':[{'type':'qcode','data':'ctrl'},{'type':'qcode','data':'c'}],'hold-time':50})
        vm.wait('root:/',start)
        vm.send('httpd','httpd: listening on port 80')
        expected=Path('user/lib/myos.h').read_bytes()
        for _ in range(40):
            with urlopen(f'http://127.0.0.1:{port}/files/include/myos.h',timeout=10) as response:
                assert response.status==200 and response.read()==expected
        text=clean(vm.log)
        assert 'PANIC' not in text and 'EXCEPTION' not in text,text[-1500:]
        print(f'Intel {model}: link down/up, DHCP, ARP/ICMP, reconnect, dmesg follow/Ctrl+C, 40 HTTP transfers and ring reuse: PASS',flush=True)
    finally:vm.stop()
assert digest(IMAGE)==before and digest(Path('disk.img'))==original
