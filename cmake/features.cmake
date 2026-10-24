# Optional software features. Each is a CMake option whose
# default is the board's preset: boards/<name>/board.cmake may set
# MT_PRESET_<option> (ON/OFF); otherwise the option is ON. The player sees
# each as a 0/1 define, read through MT_HAS_FEATURE() in
# src/core/mt_features.h.
#
# An option that is OFF builds the player without the feature: its sources
# (MT_FEATURE_SOURCES_<option>) and libraries are left out, its headers turn
# into inert inline stubs, its screens, rows and toggles are not built, and
# plugins are told it is absent. Hardware capabilities (board_has()) are a
# separate matter and do not depend on these.
#
# The host-minimal preset (CMakePresets.json) builds the host simulator with
# every option OFF; `./mt build host-minimal` and `./mt test host-minimal`
# check that it still builds and passes the tests.
set(MT_FEATURE_OPTIONS
    MT_FEATURE_AIRPLAY        # AirPlay receiver (needs CAP_WIFI)
    MT_FEATURE_DLNA_RENDERER  # DLNA renderer (needs CAP_WIFI)
    MT_FEATURE_WIFI_IMPORT    # Wi-Fi Import web upload (needs CAP_WIFI)
    MT_FEATURE_REMOTE         # Remote Control: phone web remote and its Bluetooth
                              # transport (needs CAP_WIFI or CAP_BT_AUDIO_OUT)
    MT_CODEC_OPUS             # Opus decoder and libopus
    MT_CODEC_WMA              # WMA / ASF decoder
    MT_I18N)                  # translations (OFF: English only)

# Player sources each option brings (all of them are listed in
# src/sources.cmake), and the ones that replace them when it is OFF.
set(MT_FEATURE_SOURCES_MT_FEATURE_AIRPLAY
    src/network/services/airplay_control.c src/network/services/airplay_bridge.c src/network/services/airplay_metadata.c
    src/ui/gui_airplay.c)
set(MT_FEATURE_SOURCES_MT_FEATURE_DLNA_RENDERER src/network/services/dlna_control.c src/ui/gui_dlna.c)
set(MT_FEATURE_SOURCES_MT_FEATURE_WIFI_IMPORT src/network/services/import_web.c src/ui/gui_wifi_import.c)
set(MT_FEATURE_SOURCES_MT_FEATURE_REMOTE
    src/network/services/remote_control.c src/network/services/remote_control_mdns.c src/network/bluetooth/bt_remote_control.c
    src/ui/gui_remote.c)
set(MT_FEATURE_SOURCES_MT_CODEC_OPUS src/audio/ogg_demux.c src/audio/opus_decoder.c)
set(MT_FEATURE_SOURCES_MT_CODEC_WMA src/audio/asf_demux.c src/audio/wma_decoder.c)
set(MT_FEATURE_SOURCES_MT_I18N src/ui/i18n_catalog.c)
set(MT_FEATURE_OFF_SOURCES_MT_I18N src/ui/i18n_catalog_none.c)

# mt_feature_preset(<option> <out>): the board preset for <option> (from the
# MT_PRESET_* variables in scope), ON when the board sets none.
function(mt_feature_preset opt out)
    if(DEFINED MT_PRESET_${opt})
        set(v ${MT_PRESET_${opt}})
    else()
        set(v ON)
    endif()
    set(${out} ${v} PARENT_SCOPE)
endfunction()

# The options themselves, defaulted from this board's presets (a preset or
# -D on the command line overrides them).
foreach(opt IN LISTS MT_FEATURE_OPTIONS)
    mt_feature_preset(${opt} preset)
    option(${opt} "Optional feature (default: the ${MT_BOARD} preset)" ${preset})
endforeach()

# mt_feature_defines(<out> [PRESETS]): "<option>=0|1" for every option, from
# the options' values, or with PRESETS from the board presets in scope (for
# tests that compile another board's board.c).
function(mt_feature_defines out)
    set(defs)
    foreach(opt IN LISTS MT_FEATURE_OPTIONS)
        if("PRESETS" IN_LIST ARGN)
            mt_feature_preset(${opt} v)
        else()
            set(v ${${opt}})
        endif()
        if(v)
            list(APPEND defs ${opt}=1)
        else()
            list(APPEND defs ${opt}=0)
        endif()
    endforeach()
    set(${out} ${defs} PARENT_SCOPE)
endfunction()

# mt_feature_sources(<list var>): drops from the source list the sources of
# every option that is OFF, putting its MT_FEATURE_OFF_SOURCES_* (if any) in
# their place.
function(mt_feature_sources var)
    set(result)
    foreach(src IN LISTS ${var})
        set(keep ON)
        foreach(opt IN LISTS MT_FEATURE_OPTIONS)
            if(NOT ${opt} AND src IN_LIST MT_FEATURE_SOURCES_${opt})
                set(keep OFF)
                list(FIND MT_FEATURE_SOURCES_${opt} ${src} index)
                if(index EQUAL 0)
                    list(APPEND result ${MT_FEATURE_OFF_SOURCES_${opt}})
                endif()
            endif()
        endforeach()
        if(keep)
            list(APPEND result ${src})
        endif()
    endforeach()
    set(${var} ${result} PARENT_SCOPE)
endfunction()
