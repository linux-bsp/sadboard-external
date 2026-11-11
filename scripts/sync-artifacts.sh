#!/bin/bash
# Host-side development deployment. No board login or boot-mode changes.
set -euo pipefail
script=$(realpath "${BASH_SOURCE[0]}")
scripts=$(dirname "$script")
workspace=$(dirname "$(dirname "$scripts")")
deploy="$workspace/bsp-base-external/support/scripts/deploy-netboot.sh"
original=("$@")
images=
nfs_root=/srv/nfs/sadboard-am625
tftp_root=/srv/tftp/am625
client=192.168.10.211
password_file=${SADBOARD_SUDO_PASSWORD_FILE:-${XDG_CONFIG_HOME:-$HOME/.config}/.sudo.guo}
check_only=0
usage()
{
    cat <<'EOF'
Usage: sync-artifacts.sh IMAGES [OPTIONS]
  --nfs-root DIR       NFS root (default: /srv/nfs/sadboard-am625)
  --tftp-root DIR      TFTP root (default: /srv/tftp/am625)
  --client IP         NFS client (default: 192.168.10.211)
  --password-file FILE  sudo password file (default: ~/.config/.sudo.guo)
  --check             Validate only; no sudo or destination changes

Run while the board uses its local rootfs or is powered off. Existing NFS
files are synchronized with stale files removed; /srv/sadboard results and
/etc/machine-id are preserved. Matching TFTP images and optional RAM boot
laboratories are copied, and the NFS export is refreshed.
EOF
}
while (($#)); do
    case "$1" in
        -h|--help) usage; exit 0;;
        --check) check_only=1; shift; continue;;
        --nfs-root|--tftp-root|--client|--password-file)
            (($#>=2)) || { usage >&2; exit 2; }
            case "$1" in
                --nfs-root) nfs_root=$2;; --tftp-root) tftp_root=$2;;
                --client) client=$2;; --password-file) password_file=$2;;
            esac
            shift 2; continue;;
        -*) usage >&2; exit 2;;
        *) [[ -z $images ]] || { usage >&2; exit 2; }; images=$1;;
    esac
    shift
done
[[ -n $images ]] || { usage >&2; exit 2; }
images=$(realpath "$images")
arguments=("$images" --nfs-root "$nfs_root" --tftp-root "$tftp_root" --client "$client"
           --sync --preserve srv/sadboard --preserve etc/machine-id)
"$deploy" "${arguments[@]}" --check
laboratories=0
if [[ -f $images/sadboard/manifest.json ]]; then
    "$scripts/deploy-boot-labs.sh" "$images" "$tftp_root" --check
    laboratories=1
fi
((check_only==0)) || exit 0

if ((EUID!=0)); then
    # Password bytes go only to sudo's standard input, never shell variables,
    # arguments or logs. The elevated process receives an empty standard input.
    if ! sudo -n -v 2>/dev/null; then
        [[ -f $password_file && ! -L $password_file && -r $password_file && -s $password_file ]] || {
            echo "Create a private sudo password file: $password_file" >&2; exit 1;
        }
        [[ $(stat -c %u "$password_file") == "$(id -u)" ]] || {
            echo 'The sudo password file must belong to the current user' >&2; exit 1;
        }
        mode=$(stat -c %a "$password_file")
        [[ $mode == 600 || $mode == 400 ]] || {
            echo "Set password file permissions with chmod 600: $password_file" >&2; exit 1;
        }
        sudo -S -p '' -v < "$password_file" || {
            echo 'sudo authentication failed; no deployment was performed' >&2; exit 1;
        }
    fi
    # Keep this parent process: sudo's timestamp may be scoped to its parent PID.
    sudo -n -- bash "$script" "${original[@]}" < /dev/null
    exit $?
fi

"$deploy" "${arguments[@]}"
if ((laboratories)); then
    "$scripts/deploy-boot-labs.sh" "$images" "$tftp_root"
fi
echo "Synchronized: NFS=$nfs_root TFTP=$tftp_root client=$client"
