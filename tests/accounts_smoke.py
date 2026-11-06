"""QEMU account lifecycle using a disposable copy of disk.img."""
import pathlib, shutil, subprocess, time, re
b=pathlib.Path("build")
image=b/"accounts-test.img"
shutil.copyfile("disk.img",image)
subprocess.run(["build/myfs",str(image),"rm","/etc/accounts"],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
def clean(s): return re.sub(r"\x1b\[[0-9;?]*[A-Za-z]","",s).replace("\r","")
def boot(name):
    log=b/(name+".log");log.write_text("")
    p=subprocess.Popen(["qemu-system-i386","-m","256M","-display","none","-drive","format=raw,file=build/os.img,index=0","-drive",f"format=raw,file={image},index=1","-serial",f"file:{log}","-monitor","stdio","-no-reboot"],stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,text=True)
    return p,log
def wait(log,marker,after=0):
    end=time.monotonic()+25
    while time.monotonic()<end:
        s=clean(log.read_text(errors="replace"))
        if marker in s[after:]: return len(s)
        time.sleep(.1)
    raise AssertionError(marker+" missing: "+s[-2000:])
def send(p,log,s,marker):
    start=len(clean(log.read_text(errors="replace")))
    for c in s:
        key={" ":"spc","/":"slash",".":"dot","-":"minus"}.get(c,c)
        p.stdin.write("sendkey "+key+" 10\n");p.stdin.flush();time.sleep(.04)
    p.stdin.write("sendkey ret 10\n");p.stdin.flush()
    return wait(log,marker,start)
p,log=boot("accounts-first")
try:
    wait(log,"New password:")
    send(p,log,"secret123","Confirm password:")
    send(p,log,"secret123","Login [root]:")
    send(p,log,"","Password:")
    send(p,log,"wrong","Login incorrect.")
    wait(log,"Login [root]:",len(clean(log.read_text(errors="replace")))-1)
    send(p,log,"","Password:")
    send(p,log,"secret123","root:/")
    send(p,log,"useradd alice","New password:")
    send(p,log,"alice123","Confirm password:")
    send(p,log,"alice123","root:/")
    send(p,log,"logout","Login [root]:")
    send(p,log,"alice","Password:")
    send(p,log,"alice123","alice:/")
    send(p,log,"whoami","\nalice\n")
    send(p,log,"useradd bob","root required")
    send(p,log,"cat /etc/accounts","not permitted")
    send(p,log,"passwd","New password:")
    send(p,log,"changed123","Confirm password:")
    send(p,log,"changed123","alice:/")
    s=clean(log.read_text(errors="replace"))
    assert "secret123" not in s and "alice123" not in s and "changed123" not in s
finally:
    p.terminate();p.wait(timeout=5)
p,log=boot("accounts-reboot")
try:
    wait(log,"Login [root]:")
    send(p,log,"alice","Password:")
    send(p,log,"changed123","alice:/")
    send(p,log,"whoami","\nalice\n")
    send(p,log,"exit","Login [root]:")
finally:
    p.terminate();p.wait(timeout=5)
print("accounts lifecycle and persistence: OK")
