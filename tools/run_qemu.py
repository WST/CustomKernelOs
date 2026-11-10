#!/usr/bin/env python3
"""Choose a verified hardware GL path, or the portable software display.

The selection happens before opening disks. Never restart an already booted
guest automatically: a second boot could repeat writes to the user's disk.
"""
import argparse
import ctypes as c
import json
import os
import pathlib
import platform
import shlex
import subprocess
import sys


def probe_gl():
    egl = c.CDLL('libEGL.so.1')
    gl = c.CDLL('libGL.so.1')
    pointer, integer = c.c_void_p, c.c_int

    def call(name, result, args):
        f = getattr(egl, name)
        f.restype, f.argtypes = result, args
        return f

    display = call('eglGetDisplay', pointer, [pointer])(None)
    if not call('eglInitialize', integer, [pointer, pointer, pointer])(display, None, None):
        raise RuntimeError('EGL initialization failed')
    if not call('eglBindAPI', integer, [integer])(0x30a2):
        raise RuntimeError('OpenGL API unavailable')
    attributes = (integer * 11)(0x3033, 1, 0x3040, 8, 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3038)
    config, count = pointer(), integer()
    if not call('eglChooseConfig', integer, [pointer, pointer, pointer, integer, pointer])(
            display, attributes, c.byref(config), 1, c.byref(count)) or not count.value:
        raise RuntimeError('OpenGL configuration unavailable')
    context = call('eglCreateContext', pointer, [pointer, pointer, pointer, pointer])(
        display, config, None, (integer * 1)(0x3038))
    surface = call('eglCreatePbufferSurface', pointer, [pointer, pointer, pointer])(
        display, config, (integer * 5)(0x3057, 1, 0x3056, 1, 0x3038))
    if not context or not surface or not call('eglMakeCurrent', integer, [pointer] * 4)(
            display, surface, surface, context):
        raise RuntimeError('OpenGL context unavailable')
    gl.glGetString.restype = c.c_char_p
    renderer = (gl.glGetString(0x1f01) or b'').decode('utf-8', 'replace')
    version = (gl.glGetString(0x1f02) or b'').decode('utf-8', 'replace')
    call('eglMakeCurrent', integer, [pointer] * 4)(display, None, None, None)
    call('eglDestroySurface', integer, [pointer, pointer])(display, surface)
    call('eglDestroyContext', integer, [pointer, pointer])(display, context)
    call('eglTerminate', integer, [pointer])(display)
    if not renderer or any(x in renderer.lower() for x in ('llvmpipe', 'softpipe', 'software', 'swrast')):
        raise RuntimeError('software OpenGL renderer: ' + renderer)
    return {'renderer': renderer, 'version': version}


def select(qemu, gpu='auto', vga='auto', system=None, environment=None):
    env = dict(os.environ if environment is None else environment)
    system = system or platform.system()
    if vga != 'auto':
        return ['-vga', vga], env, 'explicit VGA=' + vga
    if gpu == 'off' or system != 'Linux':
        if gpu == 'on':
            raise RuntimeError('GPU=on requires Linux QEMU with SDL/OpenGL and VirGL')
        return ['-vga', 'std'], env, 'software framebuffer (' + system + ')'
    try:
        device_help = subprocess.run([qemu, '-device', 'help'], check=True,
                                     capture_output=True, text=True, timeout=10).stdout
        display_help = subprocess.run([qemu, '-display', 'help'], check=True,
                                      capture_output=True, text=True, timeout=10).stdout
        if ('virtio-vga-gl' not in device_help or 'virtio-tablet-pci' not in device_help
                or '\nsdl\n' not in display_help):
            raise RuntimeError('QEMU lacks virtio-vga-gl, virtio-tablet-pci or SDL display support')
        # WSLg has no GBM/DMABUF path. Its actual GPU is exposed through D3D12.
        if pathlib.Path('/dev/dxg').exists():
            env.setdefault('MESA_LOADER_DRIVER_OVERRIDE', 'd3d12')
            env.setdefault('GALLIUM_DRIVER', 'd3d12')
        probe = subprocess.run([sys.executable, str(pathlib.Path(__file__).resolve()), '--probe-gl'],
                               env=env, check=True, capture_output=True, text=True, timeout=15)
        info = json.loads(probe.stdout)
        return ['-vga', 'none', '-device', 'virtio-vga-gl', '-device', 'virtio-tablet-pci', '-display', 'sdl,gl=on'], env, 'VirGL / ' + info['renderer']
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
        if gpu == 'on':
            raise RuntimeError('GPU=on unavailable: ' + str(exc)) from exc
        return ['-vga', 'std'], env, 'software framebuffer; GPU unavailable: ' + str(exc)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', default='qemu-system-i386')
    parser.add_argument('--gpu', choices=('auto', 'on', 'off'), default='auto')
    parser.add_argument('--vga', default='auto')
    parser.add_argument('--print-command', action='store_true')
    parser.add_argument('--probe-gl', action='store_true')
    parser.add_argument('arguments', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    try:
        if args.probe_gl:
            print(json.dumps(probe_gl()))
            return 0
        video, env, reason = select(args.qemu, args.gpu, args.vga)
        print('MyOS display: ' + reason, flush=True)
        rest = args.arguments[1:] if args.arguments[:1] == ['--'] else args.arguments
        command = [args.qemu, *video, *rest]
        if args.print_command:
            print(shlex.join(command))
            return 0
        os.execvpe(args.qemu, command, env)
    except (OSError, RuntimeError) as exc:
        parser.exit(1, str(exc) + '\n')


if __name__ == '__main__':
    sys.exit(main())
