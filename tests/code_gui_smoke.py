"""Exercise the real Code app on a disposable MyFS image under headless QEMU."""
import os, pathlib, re, subprocess, time, zlib, struct
ROOT=pathlib.Path(os.environ.get('MYOS_ROOT',pathlib.Path(__file__).resolve().parents[1]))
OUT=ROOT/'build'/'code-gui-smoke'
OUT.mkdir(parents=True,exist_ok=True)
image=OUT/'gui-smoke.img'
with image.open('wb') as f: f.truncate(16*1024*1024)
with (OUT/'install.log').open('w') as log:
    subprocess.run(['make','install','DISK='+str(image)],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True)
def put(source,target):
    subprocess.run([str(ROOT/'build/myfs'),str(image),'put',str(source),target],check=True,stdout=subprocess.DEVNULL)
for directory in ['/project','/project/src']:
    subprocess.run([str(ROOT/'build/myfs'),str(image),'mkdir',directory],check=True,stdout=subprocess.DEVNULL)
demo=OUT/'demo.c'
demo.write_text('#include "win.h"\n\nint main(void) {\n    // Internal MyOS C libraries\n    window *app = win_create("Hello MyOS", 420, 260);\n    if (!app) return 1;\n    gfx_fill_rect(0, 0, 420, 260, RGB(30, 30, 30));\n    gfx_text(24, 32, "Hello from C!", C_WHITE, TRANSPARENT);\n    win_update(app);\n    struct event e;\n    while (win_event(&e, -1)) {\n        if (e.type == EV_CLOSE) break;\n    }\n    win_destroy(app);\n    return 0;\n}\n')
put(demo,'/project/demo.c');put(demo,'/project/src/nested.c')
script=OUT/'start.sh';script.write_text('mode 1280x800\ndesktop &\nsleep 2\ncode /project/demo.c &\nsleep 1000\n');put(script,'/code-smoke.sh')
log=OUT/'gui-serial.log';log.write_text('')
p=subprocess.Popen(['qemu-system-i386','-m','256M','-display','none','-vga','vmware','-no-reboot','-drive',f'format=raw,file={ROOT}/build/os.img,index=0','-drive',f'format=raw,file={image},index=1','-serial',f'file:{log}','-monitor','stdio'],stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=(OUT/'qemu-errors.log').open('w'),text=True)
def text(): return re.sub(r'\x1b\[[0-9;?]*[A-Za-z]','',log.read_text(errors='replace')).replace('\r','')
def wait(s):
    end=time.monotonic()+30
    while time.monotonic()<end:
        if s in text():return
        if p.poll() is not None:raise AssertionError((OUT/'qemu-errors.log').read_text())
        time.sleep(.1)
    raise AssertionError(s+'\n'+text()[-2000:])
def command(s):p.stdin.write(s+'\n');p.stdin.flush()
def key(k):command('sendkey '+k+' 10');time.sleep(.09)
def send(s):
    for ch in s:
        key({'\n':'ret','/':'slash','-':'minus','.':'dot',' ':'spc','_':'shift-minus',';':'semicolon','(':'shift-9',')':'shift-0',',':'comma'}.get(ch,('shift-'+ch.lower()) if ch.isupper() else ch))
def click(x,y):
    for _ in range(14):command('mouse_move -100 -100');time.sleep(.02)
    while x or y:
        dx=min(x,100);dy=min(y,100);command(f'mouse_move {dx} {dy}');x-=dx;y-=dy;time.sleep(.04)
    command('mouse_button 1');time.sleep(.1);command('mouse_button 0');time.sleep(.25)
def screenshot(name):
    ppm=OUT/(name+'.ppm');command('screendump '+str(ppm));time.sleep(.4)
    raw=ppm.read_bytes();head=re.match(rb'P6\s+(\d+)\s+(\d+)\s+255\s',raw);assert head
    w,h=map(int,head.groups());rgb=raw[head.end():];assert len(rgb)==w*h*3
    def chunk(t,b):return struct.pack('>I',len(b))+t+b+struct.pack('>I',zlib.crc32(t+b)&0xffffffff)
    scan=b''.join(b'\x00'+rgb[y*w*3:(y+1)*w*3] for y in range(h))
    (OUT/(name+'.png')).write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(scan))+chunk(b'IEND',b''))
try:
    wait('New password:');send('port123\n');wait('Confirm password:');send('port123\n')
    wait('Login [root]:');send('\n');wait('Password:');send('port123\n');wait('root:/')
    send('sh /code-smoke.sh\n');time.sleep(5);screenshot('code-workspace');click(136,88);screenshot('code-menu');key('esc')
    key('ctrl-b');time.sleep(20);screenshot('code-built')
    click(112,168);screenshot('code-expanded')
    click(148,188);key('ctrl-a');send('int nested_marker;');key('ctrl-s');time.sleep(.3)
    key('ctrl-n');send('gfx_fill_r');screenshot('code-completion');key('tab');key('ctrl-s');screenshot('code-path-input');send('/project/completion.c\n');time.sleep(.5)
    screenshot('code-saved')
    key('ctrl-n');send('gfx_fill_rect(10, 20,');screenshot('code-arguments')
    send(' RGB(1, 2,');screenshot('code-nested-arguments');send(' 3');key('shift-0');key('shift-0')
    key('ctrl-s');send('/project/signature.c\n');time.sleep(.5)
    assert 'PANIC' not in text(),text()[-2000:]
finally:
    p.terminate()
    try:p.wait(timeout=3)
    except subprocess.TimeoutExpired:p.kill();p.wait()
saved=subprocess.check_output([str(ROOT/'build/myfs'),str(image),'cat','/project/completion.c'])
assert saved==b'gfx_fill_rect\n',repr(saved)
nested=subprocess.check_output([str(ROOT/'build/myfs'),str(image),'cat','/project/src/nested.c'])
assert nested==b'int nested_marker;\n',repr(nested)
signature=subprocess.check_output([str(ROOT/'build/myfs'),str(image),'cat','/project/signature.c'])
assert signature==b'gfx_fill_rect(10, 20, RGB(1, 2, 3))\n',repr(signature)
binary=subprocess.check_output([str(ROOT/'build/myfs'),str(image),'cat','/project/demo'])
assert binary.startswith(b'\x7fELF'),binary[:200]
print('Code GUI passed: workspace, library completion, save, native C build')
