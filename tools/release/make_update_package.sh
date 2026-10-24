#!/usr/bin/env bash
# Builds a player update package for mellowtone-launcher:
#
#   tools/release/make_update_package.sh BOARD VERSION SLOT_DIR OUT_DIR
#
# SLOT_DIR is the version slot's tree as it will sit in /data/mellowtone/current:
# at least `mellowtone` (the player, executable), usually `mellowtone-launcher`
# too. Writes OUT_DIR/mellowtone-BOARD-VERSION.tar (ustar) with a MANIFEST
# (board, version, SHA-256 of every file). Copy it to the card's
# .mellowtone/update/ and pick "Install player update" in the recovery menu.
set -euo pipefail

die() { echo "make_update_package: $*" >&2; exit 1; }
[[ $# -eq 4 ]] || die "usage: $0 BOARD VERSION SLOT_DIR OUT_DIR"
board=$1 version=$2 slot=$3 out=$4
case $board in r1|r3pro|r3proii|r3ii_2025) ;; *) die "unknown board $board" ;; esac
[[ $version =~ ^[A-Za-z0-9._+-]+$ ]] || die "bad version '$version'"
[[ -x $slot/mellowtone && -f $slot/mellowtone ]] || die "$slot/mellowtone missing or not executable"
[[ -z $(find "$slot" ! -type f ! -type d -print -quit) ]] || die "$slot contains links or special files"
[[ ! -e $slot/MANIFEST ]] || die "$slot must not contain a MANIFEST"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
(cd "$slot" && find . -type f | sed 's|^\./||' | LC_ALL=C sort) > "$tmp/files"
while IFS= read -r f; do
    (( ${#f} < 100 )) || die "path too long for ustar: $f"
done < "$tmp/files"
{
    echo "mellowtone-package 1"
    echo "board $board"
    echo "version $version"
    while IFS= read -r f; do
        echo "file $(sha256sum "$slot/$f" | cut -d' ' -f1) $f"
    done < "$tmp/files"
} > "$tmp/MANIFEST"

mkdir -p "$out"
pkg="$out/mellowtone-$board-$version.tar"
mtime=${SOURCE_DATE_EPOCH:-0}
tar --format=ustar --owner=0 --group=0 --numeric-owner --mtime="@$mtime" \
    -cf "$pkg" -C "$tmp" MANIFEST -C "$(realpath "$slot")" -T "$tmp/files"
echo "$pkg"
