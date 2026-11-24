#!/bin/sh
# Checks that a shared object imports only what the libraries it loads export.
#
#   tools/check-symbols.sh <library> <libdir>...
#
# Walks the DT_NEEDED graph starting at <library>. Each needed soname is looked
# up by file name under the given directories and their subdirectories,
# following symlinks. The check fails when a needed library is missing, or when
# a strong (non-weak) undefined symbol of <library> has no definition with the
# same name and version anywhere in that graph. Weak imports are optional for
# the loader and are only counted.
#
# The link step already rejects an unresolved strong reference, but only
# against the libraries present when it runs. cross-aarch64.sh links glibc and
# libstdc++ from the build host's toolchain, which can be newer than what the
# target distribution ships; this check answers against the distribution's own
# libraries instead. It does not show that the code works.
set -eu

lib=${1:?usage: check-symbols.sh <library> <libdir>...}
[ $# -ge 2 ] || { echo "usage: check-symbols.sh <library> <libdir>..." >&2; exit 2; }
shift
for dir in "$@"; do
    [ -d "$dir" ] || { echo "error: $dir is not a directory" >&2; exit 2; }
done

# Paths are exchanged through variables one per line.
IFS='
'
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# readelf -W --dyn-syms prints one symbol per line:
#   <num>: <value> <size> <type> <bind> <visibility> <ndx> <name>
# A reference reads name@VERSION, the default definition name@@VERSION, and
# both are followed by the version index in brackets. The second argument is an
# awk condition on those fields.
symbols() {
    readelf -W --dyn-syms "$1" | awk '
        $1 ~ /^[0-9]+:$/ && $8 != "" && '"$2"' {
            sub(/ \([0-9]+\)$/, "", $8); sub(/@@/, "@", $8); print $8
        }'
}
needed() {
    #   0x0000000000000001 (NEEDED)  Shared library: [libFcitx5Core.so.7]
    readelf -W -d "$1" | sed -n 's/^.*(NEEDED).*\[\(.*\)\]$/\1/p'
}

# File name -> path of every shared object below the given directories. A
# link-time name (libfoo.so) is usually a symlink to the soname.
find -L "$@" -name '*.so*' -type f | awk -F/ '
    { if (!($NF in seen)) { seen[$NF] = 1; print $NF "\t" $0 } }' > "$work/index"

# Breadth first over the libraries <library> loads, directly or not.
printf '%s\n' "$lib" > "$work/graph"
frontier=$lib
seen=''
status=0
while [ -n "$frontier" ]; do
    next=''
    for obj in $frontier; do
        for soname in $(needed "$obj"); do
            case " $seen " in *" $soname "*) continue ;; esac
            seen="$seen $soname"
            path=$(awk -F'\t' -v n="$soname" '$1==n{print $2; exit}' "$work/index")
            if [ -z "$path" ]; then
                echo "error: $(basename "$obj") needs $soname, which is not under $*" >&2
                status=1
                continue
            fi
            printf '%s\n' "$path" >> "$work/graph"
            next="$next
$path"
        done
    done
    frontier=$next
done

: > "$work/defs"
while IFS= read -r obj; do
    symbols "$obj" '$7 != "UND"' >> "$work/defs"
done < "$work/graph"
sort -u -o "$work/defs" "$work/defs"
symbols "$lib" '$7 == "UND" && $5 != "WEAK"' | sort -u > "$work/imports"
symbols "$lib" '$7 == "UND" && $5 == "WEAK"' | sort -u > "$work/weak"
comm -23 "$work/imports" "$work/defs" > "$work/missing"
comm -23 "$work/weak" "$work/defs" > "$work/optional"

count=$(wc -l < "$work/imports")
libraries=$(( $(wc -l < "$work/graph") - 1 ))
if [ -s "$work/missing" ]; then
    echo "error: $(wc -l < "$work/missing") of $count strong imports of $lib have no definition in the $libraries libraries it loads:" >&2
    head -20 "$work/missing" | sed 's/^/  /' >&2
    exit 1
fi
echo "$lib: $count strong imports resolved by $libraries libraries; $(wc -l < "$work/optional") optional imports unavailable"
[ "$status" -eq 0 ]
