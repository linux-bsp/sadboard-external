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
    labs = [f'labs/{name}-{mode}' for name in ('crash', 'cpu', 'memory', 'lock')
            for mode in ('bug', 'fixed')]
    labs.append('labs/check-result')
    required = ['vmlinux', 'System.map', 'kernel.config', 'buildroot.config', 'busybox.config',
                'source/src/crash.c', 'source/src/cpu.c', 'source/src/memory.c',
                'source/src/lock.c', 'source/src/lab.h', *labs]
    if not all(name in metadata['sha256'] for name in required):
        raise ValueError('debugging manifest is incomplete')
    for name in ('vmlinux', *labs):
        sections = subprocess.check_output(['readelf', '--sections', '--wide', str(root / name)], text=True)
        if '.debug_info' not in sections:
            raise ValueError(f'debug symbols were stripped: {name}')
    config = (root / 'kernel.config').read_text().splitlines()
    for symbol in ('CONFIG_DEBUG_INFO_DWARF4', 'CONFIG_FUNCTION_TRACER', 'CONFIG_MEMCG',
                   'CONFIG_COREDUMP', 'CONFIG_CORE_DUMP_DEFAULT_ELF_HEADERS',
                   'CONFIG_NET_NS', 'CONFIG_VETH', 'CONFIG_NET_SCH_NETEM'):
        if symbol + '=y' not in config:
            raise ValueError(f'missing kernel capability: {symbol}')
    if 'CONFIG_FLOAT_DURATION=y' not in (root / 'busybox.config').read_text().splitlines():
        raise ValueError('fractional BusyBox durations are disabled')
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
