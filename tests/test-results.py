#!/usr/bin/env python3
"""Check mount-aware result selection and host-readable evidence boundaries."""
import os
from pathlib import Path
import stat
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1] / 'package/sadboard-labs'


class Results(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='sadboard-results-')
        self.root = Path(self.tmp.name)
        self.mounts = self.root / 'mounts'
        self.mounts.write_text('server:/root / nfs rw 0 0\n/dev/mmcblk1p15 /data ext4 rw 0 0\n')
        self.env = dict(os.environ, SADBOARD_MOUNTS=str(self.mounts), SADBOARD_LIBEXEC=str(SOURCE))
        self.env.pop('SADBOARD_RESULTS', None)

    def tearDown(self):
        self.tmp.cleanup()

    def helper(self, command, *args):
        return subprocess.run(['sh', '-c', '. "$1"; shift; ' + command,
                               'results-test', str(SOURCE / 'results.sh'), *args],
                              env=self.env, text=True, capture_output=True, check=True)

    def cli(self, *args):
        return subprocess.run(['sh', str(SOURCE / 'sadboard'), *map(str, args)],
                              env=self.env, text=True, capture_output=True)

    def test_default_follows_actual_root_mount_not_presence_of_data(self):
        self.assertEqual(self.helper('sadboard_results_root').stdout.strip(), '/srv/sadboard')
        self.mounts.write_text('/dev/root / squashfs ro 0 0\n/dev/mmcblk1p15 /data ext4 rw 0 0\n')
        self.assertEqual(self.helper('sadboard_results_root').stdout.strip(), '/data/sadboard')
        self.mounts.write_text('server:/root / nfs4 rw 0 0\n')
        self.assertEqual(self.helper('sadboard_results_root').stdout.strip(), '/srv/sadboard')

    def test_longest_mount_prefix_and_escaped_paths(self):
        self.mounts.write_text('server:/root / nfs rw 0 0\n/dev/sd /data ext4 rw 0 0\n'
                              'tmpfs /data/cache tmpfs rw 0 0\nserver:/other /data/with\\040space nfs4 rw 0 0\n')
        for path, expected in [('/srv/sadboard/run', 'nfs'), ('/data/run', 'ext4'),
                               ('/data/cache/run', 'tmpfs'), ('/data/cache-other/run', 'ext4'),
                               ('/data/with space/run', 'nfs4')]:
            self.assertEqual(self.helper('sadboard_results_filesystem "$1"', path).stdout.strip(), expected)

    def test_override_and_manual_export_are_host_readable_without_following_links(self):
        results = self.root / 'results'
        self.env['SADBOARD_RESULTS'] = str(results)
        result = self.cli('results')
        self.assertEqual((result.returncode, result.stdout.strip()), (0, str(results)))
        self.assertEqual(stat.S_IMODE(results.stat().st_mode), 0o755)
        manual = results / 'manual-perf'; manual.mkdir(mode=0o700)
        for name in ('perf.data', 'core.example', 'recovery.tar.gz'):
            (manual / name).write_bytes(b'raw evidence')
            (manual / name).chmod(0o600)
        victim = self.root / 'private'; victim.write_text('private'); victim.chmod(0o600)
        (manual / 'outside-link').symlink_to(victim)
        result = self.cli('share', manual)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(stat.S_IMODE(manual.stat().st_mode), 0o755)
        for name in ('perf.data', 'core.example', 'recovery.tar.gz'):
            self.assertEqual(stat.S_IMODE((manual / name).stat().st_mode), 0o644)
        self.assertEqual(stat.S_IMODE(victim.stat().st_mode), 0o600)
        self.assertEqual(self.cli('share', self.root).returncode, 75)
        (results / 'directory-link').symlink_to(manual, target_is_directory=True)
        self.assertEqual(self.cli('share', results / 'directory-link').returncode, 75)

    def test_local_storage_keeps_private_permissions(self):
        self.mounts.write_text('/dev/root / ext4 rw 0 0\n')
        results = self.root / 'private-results'; results.mkdir(mode=0o700)
        self.env['SADBOARD_RESULTS'] = str(results)
        manual = results / 'manual'; manual.mkdir(mode=0o700)
        core = manual / 'core'; core.write_text('core'); core.chmod(0o600)
        self.assertEqual(self.cli('share', manual).returncode, 0)
        self.assertEqual(stat.S_IMODE(manual.stat().st_mode), 0o700)
        self.assertEqual(stat.S_IMODE(core.stat().st_mode), 0o600)


if __name__ == '__main__':
    unittest.main()
