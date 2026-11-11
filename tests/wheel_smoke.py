"""Mouse-wheel smoke test (PS/2).

Boots the standard image headless in QEMU (snapshot=on, so the real disk.img is
never touched), injects wheel events through QMP — button wheel-up / wheel-down,
the exact path QEMU maps onto the PS/2 wheel — and asserts the kernel's
cumulative st.wheel moved the right way (+ up, - down).

This validates the kernel PS/2 path directly, without needing a login or a GUI:
  * the IMPS/2 handshake (F3 200 -> F3 100 -> F3 80) put the mouse in 4-byte
    mode, so the wheel byte is delivered at all (QEMU silently drops dz/dw
    unless mouse_type is set);
  * the ISR decodes the IMPS/2 nibble into our "+ up / - down" convention.

st.wheel lives in the kernel (struct mouse_state st), written by the PS/2 ISR,
so it is independent of whether the desktop is running.
"""
import json
import pathlib
import re
import socket
import subprocess
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
B = ROOT / 'build'
IMG = B / 'os.img'


def clean(p):
    return re.sub(r'\x1b\[[0-9;?]*[A-Za-z]', '', p.read_text(errors='replace')).replace('\r', '')


def st_address():
    """Виртуальный адрес struct mouse_state st из ядра (BSS, символ b st)."""
    syms = subprocess.check_output(['nm', 'build/kernel.elf'], cwd=ROOT, text=True)
    return next(int(line.split()[0], 16) for line in syms.splitlines() if line.endswith(' b st'))


class VM:
    def __init__(self):
        self.temp = tempfile.TemporaryDirectory(prefix='myos-wheel-')
        self.log = pathlib.Path(self.temp.name) / 'serial.log'
        self.log.write_text('')
        self.sockpath = self.temp.name + '/qmp'
        # snapshot=on: чтение с настоящих дисков, записи — в временный оверлей.
        args = ['qemu-system-i386', '-smp', '2', '-m', '256M', '-display', 'none', '-no-reboot',
                '-drive', 'format=raw,file=' + str(IMG) + ',index=0,snapshot=on',
                '-drive', 'format=raw,file=' + str(ROOT / 'disk.img') + ',index=1,snapshot=on',
                '-nic', 'none',
                '-serial', 'file:' + str(self.log),
                '-qmp', 'unix:' + self.sockpath + ',server=on,wait=off']
        self.err = (B / 'wheel-smoke.stderr').open('w')
        self.p = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=self.err, cwd=ROOT)
        self.s = socket.socket(socket.AF_UNIX)
        self.s.settimeout(5)
        deadline = time.monotonic() + 10
        while not pathlib.Path(self.sockpath).exists():
            if self.p.poll() is not None:
                raise AssertionError((B / 'wheel-smoke.stderr').read_text())
            if time.monotonic() > deadline:
                raise AssertionError('QMP socket missing')
            time.sleep(.05)
        self.s.connect(self.sockpath)
        self.f = self.s.makefile('rb')
        self.read_response(greeting=True)
        self.qmp('qmp_capabilities')

    def read_response(self, greeting=False):
        while True:
            r = json.loads(self.f.readline())
            if greeting and 'QMP' in r:
                return r
            if 'error' in r:
                raise AssertionError(r)
            if 'return' in r:
                return r['return']

    def qmp(self, command, args=None):
        self.s.sendall((json.dumps({'execute': command, 'arguments': args or {}}) + '\n').encode())
        return self.read_response()

    def wait(self, marker, timeout=45):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            text = clean(self.log)
            if 'PANIC' in text or 'Segmentation fault' in text:
                raise AssertionError(text[-3000:])
            if marker in text:
                return len(text)
            if self.p.poll() is not None:
                raise AssertionError('QEMU exited: ' + text[-2500:])
            time.sleep(.1)
        raise AssertionError(marker + ' missing: ' + clean(self.log)[-3500:])

    def read_wheel(self):
        """st: x, y, buttons, events, wheel -> 5 dwords, 5-е = wheel.
        HMP выводит значения в десятичном виде и может перенести на несколько
        строк: на каждой строке после 'адрес:' идут числа. Собираем их все."""
        base = st_address()
        out = self.qmp('human-monitor-command', {'command-line': f'xp /5dw 0x{base:x}'})
        vals = []
        for line in out.splitlines():
            if ':' not in line:
                continue
            vals.extend(int(tok) for tok in line.split(':', 1)[1].split())
        return vals[-1]

    def wheel(self, button):
        self.qmp('input-send-event', {'events': [{'type': 'btn', 'data': {'button': button, 'down': True}}]})
        time.sleep(.25)

    def stop(self):
        try:
            self.qmp('quit')
        except (OSError, ValueError):
            pass
        try:
            self.p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait()
        self.s.close()
        self.err.close()
        (B / 'wheel-smoke.log').write_bytes(self.log.read_bytes())
        self.temp.cleanup()


def main():
    addr = st_address()
    vm = VM()
    try:
        # Мышь инициализируется ядром ещё до этой строки (kernel.c), значит
        # к моменту баннера IMPS/2-рукопожатие уже выполнено и мышь готова.
        vm.wait('Welcome to MyOS!')
        time.sleep(1.0)
        w0 = vm.read_wheel()
        for _ in range(3):
            vm.wheel('wheel-up')
        w1 = vm.read_wheel()
        for _ in range(3):
            vm.wheel('wheel-down')
        w2 = vm.read_wheel()
        print(f'st address=0x{addr:x}  wheel before={w0} after_up={w1} after_down={w2}')
        assert w1 > w0, (w0, w1)
        assert w2 < w1, (w1, w2)
    finally:
        vm.stop()
    print('WHEEL PASS: IMPS/2 handshake + 4th-byte decode (up +, down -)')


if __name__ == '__main__':
    main()
