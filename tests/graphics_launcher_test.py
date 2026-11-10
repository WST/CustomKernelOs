import sys
sys.dont_write_bytecode = True
import importlib.util
import pathlib
import subprocess
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('run_qemu', pathlib.Path(__file__).parents[1] / 'tools/run_qemu.py')
launcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(launcher)


class LauncherTest(unittest.TestCase):
    def test_mac_uses_software_without_loading_opengl(self):
        with patch.object(launcher.subprocess, 'run', side_effect=AssertionError('must not probe Linux GL')):
            video, env, reason = launcher.select('qemu', system='Darwin', environment={})
        self.assertEqual(video, ['-vga', 'std'])

    def test_explicit_adapter_is_preserved(self):
        self.assertEqual(launcher.select('qemu', vga='vmware')[0], ['-vga', 'vmware'])
        self.assertEqual(launcher.select('qemu', vga='std')[0], ['-vga', 'std'])

    def test_gpu_off_does_not_probe(self):
        with patch.object(launcher.subprocess, 'run', side_effect=AssertionError('must not probe')):
            self.assertEqual(launcher.select('qemu', gpu='off', system='Linux')[0], ['-vga', 'std'])

    def test_missing_backend_falls_back(self):
        result = subprocess.CompletedProcess([], 0, stdout='no accelerated devices')
        with patch.object(launcher.subprocess, 'run', return_value=result):
            self.assertEqual(launcher.select('qemu', system='Linux')[0], ['-vga', 'std'])

    def test_forced_gpu_fails_instead_of_silently_using_cpu(self):
        with self.assertRaises(RuntimeError):
            launcher.select('qemu', gpu='on', system='Darwin')
        with patch.object(launcher.subprocess, 'run', side_effect=OSError('missing QEMU')):
            with self.assertRaises(RuntimeError):
                launcher.select('qemu', gpu='on', system='Linux')

    def test_hardware_renderer_enables_virgl(self):
        results = [subprocess.CompletedProcess([], 0, stdout='name "virtio-vga-gl"\nname "virtio-tablet-pci"'),
                   subprocess.CompletedProcess([], 0, stdout='Available:\nsdl\n'),
                   subprocess.CompletedProcess([], 0, stdout='{"renderer":"D3D12 (AMD)","version":"4.6"}')]
        with patch.object(launcher.subprocess, 'run', side_effect=results):
            video, env, reason = launcher.select('qemu', system='Linux', environment={})
        self.assertIn('virtio-vga-gl', video)
        self.assertIn('virtio-tablet-pci', video)
        self.assertIn('D3D12 (AMD)', reason)

    def test_gpu_requires_absolute_pointer_device(self):
        result = subprocess.CompletedProcess([], 0, stdout='name "virtio-vga-gl"\nsdl\n')
        with patch.object(launcher.subprocess, 'run', return_value=result) as run:
            self.assertEqual(launcher.select('qemu', system='Linux')[0], ['-vga', 'std'])
            self.assertEqual(run.call_count, 2)  # no GL probe or guest boot

    def test_software_gl_probe_failure_falls_back(self):
        results = [subprocess.CompletedProcess([], 0, stdout='name "virtio-vga-gl"\nname "virtio-tablet-pci"'),
                   subprocess.CompletedProcess([], 0, stdout='Available:\nsdl\n'),
                   subprocess.CalledProcessError(1, ['probe'], stderr='software OpenGL renderer: llvmpipe')]
        with patch.object(launcher.subprocess, 'run', side_effect=results):
            self.assertEqual(launcher.select('qemu', system='Linux')[0], ['-vga', 'std'])


if __name__ == '__main__':
    unittest.main()
