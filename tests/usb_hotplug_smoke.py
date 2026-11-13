"""USB-only input: empty boot, reconnects, hubs, subtree removal and DMA reuse."""
from pathlib import Path
import subprocess
import tempfile
import time
from hardware_inventory_smoke import VM, live_image, login
import hardware_smoke as hardware

B=Path('build')

def run(*args):subprocess.run([str(x) for x in args],check=True,stdout=subprocess.DEVNULL)

with tempfile.TemporaryDirectory(prefix='myos-usb-hotplug-') as temporary:
    temporary=Path(temporary);root=temporary/'root.img';image=temporary/'live.img'
    with root.open('wb') as stream:stream.truncate(64*1048576)
    def fs(*args):run(B/'myfs',root,*args)
    for directory in ['/bin','/lib','/etc','/root','/home','/tmp']:fs('mkdir',directory)
    for name in ['sh','lsusb','lsports']:fs('put',B/'user'/name,'/bin/'+name)
    fs('put',B/'usb-hotplug-check','/bin/usbcheck')
    fs('put',B/'ld.so','/lib/ld.so');fs('put',B/'user/libc.so','/lib/libc.so')
    live_image(root,image,temporary)
    storage=temporary/'storage.img'
    with storage.open('wb') as stream:stream.truncate(8*1048576)
    vm=VM('usb-hotplug',image,[
        '-machine','pc,i8042=off','-device','qemu-xhci,id=xhci',
        '-drive',f'if=none,id=usbdata,format=raw,file={storage},snapshot=on'],4)
    def text():return hardware.clean(vm.log)
    def add(driver,identifier,port,**extra):
        start=len(text());vm.qmp('device_add',dict(driver=driver,id=identifier,bus='xhci.0',port=port,**extra))
        return start
    def remove(identifier):
        start=len(text());vm.qmp('device_del',{'id':identifier});return start
    def topology(devices,hids,hubs):
        vm.send(f'usbcheck {devices} {hids} {hubs}',f'USB TOPOLOGY PASS devices={devices} HID={hids} hubs={hubs}')
    def motion():
        before=vm.mouse_state()
        vm.qmp('input-send-event',{'events':[
            {'type':'rel','data':{'axis':'x','value':12}},
            {'type':'rel','data':{'axis':'y','value':7}},
            {'type':'btn','data':{'button':'wheel-up','down':True}}]})
        vm.qmp('input-send-event',{'events':[{'type':'btn','data':{'button':'wheel-up','down':False}}]})
        end=time.monotonic()+5
        while time.monotonic()<end:
            after=vm.mouse_state()
            if after[:2]!=before[:2] and ((after[4]-before[4])&0xffffffff)==1:return
            time.sleep(.05)
        raise AssertionError(('motion/wheel',before,vm.mouse_state()))
    def mouse(buttons):
        end=time.monotonic()+5
        while time.monotonic()<end:
            if vm.mouse_state()[2]==buttons:return
            time.sleep(.05)
        raise AssertionError(('mouse buttons',vm.mouse_state(),text()[-5000:]))
    try:
        vm.wait('New password:')
        start=add('usb-kbd','keyboard','1',usb_version=2)
        vm.wait('USB HID keyboard:',start);login(vm);topology(1,1,0)
        start=add('usb-mouse','mouse','2');vm.wait('USB HID mouse:',start);topology(2,2,0);motion()
        vm.qmp('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':True}}]});mouse(1)
        start=remove('mouse');vm.wait('USB disconnected:',start);mouse(0);topology(1,1,0)
        vm.send('usbcheck save','USB MEMORY BASELINE')
        # Exceed the device table capacity over time, while only two are live.
        for cycle in range(40):
            start=add('usb-mouse','cyclemouse','2');vm.wait('USB HID mouse:',start)
            start=remove('cyclemouse');vm.wait('USB disconnected:',start)
        vm.send('usbcheck mem','USB MEMORY PASS');topology(1,1,0)
        print('Empty-controller boot, hot-added USB keyboard, held-button removal and 40 reconnects without DMA-page growth: PASS',flush=True)
        start=add('usb-hub','hub','3',ports=4,**{'port-power':True});vm.wait('USB hub:',start)
        start=add('usb-kbd','hubkbd','3.1');vm.wait('USB HID keyboard:',start)
        start=add('usb-mouse','hubmouse','3.2');vm.wait('USB HID mouse:',start);topology(4,3,1)
        start=add('usb-hub','nested','3.3',ports=2);vm.wait('USB hub:',start)
        start=add('usb-mouse','nestedmouse','3.3.1');vm.wait('USB HID mouse:',start);topology(6,4,2);motion()
        vm.send('lsusb -t','[hub: 4 ports]')
        start=remove('hub');vm.wait('USB disconnected:',start);time.sleep(.5);topology(1,1,0)
        print('USB2 hub children, nested hub routes and recursive subtree disconnect: PASS',flush=True)
        # The original keyboard must not leave Shift pressed when unplugged.
        vm.qmp('input-send-event',{'events':[{'type':'key','data':{'down':True,'key':{'type':'qcode','data':'shift'}}}]})
        time.sleep(.2);start=remove('keyboard');vm.wait('USB disconnected:',start)
        start=add('usb-kbd','replacement','1',usb_version=1);vm.wait('USB HID keyboard:',start)
        topology(1,1,0)
        start=add('usb-storage','flash','4',drive='usbdata');vm.wait('USB attached:',start);topology(2,1,0)
        vm.send('lsusb','Mass storage')
        start=remove('flash');vm.wait('USB disconnected:',start);topology(1,1,0)
        assert 'quarantined' not in text() and 'timed out' not in text() and 'PANIC' not in text(),text()[-8000:]
        print('Keyboard modifier release, replacement keyboard and descriptor-only flash hotplug: PASS',flush=True)
    finally:vm.stop()
