#!/usr/bin/env python3
"""Exercise the intentional defect, reference repair and command boundaries."""
import os
import importlib.util
import json
from pathlib import Path
import resource
import shutil
import signal
import struct
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
        subprocess.run(['make', '-B', '-C', str(cls.build), 'CFLAGS=-Wall -Wextra -Werror'], check=True,
                       stdout=subprocess.DEVNULL)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def execute(self, binary, *args):
        def no_core():
            resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        return subprocess.run([str(self.build / binary), *args], capture_output=True,
                              text=True, preexec_fn=no_core, timeout=5)

    def test_debug_gate_rejects_wrong_architecture_and_truncated_elf(self):
        spec = importlib.util.spec_from_file_location('debug_verifier', SOURCE.parents[1] / 'scripts/verify-debug.py')
        verifier = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(verifier)
        path = Path(self.tmp.name) / 'elf-header'
        header = bytearray(64)
        header[:6] = b'\x7fELF\x02\x01'
        header[18:20] = (183).to_bytes(2, 'little')
        path.write_bytes(header)
        verifier.target_elf(path)
        for machine in (62, 40):  # x86-64 and ARM32 are not the AM625 target.
            bad = bytearray(header); bad[18:20] = machine.to_bytes(2, 'little')
            path.write_bytes(bad)
            with self.assertRaises(ValueError): verifier.target_elf(path)
        for bad in (header[:20], b'#!/bin/sh\n', header[:5] + b'\x02' + header[6:]):
            path.write_bytes(bad)
            with self.assertRaises(ValueError): verifier.target_elf(path)

    def test_descriptor_exhaustion_and_close_preserve_work(self):
        before = resource.getrlimit(resource.RLIMIT_NOFILE)
        for mode in ('bug', 'fixed'):
            r = self.execute('fd-' + mode, '128', '0')
            v = json.loads(r.stdout)
            self.assertEqual(v['limit'], 64)
            self.assertEqual(v['requested'], 128)
            if mode == 'bug':
                self.assertEqual((r.returncode, v['error'], v['final_open']), (24, 24, 64))
                self.assertEqual(v['completed'], 64 - v['baseline'])
            else:
                self.assertEqual((r.returncode, v['completed'], v['error']), (0, 128, 0))
                self.assertEqual(v['final_open'], v['baseline'])
                self.assertEqual(v['peak_open'], v['baseline'] + 1)
        self.assertEqual(resource.getrlimit(resource.RLIMIT_NOFILE), before)
        # Extra inherited descriptors affect capacity, not the correctness criterion.
        with open('/dev/null') as extra:
            r = subprocess.run([str(self.build / 'fd-bug'), '128', '0'], pass_fds=(extra.fileno(),),
                               capture_output=True, text=True, timeout=5)
        v = json.loads(r.stdout)
        self.assertGreaterEqual(v['baseline'], 4)
        self.assertEqual(v['completed'] + v['baseline'], 64)

    def test_descriptor_small_input_is_not_exhaustion_and_limits_are_bounded(self):
        v = json.loads(self.execute('fd-bug', '4', '0').stdout)
        self.assertEqual((v['completed'], v['error']), (4, 0))
        self.assertEqual(v['final_open'], v['baseline'] + 4)
        for args in (('0', '0'), ('4097', '0'), ('128', '51'), ('4096', '50'), ('-1', '0'), ('bad', '0'), ('1',)):
            self.assertEqual(self.execute('fd-bug', *args).returncode, 64)
        def low_limit():
            resource.setrlimit(resource.RLIMIT_NOFILE, (32, 32))
        r = subprocess.run([str(self.build / 'fd-bug'), '4', '0'], preexec_fn=low_limit,
                           capture_output=True, timeout=5)
        self.assertEqual(r.returncode, 75)

    def test_descriptor_reference_and_evidence_contract(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)
            env = dict(os.environ, SADBOARD_LIBEXEC=str(self.build), SADBOARD_RUN=tmp, SADBOARD_RESULTS=tmp)
            r = subprocess.run(['sh', str(self.build / 'execute'), 'verify', 'fd', '128', '0'],
                               env=env, capture_output=True, text=True, timeout=5)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(self.check_evidence(p, 'fd', 'verify', 0, '128', '0').returncode, 0)
            original = (p / 'fd-fixed.out').read_text()
            wrong = json.loads(original);wrong['completed'] = 1
            (p / 'fd-fixed.out').write_text(json.dumps(wrong))
            self.assertEqual(self.check_evidence(p, 'fd', 'verify', 0, '128', '0').returncode, 1)
            (p / 'fd-fixed.out').write_text(original)
            self.assertEqual(self.check_evidence(p, 'fd', 'verify', 0, '127', '0').returncode, 1)
            (p / 'fd-bug.exit').write_text('0\n')
            self.assertEqual(self.check_evidence(p, 'fd', 'verify', 0, '128', '0').returncode, 1)

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
        env = dict(os.environ, SADBOARD_LIBEXEC=str(self.build), SADBOARD_RESULTS=str(self.build / 'results'))
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
            (tools / 'check-result').symlink_to(self.build / 'check-result')
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
                     ('verify', 'panic'), ('run', 'network', 'eth0'),
                     ('run', 'cpu', '1', '2', '0;touch /tmp/should-not-exist')):
            result = subprocess.run(['sh', str(self.build / 'sadboard'), *args],
                                    capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 64)

    def check_evidence(self, directory, case, mode, code, *args):
        return self.execute('check-result', case, mode, str(directory), str(code), *map(str, args))

    def test_deadline_raw_timestamps_and_preserved_jobs(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)
            for mode in ('bug', 'fixed'):
                result = self.execute('deadline-' + mode, '20', '2000', '500', str(p / ('deadline-' + mode + '.csv')))
                self.assertEqual(result.returncode, 0, result.stderr)
                (p / ('deadline-' + mode + '.out')).write_text(result.stdout)
                self.assertEqual(json.loads(result.stdout)['completed'], 20)
            self.assertEqual(self.check_evidence(p, 'deadline', 'verify', 0, 20, 2000, 500).returncode, 0)
            csv = p / 'deadline-fixed.csv'; original = csv.read_text()
            csv.write_text('\n'.join(original.splitlines()[:-1]) + '\n')
            self.assertEqual(self.check_evidence(p, 'deadline', 'verify', 0, 20, 2000, 500).returncode, 1)
            csv.write_text(original)
            metrics = p / 'deadline-fixed.out'; data = json.loads(metrics.read_text()); data['p99_ns'] += 1
            metrics.write_text(json.dumps(data))
            self.assertEqual(self.check_evidence(p, 'deadline', 'verify', 0, 20, 2000, 500).returncode, 1)

    def test_backlog_bounds_live_memory_without_dropping_work(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)
            for mode in ('bug', 'fixed'):
                result = self.execute('backlog-' + mode, '32', '4', str(p / ('backlog-' + mode + '.csv')))
                self.assertEqual(result.returncode, 0, result.stderr)
                (p / ('backlog-' + mode + '.out')).write_text(result.stdout)
                data = json.loads(result.stdout)
                self.assertEqual((data['completed'], data['allocations'], data['frees'], data['final_live_bytes']), (32, 32, 32, 0))
                if mode == 'fixed':self.assertLessEqual(data['peak_live_bytes'], 9 * 4096)
                else:self.assertGreater(data['peak_queue'], 8)
            self.assertEqual(self.check_evidence(p, 'backlog', 'verify', 0, 32, 4).returncode, 0)
            metrics = p / 'backlog-fixed.out'; data = json.loads(metrics.read_text()); data['completed'] -= 1
            metrics.write_text(json.dumps(data))
            self.assertEqual(self.check_evidence(p, 'backlog', 'verify', 0, 32, 4).returncode, 1)

    def test_tcp_stream_and_batching_contract_on_real_loopback_sockets(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory);calls = []
            for mode in ('bug', 'fixed'):
                result = self.execute('tcp-' + mode, '512')
                self.assertEqual(result.returncode, 0, result.stderr)
                (p / ('tcp-' + mode + '.out')).write_text(result.stdout)
                data = json.loads(result.stdout)
                self.assertEqual((data['completed'], data['bytes']), (512, 512 * 64))
                calls.append(data['send_calls'])
            self.assertLess(calls[1], calls[0])
            self.assertEqual(self.check_evidence(p, 'tcp', 'verify', 0, 512).returncode, 0)
            metrics = p / 'tcp-fixed.out'; data = json.loads(metrics.read_text()); data['checksum'] ^= 1
            metrics.write_text(json.dumps(data))
            self.assertEqual(self.check_evidence(p, 'tcp', 'verify', 0, 512).returncode, 1)

    def test_advanced_inputs_are_bounded_before_work(self):
        for binary, args in [('deadline-bug', ('1', '2000', '500', '/dev/null')),
                             ('deadline-fixed', ('500', '50000', '1', '/dev/null')),
                             ('deadline-bug', ('20', '1000', '1000', '/dev/null')),
                             ('backlog-bug', ('15', '64', '/dev/null')),
                             ('backlog-fixed', ('1025', '64', '/dev/null')),
                             ('tcp-bug', ('0',)), ('tcp-fixed', ('32769',))]:
            self.assertEqual(self.execute(binary, *args).returncode, 64)

    def test_cpu_checker_requires_complete_correct_work_not_just_a_keyword(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)
            for mode in ('bug', 'fixed'):
                (p / ('cpu-' + mode + '.out')).write_text(self.execute('cpu-' + mode, '97', '1000', '42').stdout)
            valid = (p / 'cpu-fixed.out').read_text()
            self.assertEqual(self.check_evidence(p, 'cpu', 'verify', 0, 97, 1000, 42).returncode, 0)
            data = json.loads(valid)
            for invalid in ('', valid[:-2], valid + 'completed\n', valid.replace('"checksum":', '"checksum":0,"checksum":'),
                            json.dumps(dict(data, checksum=0)), json.dumps(dict(data, queries=999)),
                            json.dumps(dict(data, checksum='0')), json.dumps(dict(data, wall_ns=18446744073709551616))):
                (p / 'cpu-fixed.out').write_text(invalid)
                self.assertEqual(self.check_evidence(p, 'cpu', 'verify', 0, 97, 1000, 42).returncode, 1, invalid)

    def test_lock_checker_rejects_lost_work_and_skipped_wait(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)
            for mode in ('bug', 'fixed'):
                (p / ('lock-' + mode + '.out')).write_text(self.execute('lock-' + mode, '4', '1000').stdout)
            self.assertEqual(self.check_evidence(p, 'lock', 'verify', 0, 4, 1000).returncode, 0)
            data = json.loads((p / 'lock-fixed.out').read_text())
            for change in (dict(completed=0), dict(iterations=3), dict(wall_ns=1)):
                (p / 'lock-fixed.out').write_text(json.dumps(dict(data, **change)))
                self.assertEqual(self.check_evidence(p, 'lock', 'verify', 0, 4, 1000).returncode, 1)

    def test_crash_checker_rejects_empty_truncated_or_executable_core(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory);core = p / 'core.crash-bug.123'
            machine = struct.unpack_from('<H', (self.build / 'crash-bug').read_bytes(), 18)[0]
            ident = b'\x7fELF\x02\x01\x01' + b'\0' * 9
            note = struct.pack('<III', 5, 16, 1) + b'CORE\0\0\0\0' + b'\0' * 16
            header = struct.pack('<16sHHIQQQIHHHHHH', ident, 4, machine, 1, 0, 64, 0, 0, 64, 56, 1, 0, 0, 0)
            valid = header + struct.pack('<IIQQQQQQ', 4, 0, 120, 0, 0, len(note), len(note), 4) + note
            core.write_bytes(valid)
            self.assertEqual(self.check_evidence(p, 'crash', 'run', 139, 42).returncode, 0)
            for invalid in (b'', b'\x7fELF', valid[:80], valid[:-1], valid[:16] + b'\x02\x00' + valid[18:]):
                core.write_bytes(invalid)
                self.assertEqual(self.check_evidence(p, 'crash', 'run', 139, 42).returncode, 1)

    def test_oom_checker_requires_group_oom_limit_and_completed_reference(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)
            for mode, code, count in (('bug', 137, 2), ('fixed', 0, 0)):
                (p / ('memory-' + mode + '.exit')).write_text(str(code) + '\n')
                (p / ('memory-' + mode + '.events')).write_text('oom '+str(int(count>0))+'\noom_kill '+str(count)+'\n')
                (p / ('memory-' + mode + '.peak')).write_text('33554432\n')
                (p / ('memory-' + mode + '.limits')).write_text('memory.max=33554432\nmemory.swap.max=0\nmemory.oom.group=1\n')
            (p / 'memory-fixed.out').write_text('{"chunks":64,"checksum":532480}\n')
            self.assertEqual(self.check_evidence(p, 'oom', 'verify', 0).returncode, 0)
            (p / 'memory-bug.events').write_text('oom 0\noom_kill 2\n')
            self.assertEqual(self.check_evidence(p, 'oom', 'verify', 0).returncode, 1)
            (p / 'memory-bug.events').write_text('oom 1\noom_kill 2\n')
            (p / 'memory-fixed.out').write_text('{"chunks":1,"checksum":532480}\n')
            self.assertEqual(self.check_evidence(p, 'oom', 'verify', 0).returncode, 1)

    def test_network_checker_requires_packets_and_restored_reachability(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)
            for name, rtt in (('baseline', .3), ('delayed', 100.2), ('restored', .4)):
                (p / (name + '.ping')).write_text(f'5 packets transmitted, 5 packets received, 0% packet loss\nround-trip min/avg/max = {rtt}/{rtt}/{rtt} ms\n')
            (p / 'lost.ping').write_text('3 packets transmitted, 0 packets received, 100% packet loss\n')
            (p / 'delayed.qdisc').write_text('qdisc netem delay 100ms\n')
            (p / 'lost.qdisc').write_text('qdisc netem loss 100%\n')
            capture = struct.pack('<IHHIIII', 0xa1b2c3d4, 2, 4, 0, 0, 262144, 1)
            for protocol in (1, 6):
                ip = bytearray(20);ip[0]=0x45;ip[9]=protocol;ip[12:16]=bytes([198,18,0,1]);ip[16:20]=bytes([198,18,0,2])
                frame = b'\0'*12+b'\x08\x00'+ip
                capture += struct.pack('<IIII', 1, 0, len(frame), len(frame)) + frame
            (p / 'network.pcap').write_bytes(capture)
            self.assertEqual(self.check_evidence(p, 'network', 'verify', 0).returncode, 0)
            (p / 'network.pcap').write_bytes(capture[:-1])
            self.assertEqual(self.check_evidence(p, 'network', 'verify', 0).returncode, 1)
            (p / 'network.pcap').write_bytes(capture)
            (p / 'restored.ping').write_text('5 packets transmitted, 0 packets received, 100% packet loss\n')
            self.assertEqual(self.check_evidence(p, 'network', 'verify', 0).returncode, 1)


if __name__ == '__main__':
    unittest.main()
