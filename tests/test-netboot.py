#!/usr/bin/env python3
"""Exercise boot-mode selection, saved environment migration and build identity."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

EXTERNAL = Path(__file__).resolve().parents[1]
WORKSPACE = EXTERNAL.parent
UBOOT = WORKSPACE / 'uboot/u-boot-2026.04'


class Netboot(unittest.TestCase):
    def test_four_compiled_environment_combinations(self):
        for tftp, nfs, profile in ((0, 0, 'mmc'), (1, 0, 'tftp-mmc'), (0, 1, 'mmc-nfs'), (1, 1, 'tftp-nfs')):
            cmd = ['cc', '-E', '-P', '-x', 'c', '-I', str(UBOOT / 'include')]
            for enabled, symbol in ((tftp, 'TFTP_KERNEL'), (nfs, 'NFS_ROOT')):
                if enabled:
                    cmd.append('-DCONFIG_AM62X_BOOT_' + symbol)
            text = subprocess.check_output(cmd + [str(UBOOT / 'board/ti/am62x/am62x_raw_ab.env')], text=True)
            values = dict(line.split('=', 1) for line in text.splitlines() if '=' in line and not line[0].isspace())
            self.assertEqual(values['fw_boot_profile'], profile)
            self.assertEqual(values['tftp_kernel'], str(tftp))
            self.assertEqual(values['nfs_root'], str(nfs))

    def test_context_distinguishes_network_from_saved_sd_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'cmdline').write_text('root=/dev/nfs sadboard.netboot=1 rw\n')
            record = 'SBNET1 1 ' + '1' * 64 + '\n'
            (root / 'build.conf').write_text(record)
            (root / 'fwctl').write_text('#!/bin/sh\necho FWOTA2-local-slot\n')
            (root / 'fwctl').chmod(0o755)
            env = dict(os.environ, SADBOARD_CMDLINE=str(root / 'cmdline'),
                       SADBOARD_BUILD_INFO=str(root / 'build.conf'), PATH=directory + ':' + os.environ['PATH'])
            cmd = ['sh', str(EXTERNAL / 'rootfs-overlay/usr/libexec/sadboard/boot-context')]
            self.assertEqual(subprocess.check_output(cmd, env=env, text=True), record)
            (root / 'cmdline').write_text('root=/dev/mmcblk1p13 fw.deployment=0\n')
            self.assertEqual(subprocess.check_output(cmd, env=env, text=True), 'FWOTA2-local-slot\n')

    def test_build_identity_is_repeatable_and_tracks_kernel_and_root_changes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target, images = root / 'target', root / 'images'
            target.mkdir(); images.mkdir()
            (target / 'init').write_text('example root')
            (target / 'absolute-link').symlink_to('/usr/bin/example')
            (images / 'Image.gz').write_bytes(b'kernel')
            (images / 'k3-am625-alientek.dtb').write_bytes(b'dtb')
            env = dict(os.environ, TARGET_DIR=str(target), BINARIES_DIR=str(images))
            def build():
                subprocess.run(['bash', str(EXTERNAL / 'scripts/netboot-build.sh')], env=env, check=True, capture_output=True)
                return (target / 'etc/sadboard/build.conf').read_text()
            original = build()
            self.assertEqual(original, build())
            (target / 'init').write_text('changed root')
            self.assertNotEqual(original, build())
            before_kernel = build()
            (images / 'Image.gz').write_bytes(b'new kernel')
            self.assertNotEqual(before_kernel, build())

    def test_saved_profile_migration_preserves_provisioning_and_manual_switches(self):
        text = (UBOOT / 'board/ti/am62x/evm.c').read_text()
        function = text[text.index('static void migrate_fw_environment(void)'):]
        function = function[:function.index('\n#endif')]
        # Compile and execute the actual board migration with a memory env backend.
        harness = r'''
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
struct entry { char key[64], value[1024]; } entries[100];
int count, saves;
const char *profile="tftp-nfs", *tftp="1", *nfs="1";
const char *env_get(const char *key) {
    for (int i=0;i<count;i++) if (!strcmp(entries[i].key,key)) return entries[i].value;
    return NULL;
}
int env_set(const char *key,const char *value) {
    int i; for (i=0;i<count;i++) if (!strcmp(entries[i].key,key)) break;
    if (i==count) count++;
    snprintf(entries[i].key,64,"%s",key); snprintf(entries[i].value,1024,"%s",value);
    return 0;
}
int env_get_default_into(const char *key,char *value,int size) {
    const char *def=!strcmp(key,"fw_env_version")?"9":!strcmp(key,"fw_boot_profile")?profile:
        !strcmp(key,"tftp_kernel")?tftp:!strcmp(key,"nfs_root")?nfs:"compiled script";
    snprintf(value,size,"%s",def); return strlen(def);
}
int env_set_default_vars(int length,char *const *names,int flags) {
    for(int i=0;i<length;i++) { char value[1024]; env_get_default_into(names[i],value,1024); env_set(names[i],value); }
    return 1;
}
int env_save(void) { saves++; return 0; }
'''
        main = r'''
int main(void) {
    env_set("fw_env_version","8"); env_set("tftp_kernel","0"); env_set("nfs_root","0");
    env_set("hwid","1010001000110000"); env_set("ethaddr","1c:63:49:22:c4:f7");
    env_set("serverip","192.0.2.1"); env_set("nfsroot","/custom/root");
    migrate_fw_environment(); assert(saves==1);
    assert(!strcmp(env_get("tftp_kernel"),"1") && !strcmp(env_get("nfs_root"),"1"));
    assert(!strcmp(env_get("hwid"),"1010001000110000"));
    assert(!strcmp(env_get("ethaddr"),"1c:63:49:22:c4:f7"));
    assert(!strcmp(env_get("serverip"),"192.0.2.1"));
    assert(!strcmp(env_get("nfsroot"),"/custom/root"));
    env_set("tftp_kernel","0"); migrate_fw_environment(); assert(saves==1);
    assert(!strcmp(env_get("tftp_kernel"),"0"));
    profile="mmc-nfs"; tftp="0"; migrate_fw_environment(); assert(saves==2);
    assert(!strcmp(env_get("tftp_kernel"),"0") && !strcmp(env_get("nfs_root"),"1"));
    profile="tftp-mmc"; tftp="1"; nfs="0"; migrate_fw_environment(); assert(saves==3);
    assert(!strcmp(env_get("tftp_kernel"),"1") && !strcmp(env_get("nfs_root"),"0"));
    profile="mmc"; tftp="0"; migrate_fw_environment(); assert(saves==4);
    assert(!strcmp(env_get("tftp_kernel"),"0") && !strcmp(env_get("nfs_root"),"0"));
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'migration.c'
            source.write_text(harness + function + main)
            executable = Path(directory) / 'migration'
            subprocess.run(['cc', '-o', str(executable), str(source)], check=True)
            subprocess.run([str(executable)], check=True, capture_output=True)


if __name__ == '__main__':
    unittest.main()
