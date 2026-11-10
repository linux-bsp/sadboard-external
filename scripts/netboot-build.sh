#!/bin/bash
# Reference identity for mutable NFS debugging roots, distinct from the OTA ID.
set -euo pipefail
scripts=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
workspace=$(dirname "$(dirname "$scripts")")
destination="$TARGET_DIR/etc/sadboard/build.conf"
mkdir -p "$(dirname "$destination")"
digest=$({
    printf 'SadBoard debug build v2\000'
    for name in Image.gz k3-am625-alientek.dtb; do
        printf '%s\000' "$name"
        cat "$BINARIES_DIR/$name"
    done
    tar -C "$TARGET_DIR" --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner \
        --exclude=./etc/sadboard/build.conf -cf - .
} | sha256sum | cut -d ' ' -f 1)
IFS=. read -r major minor patch < "$workspace/edk/VERSION"
version=$((major * 65536 + minor * 256 + patch))
printf 'SBNET1 %s %s\n' "$version" "$digest" > "$destination"
chmod 0644 "$destination"
printf 'SadBoard reference network build: %.8s\n' "$digest"
