"""Boot a disposable disk and exercise Settings through real keyboard events."""
import pathlib, subprocess, time, re, struct
b=pathlib.Path('build')
subprocess.run(['make','build/myfs','build/os.img','build/user/settings','build/user/desktop'],check=True)
image=b/'settings-smoke.img'
with image.open('wb') as f: f.truncate(8*1024*1024)
subprocess.run(['make','install','DISK='+str(image)],check=True,stdout=subprocess.DEVNULL)
# A solid green 2x2 BMP makes the compositor output easy to verify.
bmp=b/'settings-wall.bmp'
pixels=(b'\x30\xc0\x20'*2+b'\0\0')*2
bmp.write_bytes(b'BM'+struct.pack('<IHHI',70,0,0,54)+struct.pack('<IiiHHIIiiII',40,2,2,1,24,0,16,0,0,0,0)+pixels)
subprocess.run(['build/myfs',str(image),'put',str(bmp),'/wall.bmp'],check=True,stdout=subprocess.DEVNULL)
script=b/'settings-smoke.sh'
script.write_text('desktop &\nsleep 1\nsettings\nifconfig > /network-result\nsleep 1000\n')
subprocess.run(['build/myfs',str(image),'put',str(script),'/settings-smoke.sh'],check=True,stdout=subprocess.DEVNULL)
log=b/'settings-smoke.log'; log.write_text('')
p=subprocess.Popen(['qemu-system-i386','-m','256M','-display','none','-vga','vmware','-nic','user,model=e1000','-drive','format=raw,file=build/os.img,index=0','-drive',f'format=raw,file={image},index=1','-serial',f'file:{log}','-monitor','stdio'],stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,text=True)
def output(): return re.sub(r'\x1b\[[0-9;?]*[A-Za-z]','',log.read_text(errors='replace')).replace('\r','')
def wait(s):
    end=time.monotonic()+25
    while time.monotonic()<end:
        if s in output(): return
        if p.poll() is not None: raise AssertionError(p.stderr.read())
        time.sleep(.1)
    raise AssertionError(s+'\n'+output()[-2000:])
def monitor(s): p.stdin.write(s+'\n'); p.stdin.flush()
def key(k): monitor('sendkey '+k+' 80'); time.sleep(.18)
def send(s):
    for ch in s: key({'\n':'ret','/':'slash','-':'minus','.':'dot',' ':'spc'}.get(ch,ch))
try:
    wait('New password:'); send('port123\n')
    wait('Confirm password:'); send('port123\n')
    wait('Login [root]:'); send('\n')
    wait('Password:'); send('port123\n')
    wait('root:/'); send('sh /settings-smoke.sh\n'); time.sleep(4)
    monitor('screendump build/settings-default.ppm'); time.sleep(.3)
    key('tab')
    for _ in range(16): key('backspace')
    send('1.1.1.1\n'); time.sleep(.4)
    monitor('screendump build/settings-network.ppm'); time.sleep(.3)
    key('tab'); key('down')
    for i in range(6):
        monitor(f'screendump build/settings-gallery-{i}.ppm'); time.sleep(.3)
        if i<5: key('right'); time.sleep(5)
    key('tab'); key('ctrl-u'); send('/wall.bmp\n'); time.sleep(2)
    monitor('screendump build/settings-wallpaper.ppm'); time.sleep(.3)
    key('esc'); time.sleep(1)
    monitor('screendump build/settings-desktop.ppm'); time.sleep(.5)
    assert 'PANIC' not in output(),output()[-2000:]
finally:
    p.terminate()
    try: p.wait(timeout=3)
    except subprocess.TimeoutExpired: p.kill(); p.wait()
assert subprocess.check_output(['build/myfs',str(image),'cat','/etc/wallpaper'])==b'/wall.bmp'
assert b'dns 1.1.1.1' in subprocess.check_output(['build/myfs',str(image),'cat','/network-result'])
raw=(b/'settings-desktop.ppm').read_bytes().split(maxsplit=4)
assert raw[0]==b'P6' and raw[3]==b'255'
assert raw[4][-3:]==bytes((32,192,48)),raw[4][-3:]
print('Settings GUI: PASS (DNS, saved wallpaper, compositor pixels, clean exit)')

# Each supplied JPEG must decode and change the visible desktop outside the window.
patches=[]
for i in range(6):
    parts=(b/f'settings-gallery-{i}.ppm').read_bytes().split(maxsplit=4)
    width=int(parts[1]); data=parts[4]
    patches.append(data[600*width*3:700*width*3])
assert len(set(patches))==6, 'Some bundled wallpaper did not change the desktop'
print('Bundled wallpapers: PASS (six decoded JPEGs and gallery selection)')
