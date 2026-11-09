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
for name in ('crash-bug', 'crash-fixed'):
    (out / 'labs').mkdir(exist_ok=True)
    shutil.copyfile(target / 'usr/libexec/sadboard' / name, out / 'labs' / name)
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
