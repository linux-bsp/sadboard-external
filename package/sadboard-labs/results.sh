# Shared result-directory policy; source this file from the case controllers.
sadboard_results_filesystem()
{
    results_path=$1
    results_type=
    results_match_length=0
    while read -r source mount_path filesystem options rest; do
        mount_path=$(printf '%b' "$mount_path")
        case "$mount_path" in
            /) ;;
            *) case "$results_path/" in "$mount_path/"*) ;; *) continue ;; esac ;;
        esac
        if [ "${#mount_path}" -ge "$results_match_length" ]; then
            results_type=$filesystem
            results_match_length=${#mount_path}
        fi
    done < "${SADBOARD_MOUNTS:-/proc/mounts}"
    [ -n "$results_type" ] || return 1
    printf '%s\n' "$results_type"
}

sadboard_results_root()
{
    if [ -n "${SADBOARD_RESULTS:-}" ]; then
        printf '%s\n' "$SADBOARD_RESULTS"
    else
        results_root_type=$(sadboard_results_filesystem /) || return 1
        case "$results_root_type" in
            nfs|nfs4) printf '%s\n' /srv/sadboard ;;
            *) printf '%s\n' /data/sadboard ;;
        esac
    fi
}

sadboard_results_prepare()
{
    case "$1" in /*) ;; *) return 1 ;; esac
    [ "$1" != / ] || return 1
    mkdir -p "$1" || return 1
    results_directory=$(cd "$1" && pwd -P) || return 1
    results_directory_type=$(sadboard_results_filesystem "$results_directory") || return 1
    case "$results_directory_type" in
        nfs|nfs4) chmod go+rx "$1" ;;
    esac
}

sadboard_results_share()
{
    results_share_type=$(sadboard_results_filesystem "$1") || return 1
    case "$results_share_type" in
        nfs|nfs4)
            # perf.data, core and diagnostic archives can explicitly use 0600.
            # Share regular evidence only; never follow links out of the run.
            find "$1" -type d -exec chmod go+rx '{}' \; &&
                find "$1" -type f -exec chmod go+r '{}' \;
            ;;
    esac
}
