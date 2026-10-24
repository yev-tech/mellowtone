#!/usr/bin/env bash
# mt: every build step for Mellowtone runs in a container (podman, or docker).
#
#   ./mt setup [build|bluez|image|all]   build the container images (default: all)
#   ./mt build <board>|host|host-minimal [-DVAR=val ...] [build args]
#                                        player and mellowtone-launcher for r3pro, r1, r3proii, r3ii_2025, or the
#                                        host simulator: CMake preset <board>, output in build/<board>/
#                                        (host-minimal: the simulator with every optional feature off).
#                                        -D... go to the configure step (e.g. -DRELEASE_LABEL="My build"),
#                                        the rest to `cmake --build` (e.g. --target launcher, -v)
#   ./mt test [host|host-minimal]        unit tests (tag writer, MusicBrainz matching, HTTP framing, state root and radio
#                                        lists, Wi-Fi, battery/charger, Bluetooth, USB modes, launcher, built
#                                        features) and the i18n check, via ctest in build/<preset> (default host)
#   ./mt i18n [check]                    regenerate translations / check they are complete
#   ./mt bluez [OUT_DIR]                 R3 Pro bluetoothd from BlueZ 4.101 + patches (default out/r3pro)
#   ./mt bluez-dev [DIR]                 editable BlueZ tree, one git commit per patch (default .bluez-dev)
#   ./mt bluez-export [DIR]              regenerate patches/bluez-4.101/ from that tree
#   ./mt image r3pro --stock <official r3pro.upt> [--null|--minimal|--bt-only] [--out DIR]
#              [--fonts DIR] [--version VER]
#                                        R3 Pro firmware image from your own official firmware
#                                        2.2 + checks B1-B8 (default out/images; never published)
#   ./mt shell [build|bluez|image]       interactive shell in a container
#
# Settings come from the environment or from ./.mt.local (not committed):
#   MT_ENGINE       podman (default if installed) or docker
#   MT_CACHE        download cache on the host (default ~/.cache/mellowtone)
#   MT_INGENIC_TC   Ingenic mips-gcc472-glibc216 toolchain (for ./mt bluez)
#   MT_BT_SYSROOT   device sysroot with glib/dbus libraries and headers (for ./mt bluez)
#   MT_FONTS_DIR    the Noto fonts for ./mt image (boards/r3pro/image/fonts.sha256)
#   SOURCE_DATE_EPOCH  passed through: fixes BUILD_STAMP for reproducible builds
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
[ -f "$ROOT/.mt.local" ] && . "$ROOT/.mt.local"

ENGINE=${MT_ENGINE:-$(command -v podman >/dev/null 2>&1 && echo podman || echo docker)}
CACHE=${MT_CACHE:-$HOME/.cache/mellowtone}
IMG_BUILD=localhost/mt-build:1
IMG_BLUEZ=localhost/mt-bluez:1
IMG_IMAGE=localhost/mt-image:1

die() { echo "mt: $*" >&2; exit 1; }

# Docker runs us as a numeric user the image does not know; podman adds the
# user itself. Tools that look the user up then fail: dbus-daemon will not
# start (the Bluetooth tests). Prints mounts of the image's own passwd and
# group files with this user added.
user_db_mounts() { # IMAGE
    local img=$1 uid gid dir
    uid=$(id -u) gid=$(id -g)
    dir="$CACHE/userdb/$uid-$gid"
    mkdir -p "$dir"
    "$ENGINE" run --rm --entrypoint cat "$img" /etc/passwd > "$dir/passwd" || die "cannot read /etc/passwd of $img"
    "$ENGINE" run --rm --entrypoint cat "$img" /etc/group > "$dir/group" || die "cannot read /etc/group of $img"
    awk -F: -v u="$uid" '$3 == u { found = 1 } END { exit !found }' "$dir/passwd" ||
        echo "mt:x:$uid:$gid:mt:/tmp:/bin/sh" >> "$dir/passwd"
    awk -F: -v g="$gid" '$3 == g { found = 1 } END { exit !found }' "$dir/group" ||
        echo "mt:x:$gid:" >> "$dir/group"
    echo "-v $dir/passwd:/etc/passwd:ro -v $dir/group:/etc/group:ro"
}

run() { # run IMAGE [extra engine args...] -- command...
    local img=$1; shift
    local extra=()
    while [ $# -gt 0 ] && [ "$1" != "--" ]; do extra+=("$1"); shift; done
    shift
    mkdir -p "$CACHE"
    local user_args=(--user "$(id -u):$(id -g)")
    if [ "$ENGINE" = podman ]; then
        user_args=(--userns=keep-id "${user_args[@]}")
    else
        user_args+=($(user_db_mounts "$img"))
    fi
    local tty=()
    [ -t 0 ] && [ -t 1 ] && tty=(-it)
    local env_args=()
    [ -n "${SOURCE_DATE_EPOCH:-}" ] && env_args=(-e "SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH")
    "$ENGINE" run --rm "${tty[@]}" "${user_args[@]}" "${env_args[@]}" \
        -v "$ROOT:/src" -v "$CACHE:/cache" -e MT_CACHE_DIR=/cache -e HOME=/tmp \
        -w /src "${extra[@]}" "$img" "$@"
}

run_as_root() { # run IMAGE -- command...  (container root = your user under rootless podman;
                #  for tests that switch users, e.g. the tag writer's "runs as nobody" case)
    local img=$1; shift; shift
    "$ENGINE" run --rm -v "$ROOT:/src" -e HOME=/tmp -w /src "$img" "$@"
}

need_image() {
    "$ENGINE" image exists "$1" 2>/dev/null || "$ENGINE" image inspect "$1" >/dev/null 2>&1 \
        || die "image $1 missing; run ./mt setup"
}

# Sets bluez_mounts=(...) for ./mt bluez. Runs in the main shell (not a
# command substitution) so die really stops ./mt.
bluez_mounts=()
set_bluez_mounts() {
    [ -n "${MT_INGENIC_TC:-}" ] || die "set MT_INGENIC_TC to the Ingenic mips-gcc472-glibc216 toolchain (see docs/TOOLS.md)"
    [ -n "${MT_BT_SYSROOT:-}" ] || die "set MT_BT_SYSROOT to the device sysroot (see docs/BUILDING.md)"
    [ -x "$MT_INGENIC_TC/bin/mips-linux-gnu-gcc" ] || die "MT_INGENIC_TC has no bin/mips-linux-gnu-gcc: $MT_INGENIC_TC"
    [ -d "$MT_BT_SYSROOT/usr/lib" ] || die "MT_BT_SYSROOT has no usr/lib: $MT_BT_SYSROOT"
    bluez_mounts=(-v "$MT_INGENIC_TC:/opt/ingenic:ro" -v "$MT_BT_SYSROOT:/opt/sysroot:ro")
}

cmd=${1:-help}; shift || true
case "$cmd" in
    setup)
        what=${1:-all}
        if [ "$what" = all ] || [ "$what" = build ]; then
            "$ENGINE" build -t "$IMG_BUILD" -f "$ROOT/containers/build/Containerfile" "$ROOT/tools/release"
        fi
        if [ "$what" = all ] || [ "$what" = bluez ]; then
            "$ENGINE" build -t "$IMG_BLUEZ" -f "$ROOT/containers/bluez/Containerfile" "$ROOT/containers/bluez"
        fi
        if [ "$what" = all ] || [ "$what" = image ]; then
            "$ENGINE" build -t "$IMG_IMAGE" -f "$ROOT/containers/image/Containerfile" "$ROOT/containers/image"
        fi
        ;;
    build)
        board=${1:?usage: ./mt build <board>|host|host-minimal}; shift || true
        case "$board" in r1|r3pro|r3proii|r3ii_2025|host|host-minimal) ;; *) die "unknown board '$board' (r1, r3pro, r3proii, r3ii_2025, host, host-minimal)" ;; esac
        need_image "$IMG_BUILD"
        configure_args=() build_args=()
        for a in "$@"; do
            case "$a" in -D*) configure_args+=("$a") ;; *) build_args+=("$a") ;; esac
        done
        # Configure every time: cheap, it refreshes BUILD_STAMP (only
        # app_version.c is recompiled for it), and the build-identity options
        # apply to this build only, as they did with make: unset ones are reset.
        run "$IMG_BUILD" -- cmake --preset "$board" --log-level=WARNING \
            -DRELEASE_LABEL= -DTEST_BUILD_TAG= -DUI_PERF_TRACE=OFF -DUI_GESTURE_TRACE=OFF -DUI_HITBOX_DEBUG=OFF \
            "${configure_args[@]}" >/dev/null
        run "$IMG_BUILD" -- cmake --build --preset "$board" "${build_args[@]}"
        ;;
    test)
        preset=${1:-host}
        case "$preset" in host|host-minimal) ;; *) die "unknown test preset '$preset' (host, host-minimal)" ;; esac
        need_image "$IMG_BUILD"
        run "$IMG_BUILD" -- sh -c 'cmake --preset "$1" --log-level=WARNING >/dev/null &&
            cmake --build --preset "$1" --target tests &&
            ctest --preset "$1" -LE root' sh "$preset"
        # The tag writer matrix runs one case as nobody, so it needs root in the container.
        run_as_root "$IMG_BUILD" -- ctest --preset "$preset" -L root
        ;;
    i18n)
        need_image "$IMG_BUILD"
        target=i18n
        [ "${1:-}" = check ] && target=i18n-check
        run "$IMG_BUILD" -- sh -c 'cmake --preset host --log-level=WARNING >/dev/null && cmake --build --preset host --target "$1"' sh "$target"
        ;;
    bluez)
        out=${1:-out/r3pro}
        need_image "$IMG_BLUEZ"
        set_bluez_mounts
        run "$IMG_BLUEZ" "${bluez_mounts[@]}" -- mt-bluez-build build "/src/$out"
        ;;
    bluez-dev)
        need_image "$IMG_BLUEZ"
        run "$IMG_BLUEZ" -- mt-bluez-build dev "/src/${1:-.bluez-dev}"
        ;;
    bluez-export)
        need_image "$IMG_BLUEZ"
        run "$IMG_BLUEZ" -- mt-bluez-build export "/src/${1:-.bluez-dev}"
        ;;
    image)
        board=${1:-}; shift || true
        [ "$board" = r3pro ] || die "usage: ./mt image r3pro --stock <official.upt> [--out DIR] [--null|--minimal|--bt-only] [--fonts DIR] [--version VER]"
        stock="" out_dir="$ROOT/out/images" mode=full fonts=${MT_FONTS_DIR:-} version=""
        while [ $# -gt 0 ]; do
            case "$1" in
                --stock) stock=${2:?--stock needs a file}; shift ;;
                --out) out_dir=${2:?--out needs a directory}; shift ;;
                --fonts) fonts=${2:?--fonts needs a directory}; shift ;;
                --version) version=${2:?--version needs a value}; shift ;;
                --null) mode=null ;;
                --minimal) mode=minimal ;;
                --bt-only) mode=bt-only ;;
                --full) mode=full ;;
                *) die "image: unknown option $1" ;;
            esac
            shift
        done
        [ -f "$stock" ] || die "image: --stock <your official r3pro.upt> is required"
        [ "$mode" != full ] || [ -d "$fonts" ] || die "image: --fonts DIR (or MT_FONTS_DIR) with the files in boards/r3pro/image/fonts.sha256"
        need_image "$IMG_IMAGE"
        mkdir -p "$out_dir"
        out_dir=$(cd "$out_dir" && pwd)
        # Images contain HiBy's files: they stay on this computer. Never write
        # one straight to a memory card (FAT/exFAT) or a removable mount.
        case "$out_dir" in /media/*|/run/media/*|/mnt/*) die "image: refusing to write to a removable mount ($out_dir)" ;; esac
        fstype=$(stat -f -c %T "$out_dir" 2>/dev/null || echo unknown)
        case "$fstype" in msdos|vfat|exfat|fuseblk) die "image: refusing to write to a $fstype file system ($out_dir)" ;; esac
        commit=$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo unknown)
        dirty=$(git -C "$ROOT" status --porcelain --untracked-files=no 2>/dev/null | head -1)
        sde=${SOURCE_DATE_EPOCH:-$(git -C "$ROOT" log -1 --format=%ct 2>/dev/null || echo 0)}
        [ -n "$version" ] || version=dev-$(date -u -d "@$sde" +%Y%m%d)-$(git -C "$ROOT" rev-parse --short=7 HEAD 2>/dev/null || echo unknown)${dirty:+-dirty}
        case "$version" in *[!A-Za-z0-9._+-]*|"") die "image: bad version '$version'" ;; esac
        img_id=$("$ENGINE" image inspect --format '{{.Id}}' "$IMG_IMAGE")
        # Rootless podman: container root is you. Docker: hand the outputs back.
        owner=()
        [ "$ENGINE" = podman ] || owner=(-e MT_HOST_UID="$(id -u)" -e MT_HOST_GID="$(id -g)")
        font_mount=()
        [ -n "$fonts" ] && font_mount=(-v "$(cd "$fonts" && pwd):/fonts:ro")
        # The theme ships tracked files only (boards/r3pro/tools/make_theme.sh);
        # the image container has no git, so hand it the list.
        theme_list=$(mktemp); chmod 644 "$theme_list"
        trap 'rm -f "$theme_list"' EXIT
        git -C "$ROOT" ls-files -- assets/themes boards/r3ii_2025/assets/themes boards/r3pro/assets/themes > "$theme_list" \
            || die "image: git ls-files failed (run from a git checkout)"
        # Container root (your own user under rootless podman) gives every
        # file its exact owner; the work tree is a tmpfs (see build.sh).
        "$ENGINE" run --rm -v "$ROOT:/src" -v "$(realpath "$stock"):/stock/stock.upt:ro" -v "$out_dir:/out" \
            "${font_mount[@]}" -v "$theme_list:/theme-files:ro" -e MT_THEME_FILES=/theme-files \
            --tmpfs /work:rw,size=3g,mode=0700 -w /src \
            -e SOURCE_DATE_EPOCH="$sde" -e MT_GIT_COMMIT="$commit" -e MT_GIT_DIRTY="${dirty:+1}" \
            -e MT_CONTAINER_IMAGE="$IMG_IMAGE $img_id" "${owner[@]}" \
            "$IMG_IMAGE" bash /src/boards/r3pro/image/build.sh "$mode" /stock/stock.upt /out "$version"
        ;;
    shell)
        which=${1:-build}
        case "$which" in build) img=$IMG_BUILD ;; bluez) img=$IMG_BLUEZ ;; image) img=$IMG_IMAGE ;; *) die "unknown image $which" ;; esac
        need_image "$img"
        run "$img" -- bash
        ;;
    help|-h|--help)
        sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
        ;;
    *) die "unknown command '$cmd' (./mt help)" ;;
esac
