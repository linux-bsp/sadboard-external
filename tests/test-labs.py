#!/usr/bin/env python3
"""Exercise the intentional defect, reference repair and command boundaries."""
import os
from pathlib import Path
import resource
import shutil
import signal
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1] / 'package/sadboard-labs'


class Labs(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='sadboard-labs-')
        cls.build = Path(cls.tmp.name) / 'build'
        shutil.copytree(SOURCE, cls.build)
        subprocess.run(['make', '-C', str(cls.build), 'CFLAGS=-Wall -Wextra -Werror'], check=True,
                       stdout=subprocess.DEVNULL)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def execute(self, binary, *args):
        def no_core():
            resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        return subprocess.run([str(self.build / binary), *args], capture_output=True,
                              text=True, preexec_fn=no_core, timeout=5)

    def test_fault_is_missing_record_only(self):
        self.assertEqual(self.execute('crash-bug', '1').stdout, 'record=1 value=64\n')
        self.assertEqual(self.execute('crash-bug', '42').returncode, -signal.SIGSEGV)

    def test_repair_retains_valid_behavior_and_rejects_missing_record(self):
        self.assertEqual(self.execute('crash-fixed', '1').stdout, 'record=1 value=64\n')
        result = self.execute('crash-fixed', '42')
        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stderr, 'record 42 not found\n')

    def test_invalid_inputs_do_not_trigger_the_exercise(self):
        for binary in ('crash-bug', 'crash-fixed'):
            for args in ((), ('bad',), ('-1',), ('999999999999999999999',), ('',)):
                with self.subTest(binary=binary, args=args):
                    self.assertEqual(self.execute(binary, *args).returncode, 64)

    def test_launcher_is_explicit_and_reference_check_is_repeatable(self):
        env = dict(os.environ, SADBOARD_LIBEXEC=str(self.build))
        for _ in range(2):
            result = subprocess.run(['sh', str(self.build / 'sadboard'), 'verify', 'crash'],
                                    env=env, capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0)
            self.assertIn('Reference repair passed', result.stdout)
        for args in ((), ('run', 'unknown'), ('run', 'crash', 'extra'), ('verify',)):
            result = subprocess.run(['sh', str(self.build / 'sadboard'), *args],
                                    env=env, capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 64)


if __name__ == '__main__':
    unittest.main()
