#!/bin/bash
# Optional SadBoard RAM-only fault payloads; TFTP/NFS deployment belongs to BSP.
set -euo pipefail
images=$(realpath "$1")
shift
tftp=/srv/tftp/am625
check_only=0
destination_set=0
while (($#)); do
    case "$1" in
        --check) check_only=1;;
        -*) echo "Unknown laboratory deployment option: $1" >&2; exit 2;;
        *) ((destination_set==0)) || { echo 'Specify one TFTP directory' >&2; exit 2; }
           tftp=$1; destination_set=1;;
    esac
    shift
done
laboratory="$images/sadboard"
jq="$images/../host/bin/jq"
[[ -x $jq ]] || jq=$(command -v jq)
# host-jq has no regular-expression support in Buildroot.
"$jq" -e '.format=="SADBOARD-RAM-BOOT-1" and (.sha256|type=="object") and
    (.sha256|length>0) and (.sha256|all(.[];type=="string"))' \
    "$laboratory/manifest.json" >/dev/null
while IFS=$'\t' read -r name expected; do
    [[ $name =~ ^[A-Za-z0-9_-][A-Za-z0-9_.-]*$ ]] || { echo 'Invalid laboratory filename' >&2; exit 1; }
    [[ $expected =~ ^[0-9a-f]{64}$ ]] || { echo "Invalid laboratory checksum: $name" >&2; exit 1; }
    actual=$(sha256sum "$laboratory/$name" | cut -d ' ' -f 1)
    [[ $actual == "$expected" ]] || { echo "Laboratory checksum mismatch: $name" >&2; exit 1; }
done < <("$jq" -r '.sha256|to_entries[]|[.key,.value]|@tsv' "$laboratory/manifest.json")
if ((check_only)); then
    echo 'Verified SadBoard RAM boot laboratories'
    exit 0
fi
mkdir -p "$tftp/sadboard"
cp -a "$laboratory/." "$tftp/sadboard/"
find "$tftp/sadboard" -maxdepth 1 -type f -exec chmod 0644 '{}' +
