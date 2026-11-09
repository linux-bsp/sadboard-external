#!/usr/bin/env python3
"""Embed a reference build ID for mutable network debugging roots (not an OTA ID)."""
import hashlib
import os
from pathlib import Path
import stat

target = Path(os.environ['TARGET_DIR'])
images = Path(os.environ['BINARIES_DIR'])
workspace = Path(__file__).resolve().parents[2]
destination = target / 'etc/sadboard/build.conf'
destination.parent.mkdir(parents=True, exist_ok=True)
digest = hashlib.sha256(b'SadBoard debug build v1\0')
for name in ('Image.gz', 'k3-am625-alientek.dtb'):
    digest.update(name.encode() + b'\0')
    digest.update((images / name).read_bytes())
for path in sorted(target.rglob('*')):
    if path == destination:
        continue
    info = path.lstat()
    digest.update(str(path.relative_to(target)).encode() + b'\0')
    digest.update(str(stat.S_IMODE(info.st_mode)).encode() + b'\0')
    if path.is_symlink():
        digest.update(os.readlink(path).encode())
    elif path.is_file():
        with path.open('rb') as stream:
            for data in iter(lambda: stream.read(1024 * 1024), b''):
                digest.update(data)
    digest.update(b'\0')
major, minor, patch = map(int, (workspace / 'edk/VERSION').read_text().strip().split('.'))
version = major << 16 | minor << 8 | patch
destination.write_text(f'SBNET1 {version} {digest.hexdigest()}\n')
print('SadBoard reference network build:', digest.hexdigest()[:8])
