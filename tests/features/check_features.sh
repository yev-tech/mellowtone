#!/bin/sh
# check_features.sh PLAYER NM OPTION=0|1 ...
# The host player against the feature options it was configured with: what
# `PLAYER --features` reports to plugins must match the options, and each
# feature's code must be in the binary exactly when its option is on (its
# own functions, its screens, its libraries).
set -eu
player=$1 nm=$2
shift 2

# <option>: <plugin token> <symbol present when on> <symbols gone when off...>
# The "on" symbol is an external function or table, never inlined away; the
# rest are the feature's own functions and screens (static ones included).
markers() {
    case "$1" in
        MT_FEATURE_AIRPLAY) echo "feature.airplay airplay_control_start airplay_control_start airplay_bridge_start \
            airplay_metadata_start build_airplay_overlay_screen populate_airplay_screen" ;;
        MT_FEATURE_DLNA_RENDERER) echo "feature.dlna_renderer dlna_control_start dlna_control_start populate_dlna_screen" ;;
        MT_FEATURE_WIFI_IMPORT) echo "feature.wifi_import import_web_start import_web_start build_import_wifi_screen \
            populate_import_wifi_screen" ;;
        MT_FEATURE_REMOTE) echo "feature.remote remote_control_start remote_control_start bt_remote_control_start \
            remote_control_mdns_start build_remote_control_screen remote_control_refresh_address" ;;
        MT_CODEC_OPUS) echo "feature.codec_opus opus_open_file opus_open_file opus_decode opus_decoder_create" ;;
        MT_CODEC_WMA) echo "feature.codec_wma wma_open_file wma_open_file asf_demux_open" ;;
        MT_I18N) echo "feature.i18n entries_es entries_es entries_pt_BR populate_language_choice_screen" ;;
        *) echo "check_features: unknown option $1" >&2; exit 2 ;;
    esac
}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
"$nm" "$player" | awk '{ print $NF }' | sort -u > "$work/symbols"
"$player" --features | sort > "$work/reported"

failures=0
fail() { echo "FAIL: $*" >&2; failures=$((failures + 1)); }
has_symbol() { grep -qx "$1" "$work/symbols"; }

: > "$work/expected"
for arg in "$@"; do
    opt=${arg%%=*} on=${arg#*=}
    # shellcheck disable=SC2046
    set -- $(markers "$opt")
    token=$1 present=$2
    shift 2
    echo "$token=$on" >> "$work/expected"
    if [ "$on" = 1 ]; then
        has_symbol "$present" || fail "$opt is on but $present is not in the player"
    else
        for s in "$@"; do
            ! has_symbol "$s" || fail "$opt is off but $s is still in the player"
        done
    fi
done

sort -o "$work/expected" "$work/expected"
diff -u "$work/expected" "$work/reported" >&2 || fail "--features does not report what was built"

if [ "$failures" -ne 0 ]; then
    echo "features: $failures failure(s)" >&2
    exit 1
fi
echo "features: ok ($(tr '\n' ' ' < "$work/reported"))"
