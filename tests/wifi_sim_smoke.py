"""QEMU: учебная точка MyOS выдаёт DHCP на wlan0, проводной eth0 остаётся."""
import pathlib, subprocess, time, re
b = pathlib.Path("build")
subprocess.run(["make", "build/myfs", "build/os.img", "build/user/ifconfig", "build/user/wifid"], check=True)
image = b / "wifi-sim.img"
with image.open("wb") as f:
    f.truncate(32 * 1024 * 1024)
subprocess.run(["make", "install", "DISK=" + str(image)], check=True, stdout=subprocess.DEVNULL)
subprocess.run(["build/myfs", str(image), "mkdir", "/etc/wifi"], check=True, stdout=subprocess.DEVNULL)
sim = b / "wifi-sim-marker"
sim.write_bytes(b"")
nets = b / "wifi-sim-networks"
nets.write_text("network {\nssid=MyOS\npriority=1\n}\n")
subprocess.run(["build/myfs", str(image), "put", str(sim), "/etc/wifi/sim"], check=True, stdout=subprocess.DEVNULL)
subprocess.run(["build/myfs", str(image), "put", str(nets), "/etc/wifi/networks"], check=True, stdout=subprocess.DEVNULL)
script = b / "wifi-sim.sh"
script.write_text("ifconfig > /wifi-result\nsync\necho WIFI-SIM-DONE\nsleep 1000\n")
subprocess.run(["build/myfs", str(image), "put", str(script), "/wifi-sim.sh"], check=True, stdout=subprocess.DEVNULL)
log = b / "wifi-sim.log"
log.write_text("")
p = subprocess.Popen(
    ["qemu-system-i386", "-m", "256M", "-display", "none", "-vga", "vmware",
     "-nic", "user,model=e1000",
     "-drive", "format=raw,file=build/os.img,index=0",
     "-drive", f"format=raw,file={image},index=1",
     "-serial", f"file:{log}", "-monitor", "stdio"],
    stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
def output():
    return re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", log.read_text(errors="replace")).replace("\r", "")
def wait(s, sec=40):
    end = time.monotonic() + sec
    while time.monotonic() < end:
        if s in output():
            return
        if p.poll() is not None:
            raise AssertionError(p.stderr.read())
        time.sleep(0.1)
    raise AssertionError(s + "\n" + output()[-2500:])
def send(s):
    for ch in s:
        key = {"\n": "ret", " ": "spc", "/": "slash", "-": "minus", ".": "dot"}.get(ch, ch)
        p.stdin.write("sendkey " + key + " 80\n")
        p.stdin.flush()
        time.sleep(0.12)
try:
    wait("New password:")
    send("port123\n")
    wait("Confirm password:")
    send("port123\n")
    wait("Login [root]:")
    send("\n")
    wait("Password:")
    send("port123\n")
    wait("root:/")
    wait("DHCP configured 10.9.9.9 on wlan0", 20)
    send("sh /wifi-sim.sh\n")
    wait("WIFI-SIM-DONE", 30)
    text = output()
    assert "PANIC" not in text, text[-2000:]
    assert "WLAN: simulated open network MyOS" in text
finally:
    p.terminate()
    try:
        p.wait(timeout=3)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()
saved = subprocess.check_output(["build/myfs", str(image), "cat", "/wifi-result"])
assert b"wlan0" in saved and b"10.9.9.9" in saved, saved
print("wifi sim: PASS")
