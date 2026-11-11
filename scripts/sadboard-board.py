#!/usr/bin/env python3
"""Drive RAM-only boot faults on a real AM625 over its host-connected UART."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import select
import subprocess
import termios
import time
import tty
import threading


class Serial:
    def __init__(self, device, directory):
        self.fd = os.open(device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        tty.setraw(self.fd)
        attributes = termios.tcgetattr(self.fd)
        attributes[4] = attributes[5] = termios.B115200
        termios.tcsetattr(self.fd, termios.TCSANOW, attributes)
        self.log = (directory / 'serial.log').open('ab', buffering=0)
        self.data = bytearray()
        self.stopped = threading.Event()
        self.failure = None
        self.recovery_retries = 0
        self.reader = threading.Thread(target=self.collect, daemon=True)
        self.reader.start()

    def collect(self):
        try:
            while not self.stopped.is_set():
                if select.select([self.fd], [], [], .1)[0]:
                    block = os.read(self.fd, 65536)
                    self.log.write(block)
                    self.data.extend(block)
        except OSError as error:
            self.failure = error
            self.stopped.set()

    def drain(self, seconds=.1):
        # Collection continues while the main thread waits for SSH/NFS health.
        self.stopped.wait(seconds)
        if self.failure: raise self.failure

    def close(self):
        self.stopped.set()
        self.reader.join(1)
        self.log.close()
        os.close(self.fd)

    def send(self, command):
        wire = (command + '\r').encode()
        if len(wire) > 480:
            raise ValueError('Split UART command shorter than the U-Boot input buffer')
        for offset in range(0, len(wire), 16):
            os.write(self.fd, wire[offset:offset + 16])
            termios.tcdrain(self.fd); self.drain(.03)

    def expect(self, text, start, timeout):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if text in self.data[start:]:
                return bytes(self.data[start:])
            self.drain(.1)
        raise TimeoutError(f'UART did not report {text!r}: '+bytes(self.data[-1200:]).decode(errors='replace'))

    def command(self, command, timeout=70):
        start = len(self.data)
        marker = 'SBACK' + str(start)
        self.send(command + '; echo ' + marker)
        output = self.expect(('\r\n' + marker + '\r\n').encode(), start, timeout)
        if any(error in output for error in (b'TFTP error', b'Error:', b'Unknown command', b'not found')):
            raise RuntimeError(output.decode(errors='replace'))
        return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('case', choices=['boot-initlib', 'boot-root', 'watchdog-probe'])
    parser.add_argument('--images', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--serial', default='/dev/ttyACM2')
    parser.add_argument('--device', default='192.168.10.211')
    parser.add_argument('--known-hosts', type=Path, required=True)
    parser.add_argument('--tftp-prefix', default='sadboard')
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_./-]+', args.tftp_prefix): parser.error('Invalid TFTP prefix')
    args.output.mkdir(parents=True, exist_ok=False)
    images = args.images.resolve()
    manifest = json.loads((images / 'sadboard/manifest.json').read_text())
    assert manifest['format'] == 'SADBOARD-RAM-BOOT-1'
    for name, digest in manifest['sha256'].items():
        assert '/' not in name and hashlib.sha256((images / 'sadboard' / name).read_bytes()).hexdigest() == digest
    identity = json.loads((images / 'firmware-identity.json').read_text())
    reference = (images / 'netboot/build.conf').read_text().split()[2]
    assert manifest['firmware_id'] == identity['firmware_id']
    assert manifest['reference_build'] == reference
    assert hashlib.sha256((images / 'netboot/Image').read_bytes()).hexdigest() == manifest['image_sha256']
    ssh = ['ssh', '-o', 'UserKnownHostsFile=' + str(args.known_hosts.resolve()),
           '-o', 'StrictHostKeyChecking=yes', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=2',
           '-o', 'ServerAliveInterval=2', '-o', 'ServerAliveCountMax=2', 'root@' + args.device]

    def call(command, timeout=5):
        return subprocess.run(ssh + [command], capture_output=True, text=True, timeout=timeout)

    def healthy(timeout=160, reference_mode=True):
        until = time.monotonic() + timeout
        retries = 0
        while time.monotonic() < until:
            if bytes(serial.data[-256:]).rstrip().endswith(b'=>') and retries < 2:
                # A PHY/ARP/TFTP retry may return to U-Boot before any kernel.
                # Retry the unchanged normal boot profile, never save arguments.
                serial.send('run fw_boot')
                serial.recovery_retries += 1
                retries += 1
                serial.drain(1)
            serial.drain(.2)
            try:
                r = call('cat /proc/sys/kernel/random/boot_id; cat /run/fwctl-boot-confirmed; '
                         '/usr/libexec/bsp/boot-context; cat /etc/bsp/netboot.conf; systemctl --failed --no-pager')
            except subprocess.TimeoutExpired:
                continue
            lines = r.stdout.splitlines()
            confirmed = len(lines) > 1 and (lines[1] == 'netboot' if reference_mode else lines[1] in ('0', '1'))
            if r.returncode == 0 and confirmed and '0 loaded units listed.' in r.stdout:
                match = re.search(r'^[a-f0-9-]{36}$', r.stdout, re.M)
                if match and (('BSPNET1 1 ' + reference) in r.stdout if reference_mode else ('FWOTA2' in r.stdout and identity['firmware_id'] in r.stdout and reference in r.stdout)):
                    return dict(boot_id=match[0], context=r.stdout)
            serial.drain(1)
        raise TimeoutError('Normal matching firmware did not regain healthy SSH access')

    def reboot_intercept():
        start = len(serial.data)
        subprocess.Popen(ssh + ['logctl boot-event operator; reboot'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        serial.expect(b'Hit any key to stop autoboot:', start, 95)
        serial.send(' '); serial.drain(.4)
        text = serial.command('printenv tftp_kernel nfs_root')
        assert b'tftp_kernel=1' in text and b'nfs_root=1' in text, 'Keep saved normal recovery TFTP+NFS enabled'

    serial = Serial(args.serial, args.output)
    before = healthy()
    # Capture the complete transcript; no saveenv, flash write or bootloader update.
    proof = dict(case=args.case, firmware_id=identity['firmware_id'], before=before, trials=[])
    in_kernel = True
    try:
        if args.case == 'watchdog-probe':
            old = before['boot_id']
            subprocess.Popen(ssh + ['logctl boot-event operator; reboot'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            until = time.monotonic() + 110
            while time.monotonic() < until:
                serial.drain(.2)
                try: state = call('cat /proc/sys/kernel/random/boot_id; logctl status')
                except subprocess.TimeoutExpired: continue
                match = re.search(r'^[0-9a-f-]{36}$', state.stdout, re.M)
                if state.returncode == 0 and match and match[0] != old and 'startup-state=collecting' in state.stdout:
                    failed = match[0]
                    assert call('systemctl kill --kill-whom=all --signal=STOP fw-confirm.service').returncode == 0
                    break
            else: raise TimeoutError('Fresh unsealed startup unavailable')
            offset = len(serial.data); started = time.monotonic()
            try:
                result = call('sadboard run watchdog-probe', 8)
                text = result.stdout + result.stderr
            except subprocess.TimeoutExpired as error:
                text = ''.join(value.decode(errors='replace') if isinstance(value, bytes) else value or '' for value in (error.stdout, error.stderr))
            (args.output / 'probe-command.log').write_text(text)
            run = re.search(r'results=(/srv/sadboard/[^\s]+)', text)
            assert run, text
            serial.expect(b'U-Boot SPL', offset, 135)
            elapsed = time.monotonic() - started
            assert 30 < elapsed < 135, elapsed
            serial.expect(b'Reset reason: ESM', offset, 15)
            proof['watchdog_observation'] = dict(reset_seconds=elapsed, reset_reason='ESM', failed_boot=failed)
            recovered = healthy()
            assert recovered['boot_id'] not in (old, failed)
            check = call('sadboard check ' + run[1], 50)
            (args.output / 'probe-check.log').write_text(check.stdout + check.stderr)
            assert check.returncode == 0
            result = call('mkdir -p /data/sadboard; echo ' + reference + ' > /data/sadboard/watchdog-proven; sync')
            assert result.returncode == 0
            proof['trials'].append(dict(variant='hardware-watchdog', failed_boot=failed, recovered=recovered,
                                        reset_seconds=elapsed, run=run[1], proven_firmware_id=reference))
        else:
            for variant in (['bug', 'guard', 'fixed'] if args.case == 'boot-initlib' else ['bug', 'fixed']):
                reboot_intercept()
                in_kernel = False
                serial.command('run fw_start_watchdog && fwctl select && run fw_set_root')
                # Explicit runtime args bypass neither flash verification nor slot metadata writes;
                # only the kernel/initramfs under test lives in RAM.
                serial.command('setenv bootargs console=ttyS2,115200n8 earlycon=ns16550a,mmio32,0x02800000')
                serial.command('setenv bootargs ${bootargs} blkdevparts=mmcblk1:${fw_parts_a},${fw_parts_b},${fw_parts_os}')
                if variant == 'fixed': serial.command('setenv bootargs ${bootargs} rootwait')
                root = '/dev/mmcblk1p99' if args.case == 'boot-root' and variant == 'bug' else '/dev/mmcblk1p${fw_rootpart}'
                serial.command('setenv bootargs ${bootargs} root=' + root + ' rootfstype=squashfs ro panic=3 fw.deployment=${fw_deployment}')
                serial.command('setenv bootargs ${bootargs} sysrq_always_enabled sadboard.case=' + args.case + '-' + variant)
                if variant == 'guard': serial.command('setenv bootargs ${bootargs} SADBOARD_RESCUE_TEST=1')
                initrd = '-'
                if args.case == 'boot-initlib':
                    filename = 'init-missing-library.cpio.gz' if variant == 'bug' else 'init-complete.cpio.gz'
                    output = serial.command('tftpboot 0x88080000 ' + args.tftp_prefix + '/' + filename + ' && setenv sb_ramdisk_size ${filesize}')
                    assert b'Bytes transferred' in output
                    initrd = '0x88080000:${sb_ramdisk_size}'
                for address, filename in [('0x82000000', str(Path(args.tftp_prefix).parent / 'Image')), ('0x88000000', args.tftp_prefix + '/verity.dtb')]:
                    output = serial.command('tftpboot ' + address + ' ' + filename)
                    assert b'Bytes transferred' in output
                start = len(serial.data)
                serial.send('booti 0x82000000 ' + initrd + ' 0x88000000')
                in_kernel = True
                if variant == 'bug':
                    needle = b'Attempted to kill init!' if args.case == 'boot-initlib' else b'VFS: Unable to mount root fs'
                    serial.expect(needle, start, 50)
                    recovered = healthy()
                elif variant == 'guard':
                    needle = b'SadBoard RAM rescue guard: timeout; rebooting'
                    serial.expect(needle, start, 50)
                    recovered = healthy()
                else:
                    needle = b'SadBoard PID1 shared-library dependency restored' if args.case == 'boot-initlib' else b'VFS: Mounted root (squashfs filesystem)'
                    serial.expect(needle, start, 50)
                    recovered = healthy(timeout=220, reference_mode=False)
                    # Leave the normal, matching TFTP+NFS profile running after the SD reference.
                    subprocess.Popen(ssh + ['logctl boot-event operator; reboot'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                    serial.drain(4)
                    recovered = healthy()
                transcript = bytes(serial.data[start:])
                (args.output / (variant + '-serial.log')).write_bytes(transcript)
                proof['trials'].append(dict(variant=variant, expected_marker=needle.decode(), recovered=recovered,
                                            serial_sha256=hashlib.sha256(transcript).hexdigest()))
        proof['result'] = 'success'
    except Exception as error:
        proof['error'] = str(error)
        # Interrupts remain enabled in both RAM exercises. Request a reboot over SSH when available; the RAM
        # rescue child and hardware WDT cover a stalled experimental kernel.
        if in_kernel:
            try: call('systemctl kill --kill-whom=all --signal=CONT fw-confirm.service; reboot')
            except subprocess.TimeoutExpired: pass
            # USB UART BREAK is not assumed. The RAM PID1 rescue child and
            # independently proven hardware WDT remain the fallback.
        else:
            serial.send('reset')
        serial.drain(4)
        try: proof['fallback'] = healthy()
        except Exception as fallback: proof['fallback_error'] = str(fallback)
        raise
    finally:
        serial.drain(.2)
        proof['normal_boot_retries'] = serial.recovery_retries
        (args.output / 'result.json').write_text(json.dumps(proof, indent=2) + '\n')
        serial.close()
    print(json.dumps(proof, indent=2))


if __name__ == '__main__':
    main()
