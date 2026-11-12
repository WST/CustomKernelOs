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
    def __init__(self,name,usb=True,vga='std',memory=None,machine='pc',usb_version=2,legacy=False,cpu='max',cpus=1,extra=None,netboot_url=None):
        self.temp=tempfile.TemporaryDirectory(prefix='myos-usb-')
        # Poll a native filesystem log: DrvFS can return ENODATA while a Windows
        # file is concurrently extended by QEMU.
        self.log=pathlib.Path(self.temp.name)/(name+'.log'); self.log.write_text('')
        self.log_artifact=B/(name+'.log')
        self.sockpath=self.temp.name+'/qmp'
        root_mb=json.loads((B/'hardware/manifest.json').read_text())['root_bytes']//1048576
        self.boot_timeout=max(40,root_mb//2)
        if memory is None:
            memory=str(1 << (max(256,root_mb+128)-1).bit_length())+'M'
        args=['qemu-system-i386','-m',memory,'-cpu',cpu,'-smp',str(cpus),'-display','none','-vga',vga,
              '-serial','file:'+str(self.log),'-qmp','unix:'+self.sockpath+',server=on,wait=off',
              '-no-reboot','-nic','none']
        if netboot_url:
            # No attached disk: successful boot must come from the HTTP server.
            args+=['-netdev','user,id=netboot,bootfile='+netboot_url,
                   '-device','e1000,netdev=netboot,bootindex=1','-boot','order=n']
        elif usb:
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
        args+=extra or []
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
    def wait(self,marker,after=0,timeout=None):
        end=time.monotonic()+(self.boot_timeout if timeout is None else timeout)
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
            if char in ('>','&'):
                self.qmp('send-key',{'keys':[{'type':'qcode','data':'shift'},
                                           {'type':'qcode','data':'dot' if char=='>' else '7'}],'hold-time':35})
                time.sleep(.08)
            else:
                self.key({' ':'spc','/':'slash','.':'dot','-':'minus',':':'semicolon',"'":'apostrophe'}.get(char,char))
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
        result=self.qmp('human-monitor-command',{'command-line':f'xp /5uw 0x{address:x}'})
        return [int(n) for line in result.splitlines() if ':' in line for n in line.split(':',1)[1].split()]

    def check_console_pixels(self,prefix="hardware-scroll"):
        symbols=subprocess.check_output(['nm','-n','build/hardware/kernel.elf'],text=True)
        addresses={line.split()[2]:int(line.split()[0],16) for line in symbols.splitlines() if len(line.split())==3}
        self.qmp('stop')
        try:
            ppm=B/(prefix+'.ppm')
            self.qmp('screendump',{'filename':str(ppm.resolve())})
            magic,dimensions,maximum,pixels=ppm.read_bytes().split(b'\n',3)
            width,height=map(int,dimensions.split()); columns=width//8; rows=height//16
            assert magic==b'P6' and maximum==b'255'
            def memory(name,size):
                path=pathlib.Path(self.temp.name)/(name+'.bin')
                self.qmp('pmemsave',{'val':addresses[name],'size':size,'filename':str(path)})
                return path.read_bytes()
            cells=struct.unpack('<'+'H'*(columns*rows),memory('shadow',columns*rows*2))
            font=memory('font',4096)
            palette=(0x000000,0x0000aa,0x00aa00,0x00aaaa,0xaa0000,0xaa00aa,0xaa5500,0xaaaaaa,
                     0x555555,0x5555ff,0x55ff55,0x55ffff,0xff5555,0xff55ff,0xffff55,0xffffff)
            colors=[bytes((v>>16,(v>>8)&255,v&255)) for v in palette]
            # Skip the three bottom scanlines where the underline cursor lives.
            for row in range(rows):
                for col in range(columns):
                    cell=cells[row*columns+col]; char=cell&255; attr=cell>>8
                    fg=colors[attr&15]; bg=colors[attr>>4]
                    for y in range(13):
                        bits=font[char*16+y]
                        expected=b''.join(fg if bits&(0x80>>x) else bg for x in range(8))
                        offset=((row*16+y)*width+col*8)*3
                        assert pixels[offset:offset+24]==expected, ('console pixels differ',row,col,y,hex(cell))
            save_png(ppm,B/(prefix+'.png'))
        finally: self.qmp('cont')

def main():
    before=digest(IMAGE); original=digest(pathlib.Path('disk.img'))
    vm=VM('hardware-usb',cpus=2)
    try:
        vm.wait('BIOS VBE framebuffer')
        vm.wait('USB HID keyboard:')
        vm.wait('USB HID mouse:')
        vm.wait('LIVE MODE:')
        vm.wait('New password:')
        # Early boot text must survive the loader's graphics-mode handoff.
        boot_console=clean(vm.log)
        assert 'Framebuffer cache: PAT write-combining' in boot_console
        assert 'SMP initialization complete: 2 CPU(s)' in boot_console
        assert 'GDT+TSS' in boot_console and 'Physical memory:' in boot_console
        stages=('physical memory allocator','paging and kernel mappings',
                'kernel heap','framebuffer and graphical console',
                'xHCI and USB HID input','storage probe','MyFS mount',
                'SMP processor startup','services','kernel ready; entering login/shell')
        previous=-1
        for stage in stages:
            position=boot_console.index('[BOOT] '+stage)
            assert position>previous, 'boot stages out of order: '+stage
            previous=position
        assert 'VBE boot: base=' in boot_console and 'Framebuffer: base=' in boot_console
        console_ppm=B/'hardware-console.ppm'
        vm.qmp('screendump',{'filename':str(console_ppm.resolve())})
        magic,dimensions,maximum,pixels=console_ppm.read_bytes().split(b'\n',3)
        width,height=map(int,dimensions.split())
        assert magic==b'P6' and maximum==b'255' and len(pixels)==width*height*3
        # The password prompt's unused right margin must stay black.
        assert width>=800 and height>=480
        for y in range(height-16,height):
            assert not any(pixels[(y*width+80*8)*3:((y+1)*width)*3]), 'corrupt boot console background'
        save_png(console_ppm,B/'hardware-console.png')
        vm.send('usbtest123','Confirm password:')
        vm.send('usbtest123','Login [root]:')
        vm.send('','Password:')
        vm.send('usbtest123','root:/')
        vm.send('whoami','\nroot\n')
        vm.send('mkdir /ramcheck','root:/')
        vm.send('ls /','ramcheck')
        # More than one screen of colored output, including blank margins:
        # every scrolled glyph must still match the logical RAM text cells.
        for _ in range(12): vm.send('help','Same name: /bin wins.')
        vm.send('whoami','\nroot\n')
        time.sleep(.2)  # let the final prompt/cursor finish before freezing CPUs
        vm.check_console_pixels()
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
    vm=VM('hardware-no-pat',usb=False,cpu='max,-pat')
    try:
        vm.wait('New password:')
        assert 'Framebuffer cache: uncached (PAT unavailable)' in clean(vm.log)
        assert 'BIOS VBE framebuffer' in clean(vm.log)
    finally: vm.stop()
    assert digest(IMAGE)==before and digest(pathlib.Path('disk.img'))==original
    print('USB boot + VBE + USB HID keyboard/mouse + ring wrap + RAM filesystem + 16 GiB reboot + text fallback + full-speed/Q35 + original boot: OK')

if __name__=='__main__': main()
