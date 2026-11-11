#!/usr/bin/env python3
"""Exercise artifact preflight and sudo stdin/parent-PID handling without root."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest

EXTERNAL = Path(__file__).resolve().parents[1]


@unittest.skipIf(os.geteuid() == 0, 'sudo authentication is tested as a normal user')
class SyncArtifacts(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        for relative, source in (
                ('sadboard-external/scripts/sync-artifacts.sh', EXTERNAL / 'scripts/sync-artifacts.sh'),
                ('bsp-base-external/support/scripts/deploy-netboot.sh',
                 EXTERNAL.parent / 'bsp-base-external/support/scripts/deploy-netboot.sh')):
            destination = self.root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, destination)
        self.script = self.root / 'sadboard-external/scripts/sync-artifacts.sh'
        self.images = self.root / 'output/images'
        (self.images / 'netboot').mkdir(parents=True)
        host = self.root / 'output/host/bin'
        host.mkdir(parents=True)
        (host / 'jq').symlink_to(os.environ.get('NETBOOT_TEST_JQ') or shutil.which('jq'))
        record = 'BSPNET1 1 ' + 'a' * 64 + '\n'
        (self.images / 'netboot/Image').write_bytes(b'kernel')
        (self.images / 'netboot/board.dtb').write_bytes(b'dtb')
        (self.images / 'netboot/build.conf').write_text(record)
        with tarfile.open(self.images / 'rootfs.tar', 'w') as archive:
            archive.add(self.images / 'netboot/build.conf', arcname='./etc/bsp/netboot.conf')
        names = ('netboot/Image', 'netboot/board.dtb', 'netboot/build.conf', 'rootfs.tar')
        manifest = dict(format='BSP-NETBOOT-1', dtb='board.dtb', reference_build=record.rstrip(),
                        sha256={name: hashlib.sha256((self.images / name).read_bytes()).hexdigest()
                                for name in names})
        (self.images / 'netboot/manifest.json').write_text(json.dumps(manifest))
        self.secret = self.root / 'private password;literal'
        self.password = 'fixture-only-$(touch SHOULD_NOT_EXIST)\n'
        self.secret.write_text(self.password)
        self.secret.chmod(0o600)
        self.calls = self.root / 'sudo.jsonl'
        binary = self.root / 'bin'
        binary.mkdir()
        sudo = binary / 'sudo'
        sudo.write_text('#!' + sys.executable + '\n' + r'''
import json, os, pathlib, sys
args = sys.argv[1:]
entry = {'args': args, 'parent': os.getppid()}
code = 0
if args == ['-n', '-v']:
    code = 0 if os.environ.get('CACHED') == '1' else 1
elif args == ['-S', '-p', '', '-v']:
    entry['stdin_matches'] = sys.stdin.read() == pathlib.Path(os.environ['SECRET']).read_text()
    code = 1 if os.environ.get('REJECT_PASSWORD') else 0
elif args[:3] == ['-n', '--', 'bash']:
    entry['stdin_empty'] = sys.stdin.read() == ''
else:
    code = 90
with open(os.environ['CALLS'], 'a') as stream:
    stream.write(json.dumps(entry) + '\n')
sys.exit(code)
''')
        sudo.chmod(0o755)
        self.env = dict(os.environ, PATH=str(binary) + ':' + os.environ['PATH'],
                        CALLS=str(self.calls), SECRET=str(self.secret))

    def invoke(self, *arguments):
        result = subprocess.run(['bash', str(self.script), str(self.images),
                                 '--password-file', str(self.secret), *arguments],
                                env=self.env, capture_output=True, text=True)
        self.assertNotIn(self.password.rstrip(), result.stdout + result.stderr)
        self.assertFalse((self.root / 'SHOULD_NOT_EXIST').exists())
        return result

    def records(self):
        return [json.loads(line) for line in self.calls.read_text().splitlines()] if self.calls.exists() else []

    def test_password_is_stdin_only_and_sudo_parent_stays_stable(self):
        self.assertEqual(self.invoke().returncode, 0)
        calls = self.records()
        self.assertEqual(len(calls), 3)
        self.assertTrue(calls[1]['stdin_matches'])
        self.assertTrue(calls[2]['stdin_empty'])
        self.assertEqual(len({call['parent'] for call in calls}), 1)
        self.assertNotIn(self.password.rstrip(), self.calls.read_text())

    def test_cached_auth_does_not_require_a_password_file(self):
        self.secret.unlink()
        self.env['CACHED'] = '1'
        self.assertEqual(self.invoke().returncode, 0)
        self.assertEqual(len(self.records()), 2)

    def test_rejected_password_stops_before_elevated_command(self):
        self.env['REJECT_PASSWORD'] = '1'
        self.assertNotEqual(self.invoke().returncode, 0)
        self.assertEqual(len(self.records()), 2)

    def test_shared_password_file_is_rejected(self):
        self.secret.chmod(0o644)
        self.assertNotEqual(self.invoke().returncode, 0)
        self.assertEqual(len(self.records()), 1)

    def test_preflight_needs_no_auth_and_corruption_never_reaches_sudo(self):
        self.assertEqual(self.invoke('--check').returncode, 0)
        self.assertFalse(self.records())
        (self.images / 'netboot/Image').write_bytes(b'corrupted')
        self.assertNotEqual(self.invoke().returncode, 0)
        self.assertFalse(self.records())


if __name__ == '__main__':
    unittest.main()
