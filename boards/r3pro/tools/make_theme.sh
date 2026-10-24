#!/bin/sh
# Assemble the R3 Pro themes tree: the repo's tracked assets/themes files
# (including the launcher/radio, pull_down/m_* and topbar/m_* icons), the
# tracked narrow-panel overlay (boards/r3ii_2025/assets/themes), then the R3 Pro
# stock theme filling images the repo does not ship.
# usage: make_theme.sh STOCK_THEME2_DIR OUT_DIR
#
# Only files tracked in git are copied, as boards/common/repack_upt.sh does, so a
# local stock dump or stray file in assets/ never reaches the image. The list
# comes from `git ls-files`, or from the file named by MT_THEME_FILES (one
# repo-relative path per line) where git is not available (./mt image passes
# it into the image container). boot_animation/ comes only from the R3 Pro's
# own overlay (boards/r3pro/assets/themes, 360x480), never from the other boards. Licence files stay with the icons; only the SOURCES*.txt
# notes are left out.
set -eu
here=$(cd "$(dirname "$0")/../../.." && pwd)
stock=$1; out=$2
list=$(mktemp)
trap 'rm -f "$list"' EXIT
if [ -n "${MT_THEME_FILES:-}" ]; then
    cp "$MT_THEME_FILES" "$list"
else
    git -C "$here" ls-files -- assets/themes boards/r3ii_2025/assets/themes boards/r3pro/assets/themes > "$list" \
        || { echo "make_theme.sh: needs git, or MT_THEME_FILES with the tracked asset list" >&2; exit 1; }
fi
rm -rf "$out"; mkdir -p "$out"
copy_tracked() { # copy_tracked SRC_PREFIX [boot]: tracked files under it, relative layout kept
    grep "^$1/" "$list" | while IFS= read -r f; do
        rel=${f#"$1"/}
        case $rel in boot_animation/*) [ "${2:-}" = boot ] || continue ;; esac
        case ${rel##*/} in *SOURCES*.txt) continue ;; esac
        [ -f "$here/$f" ] || continue
        mkdir -p "$out/$(dirname "$rel")"
        cp "$here/$f" "$out/$rel"
    done
}
copy_tracked assets/themes
copy_tracked boards/r3ii_2025/assets/themes
copy_tracked boards/r3pro/assets/themes boot
[ -n "$(ls -A "$out")" ] || { echo "make_theme.sh: no tracked theme files found" >&2; exit 1; }
fill() { [ -f "$out/$2" ] || { [ -f "$stock/$1" ] && mkdir -p "$(dirname "$out/$2")" && cp "$stock/$1" "$out/$2"; } || true; }
for f in "$stock"/play_plane/*.png; do b=$(basename "$f"); fill "play_plane/$b" "playing_plane/$b"; done
fill play_plane/buttom_bg.png playing_plane/buttom.png
fill play_plane/default_cover.png playing_plane/default_cover_565.png
for d in bt keyboard sub_back stream_media; do
    [ -d "$stock/$d" ] && for f in "$stock/$d"/*; do fill "$d/$(basename "$f")" "$d/$(basename "$f")"; done
done
# Stock R3 Pro keeps the list icons under list/ (the player reads touch_list/).
for b in a_z_result_bg list_default_album list_folder quality_high quality_hr quality_nomal; do
    fill "list/$b.png" "touch_list/$b.png"
done
fill list/menu_song_delete.png touch_list/del.png
fill list/search.png sub_back/btn_search.png
fill list/radio_cover.png stream_media/radio.png
fill usb/usb.png usb/usb.png
fill volume/vol.png volume/vol.png
# Pressed-state images fall back to the normal image.
for need in playing_plane/btn_pause playing_plane/collect_out playing_plane/loop playing_plane/order \
            playing_plane/random playing_plane/single playing_plane/speed stream_media/radio; do
    [ -f "$out/${need}_s.png" ] || { [ -f "$out/$need.png" ] && cp "$out/$need.png" "$out/${need}_s.png"; } || true
done
