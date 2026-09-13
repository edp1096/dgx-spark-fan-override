"""Exercise the actual CLI without root or hardware via a PATH uid stub."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

CLI = Path(__file__).resolve().parents[1] / 'nvfancontrol/usr/sbin/nvfancontrol'

class SpeedCLI(unittest.TestCase):
    def test_speed_validation_before_hardware_access(self):
        with tempfile.TemporaryDirectory() as directory:
            stub = Path(directory) / 'id'
            stub.write_text('#!/bin/sh\necho 1000\n')
            stub.chmod(0o755)
            env = dict(os.environ, PATH=directory + ':' + os.environ['PATH'])
            for value in ('1890', '7000', '9000', '13500', '09000'):
                result = subprocess.run(['bash', str(CLI), '--speed', value], env=env, capture_output=True, text=True)
                self.assertEqual(result.returncode, 1, value)
                self.assertIn('Run this command as root.', result.stderr)
            for args in (['--speed'], ['--speed','1889'], ['--speed','13501'], ['--speed','65535'], ['--speed','-1'], ['--speed','0'], ['--speed','7e3'], ['--speed','9000.0'], ['--speed','0x2328'], ['--speed','9000','extra'], ['--profile','1']):
                result = subprocess.run(['bash', str(CLI), *args], env=env, capture_output=True, text=True)
                self.assertEqual(result.returncode, 2, args)
                self.assertNotIn('Run this command as root.', result.stderr)

    def test_help(self):
        result = subprocess.run(['bash', str(CLI), '--help'], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0)
        self.assertIn('--speed', result.stderr)
        self.assertIn('1890..13500', result.stderr)

if __name__ == '__main__':
    unittest.main()
