#!/usr/bin/env python3
"""Save matching symbols and provenance after the BSP firmware image is built."""
import hashlib
import gzip
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
busybox = list(build.glob('busybox-*/.config'))
if len(busybox) != 1:
    raise RuntimeError('Expected one BusyBox configuration in this build tree')
shutil.copyfile(busybox[0], out / 'busybox.config')
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
(out / 'kernel').mkdir(exist_ok=True)
shutil.copyfile(build / 'sadboard-labs-1/kernel/sadboard-faults.ko', out / 'kernel/sadboard-faults.ko')
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

# booti expects an uncompressed arm64 Image; keep these separate from OTA FITs.
netboot = images / 'netboot'
netboot.mkdir(exist_ok=True)
with gzip.open(images / 'Image.gz', 'rb') as source, (netboot / 'Image').open('wb') as dest:
    shutil.copyfileobj(source, dest)
shutil.copyfile(images / 'k3-am625-alientek.dtb', netboot / 'k3-am625-alientek.dtb')
shutil.copyfile(target / 'etc/sadboard/build.conf', netboot / 'build.conf')
artifact_paths = ['netboot/Image', 'netboot/k3-am625-alientek.dtb', 'netboot/build.conf', 'rootfs.tar']
checksums = {}
for name in artifact_paths:
    digest = hashlib.sha256()
    with (images / name).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    checksums[name] = digest.hexdigest()
(netboot / 'manifest.json').write_text(json.dumps(dict(
    format='SADBOARD-NETBOOT-1', reference_build=(netboot / 'build.conf').read_text().strip(),
    sha256=checksums), indent=2) + '\n')
print('SadBoard TFTP files and NFS rootfs manifest saved:', netboot)

import importlib.util
spec = importlib.util.spec_from_file_location('boot_labs', Path(__file__).with_name('build-boot-labs.py'))
module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
module.build(images, Path(os.environ['HOST_DIR']))
