"""BIOS USB live boot, VBE, real USB HID input, RAM writes, clean reboot."""
import hashlib
import json
import pathlib
import re
import socket
import subprocess
import tempfile
import time
import struct
import zlib

B=pathlib.Path('build')
IMAGE=B/'myos-usb.img'

def digest(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def clean(p): return re.sub(r'\x1b\[[0-9;?]*[A-Za-z]','',p.read_text(errors='replace')).replace('\r','')

def save_png(ppm,png):
    magic,dimensions,maximum,pixels=ppm.read_bytes().split(b'\n',3)
    assert magic==b'P6' and maximum==b'255'
    width,height=map(int,dimensions.split()); assert len(pixels)==width*height*3
    def chunk(kind,data):
        return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data)&0xffffffff)
    rows=b''.join(b'\0'+pixels[y*width*3:(y+1)*width*3] for y in range(height))
    png.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',width,height,8,2,0,0,0))+
                    chunk(b'IDAT',zlib.compress(rows))+chunk(b'IEND',b''))

class VM:
    def __init__(self,name,usb=True,vga='std',memory='256M',machine='pc',usb_version=2,legacy=False):
        self.temp=tempfile.TemporaryDirectory(prefix='myos-usb-')
        # Poll a native filesystem log: DrvFS can return ENODATA while a Windows
        # file is concurrently extended by QEMU.
        self.log=pathlib.Path(self.temp.name)/(name+'.log'); self.log.write_text('')
        self.log_artifact=B/(name+'.log')
        self.sockpath=self.temp.name+'/qmp'
        args=['qemu-system-i386','-m',memory,'-display','none','-vga',vga,
              '-serial','file:'+str(self.log),'-qmp','unix:'+self.sockpath+',server=on,wait=off',
              '-no-reboot','-nic','none']
        if usb:
            # No PS/2 controller: all injected keys must traverse the xHCI driver.
            args+=['-machine',machine+',i8042=off','-device','qemu-xhci,id=xhci',
                   '-drive',f'if=none,id=stick,format=raw,file={IMAGE},snapshot=on',
                   '-device','usb-storage,bus=xhci.0,drive=stick,bootindex=1',
                   '-device',f'usb-kbd,bus=xhci.0,usb_version={usb_version}','-device','usb-mouse,bus=xhci.0']
        elif legacy:
            args+=['-drive','format=raw,file=build/os.img,snapshot=on,index=0',
                   '-drive','format=raw,file=build/hardware/live-root.img,snapshot=on,index=1']
        else:
            args+=['-drive',f'format=raw,file={IMAGE},snapshot=on']
        self.err=(B/(name+'.stderr')).open('w')
        self.p=subprocess.Popen(args,stdout=subprocess.DEVNULL,stderr=self.err)
        self.s=socket.socket(socket.AF_UNIX); self.s.settimeout(5)
        deadline=time.monotonic()+10
        while not pathlib.Path(self.sockpath).exists():
            if self.p.poll() is not None: raise AssertionError((B/(name+'.stderr')).read_text())
            if time.monotonic()>deadline: raise AssertionError('QMP socket missing')
            time.sleep(.05)
        self.s.connect(self.sockpath); self.f=self.s.makefile('rb')
        self.read_response(greeting=True); self.qmp('qmp_capabilities')
    def read_response(self,greeting=False):
        while True:
            r=json.loads(self.f.readline())
            if greeting and 'QMP' in r: return r
            if 'error' in r: raise AssertionError(r)
            if 'return' in r: return r['return']
    def qmp(self,command,args=None):
        self.s.sendall((json.dumps({'execute':command,'arguments':args or {}})+'\n').encode())
        return self.read_response()
    def wait(self,marker,after=0,timeout=40):
        end=time.monotonic()+timeout
        while time.monotonic()<end:
            text=clean(self.log)
            if marker in text[after:]: return len(text)
            if self.p.poll() is not None: raise AssertionError('QEMU exited: '+text[-2500:])
            time.sleep(.1)
        raise AssertionError(marker+' missing: '+clean(self.log)[-3500:])
    def key(self,qcode):
        self.qmp('send-key',{'keys':[{'type':'qcode','data':qcode}],'hold-time':35})
        time.sleep(.08)
    def send(self,text,marker):
        start=len(clean(self.log))
        for char in text:
            self.key({' ':'spc','/':'slash','.':'dot','-':'minus',':':'semicolon','>':'dot'}.get(char,char))
        self.key('ret')
        return self.wait(marker,start)
    def stop(self):
        try: self.qmp('quit')
        except (OSError,ValueError): pass
        try: self.p.wait(timeout=5)
        except subprocess.TimeoutExpired: self.p.kill(); self.p.wait()
        self.s.close(); self.err.close()
        self.log_artifact.write_bytes(self.log.read_bytes())
        self.temp.cleanup()
    def mouse_state(self):
        symbols=subprocess.check_output(['nm','-n','build/hardware/kernel.elf'],text=True)
        address=next(int(line.split()[0],16) for line in symbols.splitlines() if line.endswith(' b st'))
        result=self.qmp('human-monitor-command',{'command-line':f'xp /4uw 0x{address:x}'})
        return [int(n) for n in result.split(':',1)[1].split()]

def main():
    before=digest(IMAGE); original=digest(pathlib.Path('disk.img'))
    vm=VM('hardware-usb')
    try:
        vm.wait('BIOS VBE framebuffer')
        vm.wait('USB HID keyboard:')
        vm.wait('USB HID mouse:')
        vm.wait('LIVE MODE:')
        vm.wait('New password:')
        vm.send('usbtest123','Confirm password:')
        vm.send('usbtest123','Login [root]:')
        vm.send('','Password:')
        vm.send('usbtest123','root:/')
        vm.send('whoami','\nroot\n')
        vm.send('mkdir /ramcheck','root:/')
        vm.send('ls /','ramcheck')
        # Exercise extended keys and repeat, causing both transfer and event rings to wrap.
        for _ in range(140): vm.key('a'); vm.key('backspace')
        vm.send('whoami','\nroot\n')
        vm.send('desktop','')
        time.sleep(3)
        old_mouse=vm.mouse_state()
        vm.qmp('input-send-event',{'events':[{'type':'rel','data':{'axis':'x','value':40}},
                                               {'type':'rel','data':{'axis':'y','value':20}}]})
        time.sleep(.3)
        new_mouse=vm.mouse_state()
        assert new_mouse[0]>old_mouse[0] and new_mouse[1]>old_mouse[1] and new_mouse[3]>old_mouse[3], (old_mouse,new_mouse)
        vm.qmp('screendump',{'filename':str((B/'hardware-desktop.ppm').resolve())})
        save_png(B/'hardware-desktop.ppm',B/'hardware-desktop.png')
        assert 'EXCEPTION' not in clean(vm.log) and 'timed out' not in clean(vm.log)
    finally: vm.stop()
    vm=VM('hardware-fullspeed',machine='q35',usb_version=1)
    try:
        vm.wait('USB HID keyboard:')
        assert re.search(r'USB device: port \d+, speed 1,',clean(vm.log))
        vm.wait('New password:')
        vm.send('fullspeed123','Confirm password:')
    finally: vm.stop()
    vm=VM('hardware-original-boot',usb=False,legacy=True,vga='vmware')
    try:
        vm.wait('New password:')
        assert 'Graphics: VMware SVGA II' in clean(vm.log)
        assert 'USB live RAM disk' not in clean(vm.log)
        vm.send('legacytest123','Confirm password:')
    finally: vm.stop()
    assert digest(IMAGE)==before and digest(pathlib.Path('disk.img'))==original
    # New RAM root on every boot: account setup returns, previous RAM edits vanish.
    vm=VM('hardware-reboot',usb=False,memory='16G')
    try:
        vm.wait('New password:')
        assert 'USB live RAM disk (volatile)' in clean(vm.log)
        assert 'BIOS VBE framebuffer' in clean(vm.log)
    finally: vm.stop()
    vm=VM('hardware-text-fallback',usb=False,vga='cirrus')
    try:
        vm.wait('New password:')
        assert 'staying in VGA text mode' in clean(vm.log)
    finally: vm.stop()
    assert digest(IMAGE)==before and digest(pathlib.Path('disk.img'))==original
    print('USB boot + VBE + USB HID keyboard/mouse + ring wrap + RAM filesystem + 16 GiB reboot + text fallback + full-speed/Q35 + original boot: OK')

if __name__=='__main__': main()
