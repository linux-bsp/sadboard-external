#!/bin/bash
# Development-host deployment only; never write the board's local partitions.
set -euo pipefail
images=$(realpath "$1"); shift
root=/srv/nfs/sadboard-am625
tftp=/srv/tftp/am625
client=192.168.10.211
while (($#)); do
    case "$1" in
        --nfs-root) root=$2;; --tftp-root) tftp=$2;; --client) client=$2;;
        *) echo "Unknown deployment option: $1" >&2; exit 2;;
    esac
    shift 2
done
((EUID==0)) || { echo 'Run as root to preserve numeric rootfs ownership' >&2; exit 1; }
[[ $client =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]
IFS=. read -r -a octets <<< "$client"
for octet in "${octets[@]}"; do ((${#octet}<=3 && 10#$octet<=255)); done
for path in "$root" "$tftp"; do
    [[ $path == /* && $path != *[[:space:],]* ]] || { echo 'Use absolute export paths without whitespace or commas' >&2; exit 1; }
done
[[ ! -L $root && ( ! -e $root || ( -d $root && -z $(find "$root" -mindepth 1 -maxdepth 1 -print -quit) ) ) ]] || {
    echo 'NFS destination must be empty; stop the board and archive the previous root first' >&2; exit 1;
}
jq="$images/../host/bin/jq"
[[ -x $jq ]] || jq=$(command -v jq)
manifest="$images/netboot/manifest.json"
"$jq" -e '.format=="SADBOARD-NETBOOT-1" and
    ((.sha256|keys|sort)==(["netboot/Image","netboot/k3-am625-alientek.dtb","netboot/build.conf","rootfs.tar"]|sort))' "$manifest" >/dev/null
while IFS=$'\t' read -r name expected; do
    actual=$(sha256sum "$images/$name" | cut -d ' ' -f 1)
    [[ $actual == "$expected" ]] || { echo "Artifact checksum mismatch: $name" >&2; exit 1; }
done < <("$jq" -r '.sha256|to_entries[]|[.key,.value]|@tsv' "$manifest")
[[ -x /usr/sbin/exportfs ]] || { echo 'Install nfs-kernel-server first' >&2; exit 1; }
mkdir -p "$root" "$tftp"
tar --numeric-owner -xf "$images/rootfs.tar" -C "$root"
[[ $(cat "$root/etc/sadboard/build.conf") == "$("$jq" -r .reference_build "$manifest")" ]]
for name in Image k3-am625-alientek.dtb; do
    install -m 0644 "$images/netboot/$name" "$tftp/$name.new"
    mv "$tftp/$name.new" "$tftp/$name"
done
laboratory="$images/sadboard"
if [[ -d $laboratory ]]; then
    while IFS=$'\t' read -r name expected; do
        [[ $name != */* && $name != . && $name != .. ]]
        actual=$(sha256sum "$laboratory/$name" | cut -d ' ' -f 1)
        [[ $actual == "$expected" ]]
    done < <("$jq" -r '.sha256|to_entries[]|[.key,.value]|@tsv' "$laboratory/manifest.json")
    mkdir -p "$tftp/sadboard"
    cp -a "$laboratory/." "$tftp/sadboard/"
    find "$tftp/sadboard" -maxdepth 1 -type f -exec chmod 0644 '{}' +
fi
mkdir -p /etc/exports.d
printf '%s %s(rw,sync,no_root_squash,no_subtree_check)\n' "$root" "$client" > /etc/exports.d/sadboard.exports
systemctl enable --now nfs-server.service
/usr/sbin/exportfs -ra
printf 'TFTP: %s\nNFS: %s; allowed client: %s\n' "$tftp" "$root" "$client"
