#!/usr/bin/env python3
"""Build RAM-only AM625 PID1 loader exercises; never modify an OTA image."""
import gzip
import hashlib
import json
import os
from pathlib import Path
import stat
import shutil
import subprocess
import sys
import tempfile


def records(data):
    offset = 0
    while True:
        header = data[offset:offset + 110]
        if header[:6] != b'070701':
            raise ValueError('Expected newc initramfs')
        fields = [int(header[6 + n * 8:14 + n * 8], 16) for n in range(13)]
        offset += 110
        name = data[offset:offset + fields[11] - 1].decode()
        offset = (offset + fields[11] + 3) & ~3
        payload = data[offset:offset + fields[6]]
        offset = (offset + fields[6] + 3) & ~3
        if name == 'TRAILER!!!':
            return
        yield name, fields, payload


def archive(items):
    data = bytearray()
    for inode, (name, fields, payload) in enumerate(items + [('TRAILER!!!', [0] * 13, b'')], 1):
        fields = fields.copy()
        fields[0], fields[6], fields[11] = inode, len(payload), len(name.encode()) + 1
        data += b'070701' + ''.join(f'{x:08x}' for x in fields).encode() + name.encode() + b'\0'
        data += b'\0' * (-len(data) % 4)
        data += payload
        data += b'\0' * (-len(data) % 4)
    data += b'\0' * (-len(data) % 512)
    return gzip.compress(data, mtime=0)


def build(images, host):
    destination = images / 'sadboard'
    destination.mkdir(exist_ok=True)
    shutil.copyfile(images / 'verity-k3-am625-alientek.dtb', destination / 'verity.dtb')
    compiler = next(iter(sorted((host / 'bin').glob('*-linux-*-gcc'))))
    source = Path(__file__).resolve().parent.parent / 'package/sadboard-labs/src/boot-init.c'
    library_source = source.with_name('boot-init-library.c')
    with tempfile.TemporaryDirectory(prefix='sadboard-boot-') as temporary:
        work = Path(temporary)
        subprocess.run([str(compiler), '-Wall', '-Wextra', '-Werror', '-g3', '-O0', '-shared', '-fPIC',
                        '-Wl,-soname,libsadboard-init.so', str(library_source),
                        '-o', str(work / 'libsadboard-init.so')], check=True)
        subprocess.run([str(compiler), '-Wall', '-Wextra', '-Werror', '-g3', '-O0', str(source), '-L' + str(work),
                        '-lsadboard-init', '-Wl,-rpath,/usr/lib', '-o', str(work / 'init')], check=True)
        base = list(records(gzip.decompress((images / 'initramfs.cpio.gz').read_bytes())))
        assert any(name == 'usr/sbin/fwctl' for name, _, _ in base)
        entries = []
        for name, fields, payload in base:
            if name == 'init':
                payload = (work / 'init').read_bytes()
                fields = fields.copy(); fields[1] = stat.S_IFREG | 0o755
            entries.append((name, fields, payload))
        if not any(name == 'usr/lib' for name, _, _ in entries):
            fields = [0] * 13; fields[1] = stat.S_IFDIR | 0o755; fields[4] = 2
            entries.append(('usr/lib', fields, b''))
        (destination / 'init-missing-library.cpio.gz').write_bytes(archive(entries))
        fields = [0] * 13; fields[1] = stat.S_IFREG | 0o755; fields[4] = 1
        entries.append(('usr/lib/libsadboard-init.so', fields, (work / 'libsadboard-init.so').read_bytes()))
        (destination / 'init-complete.cpio.gz').write_bytes(archive(entries))
        for name in ('init', 'libsadboard-init.so'):
            (destination / name).write_bytes((work / name).read_bytes())
    files = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(destination.iterdir())
             if p.is_file() and p.name != 'manifest.json'}
    (destination / 'manifest.json').write_text(json.dumps(dict(
        format='SADBOARD-RAM-BOOT-1', sha256=files,
        firmware_id=json.loads((images / 'firmware-identity.json').read_text())['firmware_id'],
        reference_build=(images / 'netboot/build.conf').read_text().split()[2],
        image_sha256=hashlib.sha256((images / 'netboot/Image').read_bytes()).hexdigest(),
        base_initramfs_sha256=hashlib.sha256((images / 'initramfs.cpio.gz').read_bytes()).hexdigest()), indent=2) + '\n')
    print('SadBoard RAM-only boot exercises:', destination)


if __name__ == '__main__':
    build(Path(sys.argv[1]).resolve(), Path(os.environ['HOST_DIR']).resolve())
