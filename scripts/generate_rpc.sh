#!/usr/bin/env bash
set -euo pipefail

root_dir=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
idl="RPCInterface/meathook_interface.idl"
verify=0
if [[ "${1:-}" == "--verify" ]]; then
    verify=1
elif [[ $# -ne 0 ]]; then
    printf 'usage: %s [--verify]\n' "$0" >&2
    exit 2
fi
out_dir=$(mktemp -d "${TMPDIR:-/tmp}/meathook-rpc.XXXXXX")
trap 'rm -rf "$out_dir"' EXIT

if command -v widl >/dev/null 2>&1; then
    widl_bin=$(command -v widl)
elif command -v mingw-w64-widl >/dev/null 2>&1; then
    widl_bin=$(command -v mingw-w64-widl)
else
    printf '%s\n' 'widl or mingw-w64-widl is required' >&2
    exit 127
fi

cd "$root_dir"
"$widl_bin" --win64 -Oif -h -o "$out_dir/meathook_interface.h" "$idl"
"$widl_bin" --win64 -Oif -c -o "$out_dir/meathook_interface_c.c" "$idl"
"$widl_bin" --win64 -Oif -s -o "$out_dir/meathook_interface_s.c" "$idl"

for file in meathook_interface.h meathook_interface_c.c meathook_interface_s.c; do
    test -s "$out_dir/$file"
    if (( verify )); then
        cmp -s "$out_dir/$file" "$root_dir/RPCInterface/$file" || {
            printf 'generated artifact drift: RPCInterface/%s\n' "$file" >&2
            exit 1
        }
    else
        mv -f "$out_dir/$file" "$root_dir/RPCInterface/$file"
    fi
done

if (( verify )); then
    mod_root="$root_dir/../DoomEternal-AP-Mod/native/client"
    for file in meathook_interface.h meathook_interface_c.c; do
        cmp -s "$root_dir/RPCInterface/$file" "$mod_root/$file" || {
            printf 'MOD generated projection drift: native/client/%s\n' "$file" >&2
            exit 1
        }
    done
fi
