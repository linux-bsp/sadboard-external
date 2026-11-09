#!/usr/bin/env python3
"""Save matching symbols and provenance after the BSP firmware image is built."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys

images = Path(sys.argv[1]).resolve()
build = Path(os.environ['BUILD_DIR']).resolve()
target = Path(os.environ['TARGET_DIR']).resolve()
out = images / 'debug'
out.mkdir(exist_ok=True)
for source, name in [(build / 'linux-custom/vmlinux', 'vmlinux'),
                     (build / 'linux-custom/System.map', 'System.map'),
                     (build / 'linux-custom/.config', 'kernel.config'),
                     (build.parent / '.config', 'buildroot.config')]:
    shutil.copyfile(source, out / name)
shutil.rmtree(out / 'labs', ignore_errors=True)
(out / 'labs').mkdir()
for path in sorted((target / 'usr/libexec/sadboard').glob('*')):
    if not path.is_file():
        continue
    with path.open('rb') as stream:
        is_elf = stream.read(4) == b'\x7fELF'
    if is_elf:
        shutil.copyfile(path, out / 'labs' / path.name)
shutil.rmtree(out / 'source', ignore_errors=True)
shutil.copytree(target / 'usr/src/sadboard', out / 'source', dirs_exist_ok=True)
identity = json.loads((images / 'firmware-identity.json').read_text())
files = {}
for path in sorted(out.rglob('*')):
    if path.is_file() and path.name != 'build-info.json':
        digest = hashlib.sha256()
        with path.open('rb') as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b''):
                digest.update(block)
        files[str(path.relative_to(out))] = digest.hexdigest()
(out / 'build-info.json').write_text(json.dumps(dict(firmware=identity, sha256=files), indent=2) + '\n')
print('SadBoard symbols saved:', out)
