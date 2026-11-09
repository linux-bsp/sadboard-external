#!/usr/bin/env python3
"""Exercise the intentional defect, reference repair and command boundaries."""
import os
import json
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
            result = subprocess.run(['sh', str(self.build / 'execute'), 'verify', 'crash'],
                                    env=env, capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0)
            self.assertIn('Reference repair passed', result.stdout)
        for args in ((), ('run', 'unknown'), ('run', 'crash', 'extra'), ('verify',)):
            result = subprocess.run(['sh', str(self.build / 'sadboard'), *args],
                                    env=env, capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 64)

    def test_cpu_lookup_preserves_every_query(self):
        for items, queries, seed in ((1, 4, 0), (97, 1000, 42), (4096, 5000, 4294967295)):
            state, checksum = seed, 0
            for _ in range(queries):
                state = (state * 1664525 + 1013904223) & 0xffffffff
                checksum += (state % items) * 3 + 7
            for mode in ('bug', 'fixed'):
                result = self.execute('cpu-' + mode, str(items), str(queries), str(seed))
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(json.loads(result.stdout)['checksum'], checksum)

    def test_memory_repair_preserves_work(self):
        for mode in ('bug', 'fixed'):
            result = self.execute('memory-' + mode, '4', '0')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout), dict(chunks=4, checksum=2560))

    def test_workers_complete_the_same_work(self):
        for mode in ('bug', 'fixed'):
            result = self.execute('lock-' + mode, '4', '1000')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout)['completed'], 8)

    def test_workloads_reject_unbounded_or_invalid_arguments(self):
        for name, args in (('cpu', ('16384', '2000000', '42')),
                           ('cpu', ('4096', '2', '4294967296')),
                           ('memory', ('128', '200')), ('lock', ('200', '100000')),
                           ('memory', ('129', '0')), ('lock', ('0', '1')),
                           ('cpu', ('-1', '1', '1'))):
            for mode in ('bug', 'fixed'):
                self.assertEqual(self.execute(name + '-' + mode, *args).returncode, 64)

    def test_reference_check_rejects_early_success_without_results(self):
        with tempfile.TemporaryDirectory(prefix='sadboard-empty-result-') as directory:
            tools = Path(directory)
            for name in ('cpu-bug', 'cpu-fixed', 'lock-bug', 'lock-fixed'):
                p = tools / name
                p.write_text('#!/bin/sh\nexit 0\n')
                p.chmod(0o755)
            env = dict(os.environ, SADBOARD_LIBEXEC=directory, SADBOARD_RESULTS=directory)
            for case, args in (('cpu', ['1', '2', '0']), ('lock', ['1', '1'])):
                result = subprocess.run(['sh', str(self.build / 'execute'), 'verify', case, *args],
                                        env=env, capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, 1)
                self.assertNotIn('Reference business results match', result.stdout)

    def test_controller_rejects_malformed_parameters_before_launch(self):
        for args in (('run', 'cpu', '1', '2'), ('run', 'lock', '1', 'bad'),
                     ('verify', 'crash', '42'), ('run', 'crash', '-1'),
                     ('run', 'cpu', '1', '2', '0;touch /tmp/should-not-exist')):
            result = subprocess.run(['sh', str(self.build / 'sadboard'), *args],
                                    capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 64)


if __name__ == '__main__':
    unittest.main()
