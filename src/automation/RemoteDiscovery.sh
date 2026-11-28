#!/bin/sh
# Executed through SSH stdin; never print environment, paths, logs or credentials.
set -eu
mode=${1:-descriptors}
instance=${2:-}
explicit=${3:-}
app_id="${4:-com.sachk.spool}"
uid=$(id -u)
# Root may inspect the app sandbox's capability without changing its ownership.
# Discovery binds ownership to the matching process; explicit root selection
# accepts one private directory/file owner, never mismatched owners.
expected_owner=$uid
[ "$uid" != 0 ] || expected_owner=''
descriptor_owner=''
private_file() {
    [ -f "$1" ] && [ ! -L "$1" ] || return 1
    info=$(stat -c '%u %a %s' "$1" 2>/dev/null) || return 1
    set -- $info
    [ "$1" = "$descriptor_owner" ] && [ "$2" = 600 ] && [ "$3" -le 4096 ]
}
private_directory() {
    [ -d "$1" ] && [ ! -L "$1" ] || return 1
    info=$(stat -c '%u %a' "$1" 2>/dev/null) || return 1
    set -- $info
    [ "$2" = 700 ] || return 1
    [ -z "$expected_owner" ] || [ "$1" = "$expected_owner" ] || return 1
    descriptor_owner=$1
}
emit_descriptor() {
    private_directory "${1%/*}" && private_file "$1" || return 0
    cat "$1"
    printf '\n'
}
if [ "$mode" = descriptors ] && [ -n "$explicit" ]; then
    case "$explicit" in /*) emit_descriptor "$explicit" ;; *) exit 2 ;; esac
    exit
fi
seen=''
first_pid=''
for process in /proc/[0-9]*; do
    executable=$(readlink "$process/exe" 2>/dev/null) || continue
    case "$executable" in
        */usr/palm/applications/"$app_id"/bin/spool | */usr/palm/applications/"$app_id"/bin/spool.real) ;;
        *) continue ;;
    esac
    [ -r "$process/environ" ] || continue
    process_owner=$(stat -c %u "$process" 2>/dev/null) || continue
    [ "$uid" = 0 ] || [ "$process_owner" = "$uid" ] || continue
    expected_owner=$process_owner
    [ -n "$first_pid" ] || first_pid=${process##*/}
    value() {
        tr '\000' '\n' < "$process/environ" | awk -v key="$1" 'index($0,key "=")==1 {print substr($0,length(key)+2); exit}'
    }
    home=$(value HOME)
    data=$(value XDG_DATA_HOME)
    [ -n "$data" ] || data="$home/.local/share"
    case "$data" in /*) ;; *) continue ;; esac
    root="$data/spool/Spool"
    if [ "$mode" = diagnostics ]; then
        diagnostics=$(value SPOOL_DIAGNOSTICS_DIR)
        [ -n "$diagnostics" ] || diagnostics="$root/diagnostics"
        case "$diagnostics" in /*) ;; *) continue ;; esac
        metadata="$diagnostics/current-instance.json"
        if [ -f "$metadata" ] && [ ! -L "$metadata" ]; then
            size=$(stat -c %s "$metadata" 2>/dev/null) || continue
            [ "$size" -le 65536 ] || continue
            cat "$metadata"
            exit
        fi
    elif [ "$mode" = descriptors ]; then
        selected=$(value SPOOL_DATA_HOME)
        if [ -n "$selected" ]; then
            registry="$selected/runtime/spool-control"
        else
            registry="$root/runtime/spool-control"
        fi
        case "$registry" in /*) ;; *) continue ;; esac
        case "
$seen
" in *"
$registry
"*) continue ;; esac
        seen="$seen
$registry"
        if [ -n "$instance" ]; then
            emit_descriptor "$registry/$instance.json"
        else
            count=0
            for descriptor in "$registry"/*.json; do
                count=$((count + 1))
                [ "$count" -le 64 ] || exit 3
                emit_descriptor "$descriptor"
            done
        fi
    else
        exit 2
    fi
done
if [ "$mode" = diagnostics ]; then
    if [ -n "$first_pid" ]; then
        printf '{"pid":%s,"running":true,"metadataAvailable":false}\n' "$first_pid"
    else
        printf '{"running":false,"metadataAvailable":false}\n'
    fi
fi
