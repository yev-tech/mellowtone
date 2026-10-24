#!/bin/sh
# Builds bluetoothd for the R3 Pro inside mt-bluez.
#   mt-bluez-build build  OUT_DIR   pristine tarball + series -> OUT_DIR/bluetoothd
#   mt-bluez-build dev    DEV_DIR   scratch git tree, one commit per patch (for editing)
#   mt-bluez-build export DEV_DIR   regenerate patches/bluez-4.101/*.patch from DEV_DIR
set -eu

VERSION=4.101
TARBALL_URL="https://www.kernel.org/pub/linux/bluetooth/bluez-$VERSION.tar.xz"
TARBALL_SHA256=41f9578bef39b8c94a2d6ddeaa556afd22d136936d0f03100e422fe970a45a7d
CACHE=${MT_CACHE_DIR:-/cache}
PATCHES=/src/patches/bluez-$VERSION
TC=/opt/ingenic
SYS=/opt/sysroot

fetch() {
    mkdir -p "$CACHE"
    t="$CACHE/bluez-$VERSION.tar.xz"
    if [ ! -f "$t" ]; then
        curl -fsSL -o "$t.part" "$TARBALL_URL"
        mv "$t.part" "$t"
    fi
    echo "$TARBALL_SHA256  $t" | sha256sum -c --status || { echo "bluez tarball checksum mismatch: $t" >&2; exit 1; }
    echo "$t"
}

unpack_patched() {
    up_dir=$1
    rm -rf "$up_dir"
    mkdir -p "$up_dir"
    tar -xJf "$(fetch)" -C "$up_dir" --strip-components=1
    for p in $(cat "$PATCHES/series"); do
        patch -d "$up_dir" -p1 --fuzz=0 -s < "$PATCHES/$p" || { echo "patch failed: $p" >&2; exit 1; }
    done
}

toolchain_env() {
    [ -x "$TC/bin/mips-linux-gnu-gcc" ] || { echo "Ingenic toolchain not mounted at $TC (set MT_INGENIC_TC)" >&2; exit 1; }
    [ -d "$SYS/usr/lib" ] || { echo "device sysroot not mounted at $SYS (set MT_BT_SYSROOT)" >&2; exit 1; }
    export PATH="$TC/bin:$PATH" CC=mips-linux-gnu-gcc
    export CFLAGS="-Os -Wno-error --sysroot=$TC/mips-linux-gnu/libc -I$SYS/usr/include"
    export LDFLAGS="--sysroot=$TC/mips-linux-gnu/libc -L$SYS/usr/lib -Wl,-rpath-link,$SYS/usr/lib"
    export GLIB_CFLAGS="-I$SYS/usr/include/glib-2.0 -I$SYS/usr/lib/glib-2.0/include"
    export GLIB_LIBS="-L$SYS/usr/lib -lglib-2.0 -lgmodule-2.0 -lgobject-2.0"
    export DBUS_CFLAGS="-I$SYS/usr/include/dbus-1.0 -I$SYS/usr/lib/dbus-1.0/include"
    export DBUS_LIBS="-L$SYS/usr/lib -ldbus-1"
}

configure_and_build() {
    cb_dir=$1
    toolchain_env
    cd "$cb_dir"
    ./configure --host=mips-linux-gnu --build=x86_64-linux-gnu --prefix=/usr --sysconfdir=/etc \
        --localstatedir=/data --disable-gstreamer --disable-alsa --disable-usb --disable-cups \
        --disable-test --disable-pand --disable-dund --disable-hidd --disable-tools --disable-bccmd \
        --disable-hid2hci --disable-dfutool --disable-debug --enable-audio --disable-pcmcia \
        > configure.log 2>&1 || { tail -20 configure.log; exit 1; }
    make src/builtin.h > make.log 2>&1 || { cat make.log; exit 1; }   # generated header first (parallel race otherwise)
    make -j"$(nproc)" src/bluetoothd >> make.log 2>&1 || { cat make.log; exit 1; }
}

cmd=${1:?usage: mt-bluez-build build|dev|export DIR}
dir=${2:?missing directory}
case "$cmd" in
    build)
        work=/tmp/bluez-build
        unpack_patched "$work"
        configure_and_build "$work"
        mkdir -p "$dir"
        cp "$work/src/bluetoothd" "$dir/bluetoothd"
        md5sum "$dir/bluetoothd"
        ;;
    dev)
        rm -rf "$dir"; mkdir -p "$dir"
        tar -xJf "$(fetch)" -C "$dir" --strip-components=1
        cd "$dir"
        git init -q
        git add -A
        git -c user.name=mt -c user.email=mt@localhost commit -qm "BlueZ $VERSION (pristine)"
        for p in $(cat "$PATCHES/series"); do
            patch -p1 --fuzz=0 -s < "$PATCHES/$p"
            # Commit message = the patch's description: a format-patch Subject/body,
            # or the plain text before the first diff.
            msg=$(mktemp)
            if grep -q '^Subject: ' "$PATCHES/$p"; then
                # Headers fold long lines; unfold the subject before committing.
                sed -n '/^Subject: /,/^---$/p' "$PATCHES/$p" | sed '$d' | sed 's/^Subject: \(\[PATCH[^]]*\] \)\{0,1\}//' |
                    awk 'hdr && /^ / { sub(/ +$/, "", line); sub(/^ +/, " "); line = line $0; next }
                         { if (NR > 1) print line; line = $0 }
                         /^$/ { hdr = 0 } NR == 1 { hdr = 1 }
                         END { print line }' > "$msg"
            else
                sed -n '1,/^--- /p' "$PATCHES/$p" | sed '$d' > "$msg"
            fi
            [ -s "$msg" ] || echo "${p%.patch}" > "$msg"
            # Keep a format-patch's author and date, so export gives the same
            # patch back; plain patches are committed as "mt".
            author=$(sed -n '/^---$/q; s/^From: //p' "$PATCHES/$p" | head -n1)
            date=$(sed -n '/^---$/q; s/^Date: //p' "$PATCHES/$p" | head -n1)
            git add -A
            git -c user.name=mt -c user.email=mt@localhost commit -q -F "$msg" \
                ${author:+--author="$author"} ${date:+--date="$date"}
            rm -f "$msg"
        done
        echo "dev tree ready: $dir (one commit per patch; edit, commit, then ./mt bluez-export)"
        ;;
    export)
        [ -d "$dir/.git" ] || { echo "$dir is not a bluez-dev tree (run ./mt bluez-dev first)" >&2; exit 1; }
        cd "$dir"
        base=$(git rev-list --max-parents=0 HEAD)
        tmp=$(mktemp -d)
        git format-patch -q --no-numbered --no-signature --zero-commit -o "$tmp" "$base"
        rm -f "$PATCHES"/*.patch; : > "$PATCHES/series"
        for f in "$tmp"/*.patch; do
            name=$(basename "$f"); cp "$f" "$PATCHES/$name"; echo "$name" >> "$PATCHES/series"
        done
        rm -rf "$tmp"
        echo "exported $(wc -l < "$PATCHES/series") patches to $PATCHES"
        ;;
    *) echo "unknown command: $cmd" >&2; exit 2 ;;
esac
