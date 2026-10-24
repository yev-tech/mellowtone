#!/usr/bin/env bash
# R3 Pro firmware image recipe with the build checks B1-B8 (docs/FIRMWARE.md).
# Runs inside the mt-image container as (namespace) root, so
# it can give every file its exact owner; started by `./mt image r3pro`.
#
#   build.sh MODE STOCK_UPT OUT_DIR VERSION
#
# MODE: null     the stock rootfs repacked with our tooling
#       minimal  stock + /etc/mellowtone-test
#       bt-only  stock + our bluetoothd
#       full     Mellowtone
# Inputs (full/bt-only) come from the repository at /src: build/r3pro/
# (./mt build r3pro), out/r3pro/bluetoothd (./mt bluez); the Noto fonts from
# /fonts (checked against fonts.sha256). Work happens in /work (a tmpfs, so
# directory order -- and with it mkfs.ubifs's inode numbering -- depends only
# on the order this script creates files in).
#
# Nothing is written to OUT_DIR unless every check passes. Environment:
# SOURCE_DATE_EPOCH (required), MT_GIT_COMMIT, MT_GIT_DIRTY, MT_CONTAINER_IMAGE.
set -euo pipefail
export LC_ALL=C TZ=UTC PYTHONDONTWRITEBYTECODE=1

here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$here/../../.." && pwd)
mode=${1:?mode}; stock=${2:?stock .upt}; out=${3:?out dir}; version=${4:?version}
work=${MT_IMAGE_WORK:-/work}
: "${SOURCE_DATE_EPOCH:?SOURCE_DATE_EPOCH must be set}"
case $mode in null|minimal|bt-only|full) ;; *) echo "build.sh: unknown mode $mode" >&2; exit 2 ;; esac

# Stock parameters: the superblock of firmware 2.2.
STOCK_SHA256=$(cut -d' ' -f1 "$here/stock.sha256")
# -j: the stock journal (max_bud_bytes, 59 LEBs); mkfs.ubifs 2.3.0's default is one LEB less.
MKFS_ARGS=(-m 2048 -e 126976 -c 480 -x lzo -f 8 -l 4 -p 1 -k r5 -j 7491584)
VOLUME_LEBS=488          # ubi0 "rootfs" on the device (ubinfo)
LEB_SIZE=126976
KERNEL_PART=$((6 * 1024 * 1024))   # mtd2 "kernel"

report=$work/report.txt
fail=0
rm -rf "${work:?}"/* 2>/dev/null || true
mkdir -p "$work"
: > "$report"
say() { echo "$*" | tee -a "$report"; }
check() { # check NAME ok|FAIL detail
    say "$(printf '%-3s %-4s %s' "$1" "$2" "$3")"
    [ "$2" = ok ] || fail=1
}
die() { say "build.sh: $*"; exit 1; }
py() { python3 "$here/$1" "${@:2}"; }

name=mellowtone-r3pro-$version
[ "$mode" = full ] || name=$name-$mode
say "Mellowtone R3 Pro image: $name.upt (mode $mode)"
say "commit ${MT_GIT_COMMIT:-unknown}${MT_GIT_DIRTY:+ (uncommitted changes)}, SOURCE_DATE_EPOCH $SOURCE_DATE_EPOCH"
say ""

# ---- B1: the input -----------------------------------------------------------
have=$(sha256sum "$stock" | cut -d' ' -f1)
if [ "$have" != "$STOCK_SHA256" ]; then
    check B1 FAIL "stock image sha256 $have is not firmware 2.2 ($STOCK_SHA256); refused"
    exit 1
fi
check B1 ok "stock image is HiBy R3 Pro firmware 2.2 (sha256 $have)"

# ---- unpack ------------------------------------------------------------------
mkdir -p "$work/stock-iso"
isoinfo -d -i "$stock" > "$work/stock-iso.pvd"
isoinfo -l -i "$stock" > "$work/stock-iso.ls"
if ! grep -q 'NO Joliet present' "$work/stock-iso.pvd" || ! grep -q 'NO Rock Ridge present' "$work/stock-iso.pvd"; then
    die "stock ISO has Joliet or Rock Ridge: not the layout this recipe knows"
fi
# Plain ISO 9660 names in directory order, e.g. "SYSTEM.UBI;1".
iso_names() { awk '/^-/ { n = $NF; if (n != "." && n != "..") print n }' "$1"; }
iso_names "$work/stock-iso.ls" > "$work/stock-iso.names"
while read -r n; do
    isoinfo -i "$stock" -x "/$n" > "$work/stock-iso/${n%;1}"
done < "$work/stock-iso.names"
for f in SYSTEM.UBI UIMAGE.BIN UPDATE.TXT VERSION.TXT _GITIGNO.; do
    [ -f "$work/stock-iso/$f" ] || die "stock ISO has no $f"
done
[ "$(wc -l < "$work/stock-iso.names")" -eq 5 ] || die "stock ISO has files this recipe does not know"
py ubifs_tree.py info "$work/stock-iso/SYSTEM.UBI" > "$work/stock-sb.json"
py ubifs_tree.py extract "$work/stock-iso/SYSTEM.UBI" "$work/stock-tree" "$work/stock.json" | tee -a "$report"

# ---- inputs and the change list (the manifest of intended changes) ----------
inputs=$work/inputs
mkdir -p "$inputs"
changes=$work/changes.tsv
: > "$changes"
add() { printf '%s\n' "$(IFS=$'\t'; echo "$*")" >> "$changes"; }
need() { [ -f "$1" ] || die "missing input $1 ($2)"; }

if [ "$mode" = minimal ]; then
    printf 'Mellowtone test image: this file is its only change.\n' > "$inputs/mellowtone-test"
    add file etc/mellowtone-test 0644 0 0 "$inputs/mellowtone-test"
fi
if [ "$mode" = bt-only ] || [ "$mode" = full ]; then
    need "$src/out/r3pro/bluetoothd" "./mt bluez"
    cp "$src/out/r3pro/bluetoothd" "$inputs/bluetoothd"
    add file usr/sbin/bluetoothd - - - "$inputs/bluetoothd"   # owner and mode stay the stock file's
fi
if [ "$mode" = full ]; then
    need "$src/build/r3pro/mellowtone-r3pro" "./mt build r3pro"
    need "$src/build/r3pro/mellowtone-launcher" "./mt build r3pro"
    cp "$src/build/r3pro/mellowtone-r3pro" "$inputs/mellowtone"
    cp "$src/build/r3pro/mellowtone-launcher" "$inputs/mellowtone-launcher"
    cp "$src/boards/r3pro/launcher/hiby_player.sh" "$inputs/hiby_player.sh"
    printf '%s\n' "$version" > "$inputs/VERSION"
    # Fonts: pinned by fonts.sha256 (name on the device, sha256).
    mkdir -p "$inputs/fonts"
    while read -r sum f; do
        case $sum in ''|'#'*) continue ;; esac
        need "/fonts/$f" "--fonts DIR"
        [ "$(sha256sum "/fonts/$f" | cut -d' ' -f1)" = "$sum" ] || die "/fonts/$f does not match fonts.sha256"
        cp "/fonts/$f" "$inputs/fonts/$f"
    done < "$here/fonts.sha256"
    # Theme: repo assets/themes + the narrow-panel overlay + stock images it lacks.
    sh "$src/boards/r3pro/tools/make_theme.sh" "$work/stock-tree/usr/resource/litegui/theme2" "$inputs/themes"

    # 1. The stock player and the libraries only it needs.
    add rm usr/bin/hiby_player "the stock player (replaced by Mellowtone)"
    py elf_check.py removable "$work/stock-tree" usr/bin/hiby_player > "$work/removable.tsv" 2> "$work/removable.log"
    cat "$work/removable.tsv" >> "$changes"
    # 2. The init shim in place of the stock player's start script.
    add file usr/bin/hiby_player.sh - - - "$inputs/hiby_player.sh"
    # 3. /usr/mellowtone: factory player + launcher, and the read-only assets
    #    the player reads from paths.image_root (boards/r3pro/board.c).
    add dir usr/mellowtone 0755 0 0
    add dir usr/mellowtone/factory 0755 0 0
    add file usr/mellowtone/factory/mellowtone 0755 0 0 "$inputs/mellowtone"
    add file usr/mellowtone/factory/mellowtone-launcher 0755 0 0 "$inputs/mellowtone-launcher"
    add file usr/mellowtone/factory/VERSION 0644 0 0 "$inputs/VERSION"
    add dir usr/mellowtone/resource 0755 0 0
    for n in 0 1; do  # the stock player's volume tables; board.c picks one by dac_choice
        add file "usr/mellowtone/resource/ot_devices_$n.json" 0644 0 0 "$work/stock-tree/usr/resource/ot_devices_$n.json"
    done
    add dir usr/mellowtone/fonts 0755 0 0
    while read -r f; do add file "usr/mellowtone/fonts/$f" 0644 0 0 "$inputs/fonts/$f"; done \
        < <(cd "$inputs/fonts" && find . -type f | sed 's|^\./||' | sort)
    add dir usr/mellowtone/themes 0755 0 0
    while read -r d; do add dir "usr/mellowtone/themes/$d" 0755 0 0; done \
        < <(cd "$inputs/themes" && find . -mindepth 1 -type d | sed 's|^\./||' | sort)
    while read -r f; do add file "usr/mellowtone/themes/$f" 0644 0 0 "$inputs/themes/$f"; done \
        < <(cd "$inputs/themes" && find . -type f | sed 's|^\./||' | sort)
    [ -z "$(cd "$inputs/themes" && find . ! -type f ! -type d)" ] || die "themes has links or special files"
fi

# ---- build (twice, for B8) -----------------------------------------------------
# UUID: fixed per input (stock image, change list, version), so the same
# inputs give the same image.
uuid=$( { echo "$STOCK_SHA256 $version $mode"; cut -f1-5 "$changes"; \
          awk -F'\t' '$1 == "file" { print $6 }' "$changes" | xargs -r sha256sum | cut -d' ' -f1; } \
        | sha256sum | cut -c1-32)

build_once() { # build_once DIR
    local b=$1
    mkdir -p "$b"
    py ubifs_tree.py extract "$work/stock-iso/SYSTEM.UBI" "$b/tree" "$b/stock.json" > /dev/null
    py ubifs_tree.py target "$b/stock.json" "$changes" "$b/tree" "$b/target.json" "$SOURCE_DATE_EPOCH"
    py ubifs_tree.py apply "$b/target.json" "$b/tree" "$b/devtable" > "$b/apply.log"
    local dt=()
    [ -s "$b/devtable" ] && dt=(-D "$b/devtable")
    mkfs.ubifs "${MKFS_ARGS[@]}" "${dt[@]}" -r "$b/tree" -o "$b/system.ubi" > "$b/mkfs.log" 2>&1 \
        || { cat "$b/mkfs.log"; die "mkfs.ubifs failed"; }
    py ubifs_tree.py fixup "$b/system.ubi" "$b/target.json" "$uuid" > "$b/fixup.log"
    # ISO contents: the stock files, our rootfs, UPDATE.TXT md5 values and VERSION.TXT ver=.
    mkdir -p "$b/iso"
    cp "$work/stock-iso/UIMAGE.BIN" "$b/iso/uimage.bin"
    cp "$b/system.ubi" "$b/iso/system.ubi"
    cp "$work/stock-iso/_GITIGNO." "$b/iso/.gitignore"   # a build leftover; genisoimage names it _GITIGNO.;1 as in stock
    python3 - "$work/stock-iso/UPDATE.TXT" "$b/iso/update.txt" "$b/iso" <<'EOF'
import hashlib, re, sys
text = open(sys.argv[1], 'rb').read().decode('ascii')
def fix(m):
    block = m.group(0)
    path = re.search(r'file_path=autoupdate/(\S+)', block).group(1)
    md5 = hashlib.md5(open(sys.argv[3] + '/' + path, 'rb').read()).hexdigest()
    return re.sub(r'md5=[0-9a-f]{32}', 'md5=' + md5, block)
out = re.sub(r'\w+=\{[^}]*\}', fix, text)
open(sys.argv[2], 'wb').write(out.encode('ascii'))
EOF
    python3 - "$work/stock-iso/VERSION.TXT" "$b/iso/version.txt" "$version" <<'EOF'
import re, sys
text = open(sys.argv[1], 'rb').read().decode('ascii')
out, n = re.subn(r'(ver=)\S+', lambda m: m.group(1) + sys.argv[3], text)
assert n == 1, 'VERSION.TXT: no single ver='
open(sys.argv[2], 'wb').write(out.encode('ascii'))
EOF
    # Fixed times on every input of the ISO (directory records carry them).
    find "$b/iso" -exec touch -h -d "@$SOURCE_DATE_EPOCH" {} +
    # Plain ISO 9660 as the stock image: volume id CDROM, system id LINUX,
    # no Rock Ridge, no Joliet (recoveryd's loop mount folds the 8.3 names
    # to the lower-case paths UPDATE.TXT uses).
    genisoimage -quiet -V CDROM -sysid LINUX -o "$b/image.upt" "$b/iso" 2> "$b/genisoimage.log" \
        || { cat "$b/genisoimage.log"; die "genisoimage failed"; }
    # genisoimage stamps the volume descriptor with the current time: set the
    # four dates to SOURCE_DATE_EPOCH (ISO 9660 8.4.26-29, 17-byte fields).
    python3 - "$b/image.upt" "$SOURCE_DATE_EPOCH" <<'EOF'
import sys, time
path, sde = sys.argv[1], int(sys.argv[2])
stamp = time.strftime('%Y%m%d%H%M%S00', time.gmtime(sde)).encode() + b'\x00'
with open(path, 'r+b') as f:
    for sector in range(16, 64):
        f.seek(sector * 2048)
        vd = bytearray(f.read(2048))
        if vd[1:6] != b'CD001':
            raise SystemExit('no volume descriptor at sector %d' % sector)
        if vd[0] == 255:
            break
        if vd[0] == 1:
            for off in (813, 830, 864):
                vd[off:off + 17] = stamp
            vd[847:864] = b'0000000000000000\x00'  # no expiration date
            f.seek(sector * 2048)
            f.write(vd)
EOF
}

say ""
build_once "$work/b1"
build_once "$work/b2"
img=$work/b1/image.upt
ubi=$work/b1/system.ubi

# ---- B2: kernel ----------------------------------------------------------------
isoinfo -i "$img" -x '/UIMAGE.BIN;1' > "$work/our-uimage.bin"
if cmp -s "$work/our-uimage.bin" "$work/stock-iso/UIMAGE.BIN"; then
    check B2 ok "kernel UIMAGE.BIN bit-identical to stock ($(stat -c %s "$work/our-uimage.bin") bytes, md5 $(md5sum < "$work/our-uimage.bin" | cut -c1-32))"
else
    check B2 FAIL "kernel differs from stock"
fi

# ---- B3: UBIFS superblock -------------------------------------------------------
py ubifs_tree.py info "$ubi" > "$work/our-sb.json"
b3=$(python3 - "$work/stock-sb.json" "$work/our-sb.json" "$(stat -c %s "$ubi")" "$VOLUME_LEBS" \
     "$work/stock-iso/SYSTEM.UBI" "$ubi" <<'EOF'
import json, sys
s, o = json.load(open(sys.argv[1])), json.load(open(sys.argv[2]))
size, volume = int(sys.argv[3]), int(sys.argv[4])
leb0 = [open(p, 'rb').read(s['leb_size'])[4096:] for p in sys.argv[5:7]]
bad = ['%s %r (stock %r)' % (k, o.get(k), s.get(k)) for k in sorted(set(s) | set(o))
       if k not in ('leb_cnt', 'uuid') and s.get(k) != o.get(k)]
if o['max_leb_cnt'] > volume: bad.append('max_leb_cnt %d > volume %d LEBs' % (o['max_leb_cnt'], volume))
if o['leb_cnt'] > o['max_leb_cnt']: bad.append('leb_cnt %d > max_leb_cnt' % o['leb_cnt'])
if size != o['leb_cnt'] * o['leb_size']: bad.append('image size %d != leb_cnt x leb_size' % size)
if leb0[0] != leb0[1]: bad.append('LEB 0 after the superblock differs from stock')
print(('FAIL ' + '; '.join(bad)) if bad else
      'ok all %d superblock fields equal stock except leb_cnt (%d, stock %d) and uuid; rest of LEB 0 as stock; max_leb_cnt %d <= volume %d; '
      'used %d of %d LEBs (%d free)' % (len(s) - 2, o['leb_cnt'], s['leb_cnt'], o['max_leb_cnt'], volume,
                                        o['leb_cnt'], o['max_leb_cnt'], o['max_leb_cnt'] - o['leb_cnt']))
EOF
)
check B3 "${b3%% *}" "${b3#* }"

# ---- B4: round trip ----------------------------------------------------------------
py ubifs_tree.py extract "$ubi" "$work/check-tree" "$work/our.json" > "$work/b4-extract.log"
# The re-extracted tree with the image's own owners and modes: B5 runs binaries from it.
py ubifs_tree.py apply "$work/our.json" "$work/check-tree" "$work/check-devtable" >> "$work/b4-extract.log"
if py ubifs_tree.py compare "$work/b1/target.json" "$work/our.json" > "$work/b4.log"; then
    check B4 ok "re-extracted rootfs equals stock + change list: $(tail -1 "$work/b4.log" | sed 's/^ubifs_tree: compare: //') (type, perm incl. setuid, uid/gid, size, sha256, symlink target, device numbers, xattrs, inode flags, link count, times)"
else
    tee -a "$report" < "$work/b4.log"
    check B4 FAIL "re-extracted rootfs differs from stock + change list"
fi

# ---- B5: ELF files ------------------------------------------------------------------
if py elf_check.py check "$work/check-tree" "$work/stock-tree" > "$work/b5.log"; then b5=ok; else b5=FAIL; fi
cat "$work/b5.log" >> "$report"
runs=()
while IFS=$'\t' read -r op path _ _ _ srcf; do
    [ "$op" = file ] || continue
    head -c4 "$srcf" | grep -q 'ELF' || continue
    case $path in
        usr/sbin/bluetoothd) args=(-v) ;;
        *) args=(--version) ;;
    esac
    if outp=$(cd "$work/check-tree" && timeout 30 qemu-mipsel -L "$work/check-tree" "$work/check-tree/$path" "${args[@]}" 2>&1); then
        runs+=("/$path ${args[*]}: $(echo "$outp" | head -1)")
    else
        runs+=("/$path ${args[*]}: FAILED: $(echo "$outp" | head -3 | tr '\n' ' ')")
        b5=FAIL
    fi
done < "$changes"
[ ${#runs[@]} -gt 0 ] || runs=("no new binaries in this image")
for r in "${runs[@]}"; do say "  B5: qemu-mipsel -L rootfs $r"; done
check B5 $b5 "ELF files MIPS32 LE, interpreters and NEEDED libraries present, new binaries run under qemu-mipsel"

# ---- B6: scripts -----------------------------------------------------------------------
b6=ok; n6=0
while IFS=$'\t' read -r op path _ _ _ srcf; do
    [ "$op" = file ] || continue
    head -c2 "$srcf" | grep -q '#!' || continue
    n6=$((n6 + 1))
    if ! sh -n "$work/check-tree/$path" 2>> "$work/b6.log"; then b6=FAIL; say "  B6: sh -n /$path failed"; fi
    if ! shellcheck -s sh "$work/check-tree/$path" >> "$work/b6.log" 2>&1; then b6=FAIL; say "  B6: shellcheck /$path failed"; fi
done < "$changes"
[ "$b6" = ok ] || tee -a "$report" < "$work/b6.log"
check B6 $b6 "$n6 changed script(s): sh -n and shellcheck clean"

# ---- B7: ISO, UPDATE.TXT, size -------------------------------------------------------
b7=ok; d7=()
isoinfo -d -i "$img" > "$work/our-iso.pvd"
isoinfo -l -i "$img" > "$work/our-iso.ls"
iso_names "$work/our-iso.ls" > "$work/our-iso.names"
if cmp -s "$work/stock-iso.names" "$work/our-iso.names"; then
    d7+=("ISO files $(tr '\n' ' ' < "$work/our-iso.names")in stock order")
else
    b7=FAIL; d7+=("ISO file list differs: $(tr '\n' ' ' < "$work/our-iso.names") vs stock $(tr '\n' ' ' < "$work/stock-iso.names")")
fi
for k in 'Volume id' 'System id' 'Logical block size'; do
    [ "$(grep "^$k:" "$work/our-iso.pvd")" = "$(grep "^$k:" "$work/stock-iso.pvd")" ] || { b7=FAIL; d7+=("$k differs"); }
done
if ! grep -q 'NO Joliet present' "$work/our-iso.pvd" || ! grep -q 'NO Rock Ridge present' "$work/our-iso.pvd"; then
    b7=FAIL; d7+=("Joliet or Rock Ridge present")
fi
isoinfo -i "$img" -x '/UPDATE.TXT;1' > "$work/our-update.txt"
while read -r part file md5; do
    got=$(isoinfo -i "$img" -x "/$(echo "${file#autoupdate/}" | tr '[:lower:]' '[:upper:]');1" | md5sum | cut -c1-32)
    if [ "$got" = "$md5" ]; then d7+=("$part md5 $md5 matches"); else b7=FAIL; d7+=("$part md5 $md5 but file has $got"); fi
done < <(python3 - "$work/our-update.txt" <<'EOF'
import re, sys
for m in re.finditer(r'(\w+)=\{([^}]*)\}', open(sys.argv[1]).read()):
    kv = dict(re.findall(r'(\w+)=(\S+)', m.group(2)))
    print(m.group(1), kv['file_path'], kv['md5'])
EOF
)
# Same text as stock apart from the md5 values (and VERSION.TXT's ver=).
python3 - "$work/stock-iso/UPDATE.TXT" "$work/our-update.txt" <<'EOF' || { b7=FAIL; d7+=("UPDATE.TXT format differs from stock"); }
import re, sys
a, b = (re.sub(r'md5=[0-9a-f]{32}', 'md5=X', open(p).read()) for p in sys.argv[1:3])
sys.exit(a != b)
EOF
isoinfo -i "$img" -x '/VERSION.TXT;1' > "$work/our-version.txt"
python3 - "$work/stock-iso/VERSION.TXT" "$work/our-version.txt" <<'EOF' || { b7=FAIL; d7+=("VERSION.TXT format differs from stock"); }
import re, sys
a, b = (re.sub(r'ver=\S+', 'ver=X', open(p).read()) for p in sys.argv[1:3])
sys.exit(a != b)
EOF
ubi_size=$(stat -c %s "$ubi")
vol_bytes=$((VOLUME_LEBS * LEB_SIZE))
if [ "$ubi_size" -le "$vol_bytes" ]; then
    d7+=("rootfs $ubi_size bytes <= volume $vol_bytes")
else
    b7=FAIL; d7+=("rootfs $ubi_size bytes > volume $vol_bytes")
fi
[ "$(stat -c %s "$work/our-uimage.bin")" -le $KERNEL_PART ] || { b7=FAIL; d7+=("kernel larger than mtd2"); }
d7+=("image $(stat -c %s "$img") bytes (stock $(stat -c %s "$stock"))")
for d in "${d7[@]}"; do say "  B7: $d"; done
check B7 $b7 "ISO layout, UPDATE.TXT/VERSION.TXT, md5 values and sizes"

# ---- B8: reproducible ---------------------------------------------------------------
if cmp -s "$work/b1/image.upt" "$work/b2/image.upt"; then
    check B8 ok "two builds from clean trees are bit-identical (sha256 $(sha256sum < "$img" | cut -c1-64))"
else
    check B8 FAIL "two builds differ: $(cmp "$work/b1/image.upt" "$work/b2/image.upt" | head -1)"
fi

say ""
if [ $fail -ne 0 ]; then
    say "RESULT: FAILED -- no image written"
    exit 1
fi

# ---- outputs ---------------------------------------------------------------------------
manifest=$work/manifest.txt
{
    echo "image $name.upt"
    echo "sha256 $(sha256sum < "$img" | cut -c1-64)"
    echo "mode $mode"
    echo "version $version"
    echo "git_commit ${MT_GIT_COMMIT:-unknown}${MT_GIT_DIRTY:+ +uncommitted changes}"
    echo "container ${MT_CONTAINER_IMAGE:-unknown}"
    echo "tools $(mkfs.ubifs --version 2>&1 | head -1); genisoimage $(dpkg-query -W -f '${Version}' genisoimage); ubi_reader $(pip3 show ubi_reader 2>/dev/null | awk '/^Version/ {print $2}')"
    echo "source_date_epoch $SOURCE_DATE_EPOCH"
    echo "stock_sha256 $STOCK_SHA256"
    echo "mkfs.ubifs ${MKFS_ARGS[*]} (UUID $uuid)"
    echo
    echo "# ISO files: sha256 size name"
    while read -r n; do
        echo "$(isoinfo -i "$img" -x "/$n" | sha256sum | cut -c1-64) $(isoinfo -i "$img" -x "/$n" | wc -c) $n"
    done < "$work/our-iso.names"
    echo
    echo "# inputs: sha256 path"
    awk -F'\t' '$1 == "file" { print $6 }' "$changes" | sort -u | while read -r f; do
        echo "$(sha256sum < "$f" | cut -c1-64) ${f#"$work"/}"
    done
    echo
    echo "# change list (op path perm uid gid source | reason)"
    sed "s|$work/||g" "$changes"
    echo
    echo "# rootfs: type perm uid gid size sha256|target path"
    python3 - "$work/our.json" <<'EOF'
import json, sys
for p, e in sorted(json.load(open(sys.argv[1]))['entries'].items()):
    what = e.get('sha256') or e.get('target') or ('%d,%d' % tuple(e['dev']) if 'dev' in e else '-')
    print(e['type'], e['perm'], e['uid'], e['gid'], e.get('size', '-'), what, p)
EOF
} > "$manifest"
if [ -s "$work/removable.log" ]; then
    { echo; echo "Library removal (elf_check.py removable):"; sed 's/^/  /' "$work/removable.log"
      awk -F'\t' '$1 == "rm" { print "  removed /" $2 ": " $3 }' "$changes"; } >> "$report"
fi
say "RESULT: all checks passed"
mkdir -p "$out"
cp "$img" "$out/$name.upt.tmp"
cp "$report" "$out/$name.report.txt"
cp "$manifest" "$out/$name.manifest.txt"
mv "$out/$name.upt.tmp" "$out/$name.upt"
if [ -n "${MT_HOST_UID:-}" ]; then chown "$MT_HOST_UID:${MT_HOST_GID:-$MT_HOST_UID}" "$out/$name".* "$out"; fi
echo "$out/$name.upt"
