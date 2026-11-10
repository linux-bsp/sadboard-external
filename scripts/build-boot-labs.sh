#!/bin/bash
# Buildroot compiled the two RAM-only laboratory ELFs; never change an OTA root.
set -euo pipefail
images=$(realpath "$1")
scripts=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
archive="$scripts/../../bsp-base-external/support/scripts/archive-initramfs.sh"
destination="$images/sadboard"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$destination" "$work/root"
cp "$images/verity-k3-am625-alientek.dtb" "$destination/verity.dtb"
gzip -dc "$images/initramfs.cpio.gz" > "$work/base.cpio"
(cd "$work/root"; "$HOST_DIR/bin/cpio" -id --quiet --no-preserve-owner \
    --no-absolute-filenames -f dev/console dev/null < "$work/base.cpio")
[ -f "$work/root/usr/sbin/fwctl" ]
cp "$BUILD_DIR/sadboard-labs-1/boot-init" "$work/root/init"
chmod 0755 "$work/root/init"
mkdir -p "$work/root/usr/lib"
"$archive" "$work/root" "$destination/init-missing-library.cpio.gz"
cp "$BUILD_DIR/sadboard-labs-1/libsadboard-init.so" "$work/root/usr/lib/"
"$archive" "$work/root" "$destination/init-complete.cpio.gz"
cp "$work/root/init" "$destination/init"
cp "$work/root/usr/lib/libsadboard-init.so" "$destination/libsadboard-init.so"
for path in "$destination"/*; do
    [ -f "$path" ] || continue
    [ "${path##*/}" != manifest.json ] || continue
    "$HOST_DIR/bin/jq" -cn --arg name "${path##*/}" --arg hash "$(sha256sum "$path" | cut -d ' ' -f 1)" '{($name):$hash}'
done | "$HOST_DIR/bin/jq" -s 'add' > "$work/hashes.json"
"$HOST_DIR/bin/jq" -n --slurpfile sha "$work/hashes.json" --slurpfile identity "$images/firmware-identity.json" \
    --arg reference "$(cut -d ' ' -f 3 "$images/netboot/build.conf")" \
    --arg image "$(sha256sum "$images/netboot/Image" | cut -d ' ' -f 1)" \
    --arg base "$(sha256sum "$images/initramfs.cpio.gz" | cut -d ' ' -f 1)" \
    '{format:"SADBOARD-RAM-BOOT-1",sha256:$sha[0],firmware_id:$identity[0].firmware_id,
      reference_build:$reference,image_sha256:$image,base_initramfs_sha256:$base}' > "$destination/manifest.json"
