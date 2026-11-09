#!/usr/bin/env python3
"""Check exported debugging files before deploying or analyzing a firmware."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def verify(images):
    root = images.resolve() / 'debug'
    metadata = json.loads((root / 'build-info.json').read_text())
    identity = json.loads((images / 'firmware-identity.json').read_text())
    if metadata['firmware'] != identity:
        raise ValueError('debugging files belong to a different firmware')
    for name, expected in metadata['sha256'].items():
        path = (root / name).resolve()
        if not path.is_relative_to(root) or not path.is_file():
            raise ValueError(f'invalid debugging file: {name}')
        digest = hashlib.sha256()
        with path.open('rb') as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b''):
                digest.update(block)
        if digest.hexdigest() != expected:
            raise ValueError(f'debugging file changed: {name}')
    required = ('vmlinux', 'System.map', 'kernel.config', 'buildroot.config',
                'labs/crash-bug', 'labs/crash-fixed', 'source/src/crash.c')
    if not all(name in metadata['sha256'] for name in required):
        raise ValueError('debugging manifest is incomplete')
    for name in ('vmlinux', 'labs/crash-bug', 'labs/crash-fixed'):
        sections = subprocess.check_output(['readelf', '--sections', '--wide', str(root / name)], text=True)
        if '.debug_info' not in sections:
            raise ValueError(f'debug symbols were stripped: {name}')
    config = (root / 'kernel.config').read_text().splitlines()
    for symbol in ('CONFIG_DEBUG_INFO_DWARF4', 'CONFIG_FUNCTION_TRACER', 'CONFIG_MEMCG',
                   'CONFIG_COREDUMP', 'CONFIG_CORE_DUMP_DEFAULT_ELF_HEADERS'):
        if symbol + '=y' not in config:
            raise ValueError(f'missing kernel capability: {symbol}')
    return identity


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('images', type=Path)
    args = parser.parse_args()
    try:
        identity = verify(args.images)
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'debug validation: {error}\n')
    print('Verified debugging files:', identity['tag'], identity['firmware_id'])
