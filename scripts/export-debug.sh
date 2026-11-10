#!/bin/bash
# Native Buildroot post-image hook: preserve matching symbols, sources and hashes.
set -euo pipefail
images=$(realpath "$1")
scripts=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out="$images/debug"
jq="$HOST_DIR/bin/jq"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$out"
cp "$BUILD_DIR/linux-custom/vmlinux" "$out/vmlinux"
cp "$BUILD_DIR/linux-custom/System.map" "$out/System.map"
cp "$BUILD_DIR/linux-custom/.config" "$out/kernel.config"
cp "$BR2_CONFIG" "$out/buildroot.config"
set -- "$BUILD_DIR"/busybox-*/.config
[ "$#" -eq 1 ] && [ -f "$1" ]
cp "$1" "$out/busybox.config"
rm -rf "$out/labs" "$out/source"
mkdir -p "$out/labs" "$out/kernel"
for file in "$TARGET_DIR/usr/libexec/sadboard"/*; do
    [ -f "$file" ] || continue
    magic=$(od -An -tx1 -N4 "$file" | tr -d ' \n')
    [ "$magic" != 7f454c46 ] || cp "$file" "$out/labs/"
done
cp -a "$TARGET_DIR/usr/src/sadboard" "$out/source"
cp "$BUILD_DIR/sadboard-labs-1/kernel/sadboard-faults.ko" "$out/kernel/"
find "$out" -type f ! -name build-info.json -print | LC_ALL=C sort | while IFS= read -r file; do
    "$jq" -cn --arg name "${file#"$out"/}" --arg hash "$(sha256sum "$file" | cut -d ' ' -f 1)" '{($name):$hash}'
done | "$jq" -s 'add' > "$work/hashes.json"
"$jq" -n --slurpfile identity "$images/firmware-identity.json" --slurpfile sha "$work/hashes.json" \
    '{firmware:$identity[0],sha256:$sha[0]}' > "$out/build-info.json"
netboot="$images/netboot"
mkdir -p "$netboot"
gzip -dc "$images/Image.gz" > "$netboot/Image"
cp "$images/k3-am625-alientek.dtb" "$netboot/"
cp "$TARGET_DIR/etc/sadboard/build.conf" "$netboot/build.conf"
for name in netboot/Image netboot/k3-am625-alientek.dtb netboot/build.conf rootfs.tar; do
    "$jq" -cn --arg name "$name" --arg hash "$(sha256sum "$images/$name" | cut -d ' ' -f 1)" '{($name):$hash}'
done | "$jq" -s 'add' > "$work/hashes.json"
"$jq" -n --arg reference "$(cat "$netboot/build.conf")" --slurpfile sha "$work/hashes.json" \
    '{format:"SADBOARD-NETBOOT-1",reference_build:$reference,sha256:$sha[0]}' > "$netboot/manifest.json"
"$scripts/build-boot-labs.sh" "$images"
printf 'SadBoard symbols and network boot artifacts saved: %s\n' "$out"
