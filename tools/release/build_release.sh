#!/usr/bin/env bash
# Builds the release files for the released boards:
#
#   tools/release/build_release.sh VERSION [OUT_DIR]
#
# Only boards tested on hardware are released (RELEASE_BOARDS below; the
# other boards are still built by CI). MT_RELEASE_BOARDS="r1 r3pro" overrides
# the list for a local test build.
#
# For each board: the player and launcher (./mt build <board>), a version
# slot (mellowtone, mellowtone-launcher, VERSION) and its update package
# (make_update_package.sh). Also the unstripped binaries, for crash
# backtraces, and SHA256SUMS. Output in OUT_DIR (default out/release/).
# SOURCE_DATE_EPOCH defaults to the commit time, so the same commit gives the
# same files. Needs ./mt setup build; used by .github/workflows/release.yml.
set -euo pipefail
die() { echo "build_release: $*" >&2; exit 1; }
[[ $# -ge 1 && $# -le 2 ]] || die "usage: $0 VERSION [OUT_DIR]"
version=$1
[[ $version =~ ^[A-Za-z0-9._+-]+$ ]] || die "bad version '$version'"
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
out=$(realpath -m "${2:-$root/out/release}")
RELEASE_BOARDS="r3pro" # add a board here once it is tested on hardware
read -r -a boards <<< "${MT_RELEASE_BOARDS:-$RELEASE_BOARDS}"
for board in "${boards[@]}"; do
    case $board in r1|r3pro|r3proii|r3ii_2025) ;; *) die "unknown board '$board'" ;; esac
done

export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-$(git -C "$root" log -1 --format=%ct)}
rm -rf "$out"
mkdir -p "$out"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

for board in "${boards[@]}"; do
    echo "== $board"
    "$root/mt" build "$board" -DRELEASE_LABEL="$version"
    build="$root/build/$board"
    slot="$work/slot-$board"
    mkdir -p "$slot" "$work/debug/$board"
    install -m 755 "$build/mellowtone-$board" "$slot/mellowtone"
    install -m 755 "$build/mellowtone-launcher" "$slot/mellowtone-launcher"
    echo "$version" > "$slot/VERSION"
    touch -d "@$SOURCE_DATE_EPOCH" "$slot"/*
    "$root/tools/release/make_update_package.sh" "$board" "$version" "$slot" "$out" >/dev/null
    cp "$build/mellowtone-${board}_unstripped" "$build/mellowtone-launcher_unstripped" "$work/debug/$board/"
done

tar --sort=name --owner=0 --group=0 --numeric-owner --mtime="@$SOURCE_DATE_EPOCH" \
    -C "$work/debug" -cJf "$out/mellowtone-$version-debug-symbols.tar.xz" .
(cd "$out" && sha256sum -- *.tar *.tar.xz > SHA256SUMS)
echo "Release files in $out:"
ls -l "$out"
