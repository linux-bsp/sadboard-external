#!/usr/bin/env python3
"""Install a built SadBoard NFS root and TFTP files on a Debian development host."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('images', type=Path)
    parser.add_argument('--nfs-root', type=Path, default=Path('/srv/nfs/sadboard-am625'))
    parser.add_argument('--tftp-root', type=Path, default=Path('/srv/tftp/am625'))
    parser.add_argument('--client', default='192.168.10.211')
    args = parser.parse_args()
    import ipaddress
    client = str(ipaddress.IPv4Address(args.client))
    images = args.images.resolve()
    root = args.nfs_root.absolute()
    tftp = args.tftp_root.absolute()
    if os.geteuid() != 0:
        parser.error('run as root to preserve rootfs ownership and configure /etc/exports.d')
    if root.is_symlink() or (root.exists() and any(root.iterdir())):
        parser.error('NFS destination must be empty; stop the board and archive the previous root before redeploying')
    for path in (root, tftp):
        if any(c.isspace() for c in str(path)) or ',' in str(path):
            parser.error('export paths must not contain whitespace or commas')
    manifest = json.loads((images / 'netboot/manifest.json').read_text())
    required = {'netboot/Image', 'netboot/k3-am625-alientek.dtb', 'netboot/build.conf', 'rootfs.tar'}
    if manifest.get('format') != 'SADBOARD-NETBOOT-1' or set(manifest['sha256']) != required:
        parser.error('invalid netboot manifest')
    for name, expected in manifest['sha256'].items():
        digest = hashlib.sha256()
        with (images / name).open('rb') as stream:
            for data in iter(lambda: stream.read(1024 * 1024), b''):
                digest.update(data)
        if digest.hexdigest() != expected:
            parser.error(f'artifact checksum mismatch: {name}')
    if not Path('/usr/sbin/exportfs').is_file():
        parser.error('install nfs-kernel-server first')
    root.mkdir(parents=True, exist_ok=True)
    # Buildroot rootfs.tar was produced under fakeroot, unlike target/.
    subprocess.run(['tar', '--numeric-owner', '-xf', str(images / 'rootfs.tar'), '-C', str(root)], check=True)
    if (root / 'etc/sadboard/build.conf').read_text().strip() != manifest['reference_build']:
        parser.error('rootfs and kernel reference builds do not match')
    tftp.mkdir(parents=True, exist_ok=True)
    for name in ('Image', 'k3-am625-alientek.dtb'):
        source = images / 'netboot' / name
        temporary = tftp / (name + '.new')
        shutil.copyfile(source, temporary)
        temporary.chmod(0o644)
        temporary.replace(tftp / name)
    exports = Path('/etc/exports.d')
    exports.mkdir(exist_ok=True)
    (exports / 'sadboard.exports').write_text(f'{root} {client}(rw,sync,no_root_squash,no_subtree_check)\n')
    subprocess.run(['systemctl', 'enable', '--now', 'nfs-server.service'], check=True)
    subprocess.run(['/usr/sbin/exportfs', '-ra'], check=True)
    print(f'TFTP: {tftp}/Image and k3-am625-alientek.dtb')
    print(f'NFS: {root}; allowed client: {client}')


if __name__ == '__main__':
    main()
