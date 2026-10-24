# Host unit tests (preset "host"; ./mt test runs them with ctest).
#
#   musicbrainz        offline MusicBrainz matcher tests on recorded fixtures
#                      (mb_live: live queries, `mb_live --save-fixtures fixtures`
#                      from tests/musicbrainz rewrites the fixtures; 1 req/s)
#   http               http_client and http_stream framing against a local one-shot
#                      server (EOF-delimited bodies, bad chunk lines, long Location,
#                      closing a stalled stream)
#   state              the state root on the card, radio lists, settings reset,
#                      settings writes that fail (full card), card swaps
#   plugins            plugin JSON nesting and error paths (Lua with API checks),
#                      the reserved-path checks (case, symlinks, ancestors) and
#                      plugin data under case-folded ids; ASan/UBSan
#   board_<name>       each board descriptor (boards/<name>/board.c) against its
#                      board_config.h: caps, tokens, screen, paths, features
#   power              battery reader, power events (fake uevents), charge control
#                      (hysteresis, stop/resume, DAC inhibit), the sysfs charger back
#                      end and the input-current policy on a fake sysfs tree, the
#                      AXP2101 back end on a fake I2C bus; ASan/UBSan
#   wifi               Wi-Fi core: wpa_ctrl, wifi_core and the generic
#                      back end against a fake wpa_supplicant control socket
#                      (parsing, update-not-duplicate, no select_network, WRONG_KEY,
#                      SAE, DHCP once, stale socket = off, timeouts), ASan/UBSan
#   bluetooth          Bluetooth back ends on a private dbus-daemon:
#                      stack detection (nothing / a fake BlueZ 4 / dbusmock's
#                      bluez5 template), BlueZ 5 power, devices, signals,
#                      connect/disconnect/forget, inquiry, reconnect, restart;
#                      the moved BlueZ 4 reads; the BlueZ 4 AVRCP player
#                      (RegisterPlayer arguments, signals, Release); ASan/UBSan.
#                      Needs dbus-daemon and python3-dbusmock (mt-build image)
#   bt_codec_choice    the R3 Pro's per-device codec choice: bluetoothd's codec list,
#                      Auto's order, per-device choices, the table file, the
#                      old global setting's migration; ASan/UBSan
#   usb                USB modes: the android_usb and configfs gadget back
#                      ends' write sequences on fake sysfs/configfs trees, the sys_server
#                      storage hand-over against a fake sys_server socket, the busy-card
#                      scan, the lending state machine and the whole hand-over, the
#                      /dev/uac_sa helpers; ASan/UBSan
#   uac_bridge         the USB DAC bridge built as on the device against a fake
#                      /dev/uac_sa (both ABIs, PCM, rate change, DoP, native DSD)
#   usb_dac_rate       the bridge's clock-drift resampler and rate controller on
#                      simulated time: no cuts, bounded ring, ratio settles; ASan/UBSan
#   shell_state        what the shell re-applies after a UI reload: the cached
#                      now-playing card (survives being re-applied through its own
#                      setter, clears, truncates) and the status-bar Bluetooth glyph;
#                      ASan/UBSan
#   launcher           mellowtone-launcher's core with fakes: fallback chain,
#                      boot-loop counter, menu flag, menu by keys/touch on a memory
#                      framebuffer, update packages (refusals, power loss at every
#                      step), rollback, firmware images (low battery, md5, model),
#                      settings reset; ASan/UBSan
#   tagcache           library database engine on a temporary directory: a
#                      written library reopens and answers queries; older or
#                      foreign formats are refused and rebuilt; ASan/UBSan
#   features           the host player against its feature options: --features
#                      reports exactly the options, and each feature's code is
#                      in the binary only when its option is on (builds the
#                      player; the host-minimal preset runs it with all off)
#   tag_writer         tag writer matrix under ASan/UBSan, cross-checked with
#                      mutagen/ffmpeg/flac. Label "root": one case runs the tool
#                      as nobody, so it needs root in the container (./mt test
#                      runs `ctest -L root` through run_as_root).
set(S ${CMAKE_SOURCE_DIR})
set(D ${MT_DEPS_SRC})
add_custom_target(tests)

# --- musicbrainz ----------------------------------------------------------------
set(mb_lib_flags -std=gnu99 -O2 -fwrapv)
set(mb_lib_includes ${D}/mbedtls/include ${D}/mbedtls/library ${MT_NETWORK_INCLUDES} ${S}/src/core ${D}/cJSON)
set(mb_flags -std=gnu99 -O2 -g -Wall -Wextra -fwrapv)
set(mb_includes ${S}/vendor/config ${MT_NETWORK_INCLUDES} ${S}/src/library ${S}/src/core ${D}/cJSON ${D}/mbedtls/include)

file(GLOB mb_mbedtls_sources ${D}/mbedtls/library/*.c)
list(SORT mb_mbedtls_sources)
add_library(mb_mbedtls STATIC ${mb_mbedtls_sources})
target_compile_options(mb_mbedtls PRIVATE ${mb_lib_flags})
target_include_directories(mb_mbedtls PRIVATE ${mb_lib_includes})

add_library(mb_net OBJECT ${S}/src/network/http/http_client.c ${S}/src/network/http/http_conn.c
                          ${S}/src/network/http/ca_bundle.c ${D}/cJSON/cJSON.c)
target_compile_options(mb_net PRIVATE ${mb_lib_flags})
set_source_files_properties(${S}/src/network/http/http_client.c TARGET_DIRECTORY mb_net PROPERTIES COMPILE_OPTIONS -Wall)
target_include_directories(mb_net PRIVATE ${mb_lib_includes})

add_library(mb_core OBJECT ${S}/src/core/branding.c ${S}/src/core/app_version.c
                           ${S}/src/network/online/musicbrainz.c ${S}/src/library/retag_match.c)
target_compile_options(mb_core PRIVATE ${mb_flags})
target_include_directories(mb_core PRIVATE ${mb_includes})

foreach(t test_retag mb_live)
    add_executable(${t} musicbrainz/${t}.c $<TARGET_OBJECTS:mb_core> $<TARGET_OBJECTS:mb_net>)
    target_compile_options(${t} PRIVATE ${mb_flags})
    target_include_directories(${t} PRIVATE ${mb_includes})
    target_link_libraries(${t} PRIVATE mb_mbedtls pthread m)
    add_dependencies(tests ${t})
endforeach()
add_test(NAME musicbrainz COMMAND test_retag fixtures
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/musicbrainz)

# --- http -------------------------------------------------------------------------
add_executable(test_http http/test_http.c ${S}/src/network/http/http_stream.c
    $<TARGET_OBJECTS:mb_core> $<TARGET_OBJECTS:mb_net>)
target_compile_options(test_http PRIVATE ${mb_flags})
target_include_directories(test_http PRIVATE ${mb_includes})
target_link_libraries(test_http PRIVATE mb_mbedtls pthread m)
add_dependencies(tests test_http)
add_test(NAME http COMMAND test_http)
set_tests_properties(http PROPERTIES TIMEOUT 60)

# --- state ------------------------------------------------------------------------
add_executable(test_state state/test_state.c
    ${S}/src/core/state_reset.c ${S}/src/core/storage_paths.c ${S}/src/core/sd_card.c
    ${S}/src/library/radio_list.c ${S}/src/core/settings.c ${S}/src/core/utf8_util.c)
target_compile_options(test_state PRIVATE -std=gnu11 -O2 -g -Wall -Wextra -fwrapv)
target_compile_definitions(test_state PRIVATE HOST_BUILD=1 ${mt_feature_defs})
target_include_directories(test_state PRIVATE ${S}/vendor/config ${S}/src/core ${S}/src/library)
target_link_libraries(test_state PRIVATE pthread)
add_dependencies(tests test_state)
add_test(NAME state COMMAND test_state)

# --- plugins ------------------------------------------------------------------------
set(pl_san -fsanitize=address,undefined)
add_executable(test_plugins plugins/test_plugins.c ${S}/src/plugins/plugin_json.c
    ${S}/src/plugins/plugin_storage.c ${S}/src/core/storage_paths.c ${S}/src/core/sd_card.c
    ${S}/src/core/state_reset.c ${lua_sources} ${D}/cJSON/cJSON.c)
target_compile_options(test_plugins PRIVATE -std=gnu11 -O1 -g -fwrapv ${pl_san})
set_source_files_properties(plugins/test_plugins.c ${S}/src/plugins/plugin_json.c ${S}/src/plugins/plugin_storage.c
    TARGET_DIRECTORY test_plugins PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Werror")
# API checks: a push without stack room fails loudly instead of corrupting the heap.
target_compile_definitions(test_plugins PRIVATE HOST_BUILD=1 BOARD_R1 LUA_USE_APICHECK)
target_include_directories(test_plugins PRIVATE ${S}/src/plugins ${S}/src/core ${MT_HARDWARE_INCLUDES} ${MT_NETWORK_INCLUDES}
    ${D}/lua/src ${D}/cJSON ${D}/mbedtls/include)
target_link_options(test_plugins PRIVATE ${pl_san})
target_link_libraries(test_plugins PRIVATE mb_mbedtls pthread m)
add_dependencies(tests test_plugins)
add_test(NAME plugins COMMAND test_plugins)
set_tests_properties(plugins PROPERTIES TIMEOUT 120)

# --- tagcache -------------------------------------------------------------------------
set(tc_san -fsanitize=address,undefined)
add_executable(test_tagcache tagcache/test_tagcache.c ${S}/src/library/tagcache.c)
target_compile_options(test_tagcache PRIVATE -std=gnu11 -O1 -g -Wall -fwrapv ${tc_san})
set_source_files_properties(tagcache/test_tagcache.c TARGET_DIRECTORY test_tagcache
    PROPERTIES COMPILE_OPTIONS "-Wextra;-Werror")
target_compile_definitions(test_tagcache PRIVATE HOST_BUILD=1 BOARD_R1)
target_include_directories(test_tagcache PRIVATE ${S}/src/library ${S}/src/core ${MT_HARDWARE_INCLUDES} ${MT_NETWORK_INCLUDES})
target_link_options(test_tagcache PRIVATE ${tc_san})
target_link_libraries(test_tagcache PRIVATE pthread)
add_dependencies(tests test_tagcache)
add_test(NAME tagcache COMMAND test_tagcache)
set_tests_properties(tagcache PROPERTIES TIMEOUT 120)

# --- tag_writer ---------------------------------------------------------------------
set(tw_includes ${D} ${S}/vendor/config ${S}/src/audio ${MT_NETWORK_INCLUDES} ${S}/src/library ${MT_HARDWARE_INCLUDES} ${S}/src/ui
    ${S}/src/core ${D}/dr_libs ${S}/vendor/stb_vorbis ${D}/mbedtls/include ${D}/lvgl)
set(tw_defines TAG_WRITER_TEST_HOOKS WITH_METADATA_READER)
set(tw_san -fsanitize=address,undefined)

# The app's reader for cross-checks (warnings from vendored code muted).
add_library(tw_reader OBJECT ${S}/src/library/metadata.c ${S}/src/core/utf8_util.c tag_writer/md_stubs.c)
target_compile_options(tw_reader PRIVATE -O1 -g -w -fwrapv)
target_compile_definitions(tw_reader PRIVATE ${mt_feature_defs})
target_include_directories(tw_reader PRIVATE ${tw_includes})

# The writer itself: project flags plus extra warnings and sanitizers.
add_executable(test_tag_writer ${S}/src/library/tag_writer.c tag_writer/test_tag_writer.c
    $<TARGET_OBJECTS:tw_reader>)
target_compile_options(test_tag_writer PRIVATE -std=gnu99 -O1 -g -Wall -fwrapv ${tw_san})
set_source_files_properties(${S}/src/library/tag_writer.c TARGET_DIRECTORY test_tag_writer
    PROPERTIES COMPILE_OPTIONS -Wextra)
target_compile_definitions(test_tag_writer PRIVATE ${tw_defines})
target_include_directories(test_tag_writer PRIVATE ${tw_includes})
target_link_options(test_tag_writer PRIVATE ${tw_san})
target_link_libraries(test_tag_writer PRIVATE m pthread)

# Same without sanitizers (timing / memory figures for the large-file case);
# run_tests.py picks it up next to test_tag_writer.
add_executable(test_tag_writer_plain ${S}/src/library/tag_writer.c tag_writer/test_tag_writer.c
    $<TARGET_OBJECTS:tw_reader>)
target_compile_options(test_tag_writer_plain PRIVATE -std=gnu99 -O2 -g -Wall -fwrapv)
target_compile_definitions(test_tag_writer_plain PRIVATE ${tw_defines})
target_include_directories(test_tag_writer_plain PRIVATE ${tw_includes})
target_link_libraries(test_tag_writer_plain PRIVATE m pthread)
add_dependencies(tests test_tag_writer test_tag_writer_plain)

set(tw_work /tmp/mt-tag_writer-test)
add_test(NAME tag_writer
    COMMAND sh -c "rm -rf \"$1\" && mkdir -p \"$1\" && exec python3 \"$2\" \"$3\" \"$1\""
            sh ${tw_work} ${CMAKE_CURRENT_SOURCE_DIR}/tag_writer/run_tests.py $<TARGET_FILE:test_tag_writer>)
set_tests_properties(tag_writer PROPERTIES LABELS root)

# --- features ---------------------------------------------------------------------
add_dependencies(tests player)
add_test(NAME features COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/features/check_features.sh
    $<TARGET_FILE:player> ${CMAKE_NM} ${mt_feature_defs})
set_tests_properties(features PROPERTIES TIMEOUT 60)

# --- board ------------------------------------------------------------------------
# Every board's descriptor (boards/<name>/board.c), compiled with that board's
# BOARD_* define and feature presets next to the matching board_config.h:
# one test per board (board_r1, board_r3pro, ...).
function(mt_board_test board)
    include(${S}/boards/${board}/board.cmake)
    mt_feature_defines(feature_defs PRESETS)
    set(t test_board_${board})
    add_executable(${t} board/test_board.c board/charger_stubs.c ${S}/boards/${board}/board.c
        ${S}/src/core/board.c ${S}/src/core/mt_features.c ${S}/src/launcher/hooks/hooks_common.c
        ${S}/src/launcher/hooks/launcher_hiby_x1000.c ${S}/src/launcher/hooks/launcher_x1600.c)
    target_compile_options(${t} PRIVATE -std=gnu11 -O1 -g -Wall -Wextra -Werror)
    target_compile_definitions(${t} PRIVATE ${MT_BOARD_DEFINE} ${feature_defs}
        "EXPECTED_BOARD_NAME=\"${board}\"")
    target_include_directories(${t} PRIVATE ${S}/src/core ${MT_HARDWARE_INCLUDES} ${MT_NETWORK_INCLUDES})
    target_link_libraries(${t} PRIVATE pthread)
    add_dependencies(tests ${t})
    add_test(NAME board_${board} COMMAND ${t})
endfunction()
file(GLOB mt_board_dirs LIST_DIRECTORIES true RELATIVE ${S}/boards ${S}/boards/*)
list(SORT mt_board_dirs)
foreach(b IN LISTS mt_board_dirs)
    if(EXISTS ${S}/boards/${b}/board.cmake)
        mt_board_test(${b})
    endif()
endforeach()

# --- wifi -------------------------------------------------------------------------
set(wifi_san -fsanitize=address,undefined)
add_executable(test_wifi wifi/test_wifi.c
    ${S}/src/network/wifi/wpa_ctrl.c ${S}/src/network/wifi/wifi_core.c
    ${S}/src/network/wifi/wifi_backend_wpa.c ${S}/src/core/subprocess.c)
target_compile_options(test_wifi PRIVATE -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wundef -fwrapv ${wifi_san})
# The fake supplicant copies command arguments into fixed fields on purpose.
set_source_files_properties(wifi/test_wifi.c TARGET_DIRECTORY test_wifi PROPERTIES COMPILE_OPTIONS -Wno-format-truncation)
target_compile_definitions(test_wifi PRIVATE HOST_BUILD=1)
target_include_directories(test_wifi PRIVATE ${MT_NETWORK_INCLUDES} ${S}/src/core
    ${D}/mbedtls/include)
target_link_options(test_wifi PRIVATE ${wifi_san})
target_link_libraries(test_wifi PRIVATE mb_mbedtls pthread)
add_dependencies(tests test_wifi)
add_test(NAME wifi COMMAND test_wifi)
set_tests_properties(wifi PROPERTIES TIMEOUT 120)

# --- power ------------------------------------------------------------------------
# Built like a device board (BOARD_R1: board_config.h paths), without HOST_BUILD
# so the real external-power logic runs against the fake tree.
set(power_san -fsanitize=address,undefined)
add_executable(test_power power/test_power.c
    ${S}/src/hardware/power/power_sysfs.c ${S}/src/hardware/power/power_events.c ${S}/src/hardware/power/battery.c
    ${S}/src/core/loop_wake.c
    ${S}/src/hardware/power/charger.c ${S}/src/hardware/power/charger_axp2101_i2c.c ${S}/src/hardware/power/charger_sysfs.c
    ${S}/src/hardware/power/charge_control.c ${S}/src/core/board.c)
target_compile_options(test_power PRIVATE -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wundef -fwrapv ${power_san})
target_compile_definitions(test_power PRIVATE BOARD_R1)
target_include_directories(test_power PRIVATE ${MT_HARDWARE_INCLUDES} ${S}/src/core)
target_link_options(test_power PRIVATE ${power_san})
target_link_libraries(test_power PRIVATE pthread)
add_dependencies(tests test_power)
add_test(NAME power COMMAND test_power)
set_tests_properties(power PROPERTIES TIMEOUT 60)

# --- bluetooth ----------------------------------------------------------------------
# HOST_BUILD: no board platform (the stack's own Powered), as in the simulator.
find_package(PkgConfig REQUIRED)
pkg_check_modules(TEST_DBUS REQUIRED IMPORTED_TARGET dbus-1)
set(bt_san -fsanitize=address,undefined)
set(bt_dir ${S}/src/network/bluetooth)
add_executable(test_bluetooth bluetooth/test_bluetooth.c
    ${bt_dir}/bt_dbus.c ${bt_dir}/bt_stack.c ${bt_dir}/bt_stack_bluez4.c ${bt_dir}/bt_stack_bluez5.c
    ${bt_dir}/bt_platform_hiby.c ${bt_dir}/bt_platform_scripts.c ${bt_dir}/bt_audio_bluealsa.c
    ${bt_dir}/bt_audio_hiby.c ${bt_dir}/bt_avrcp_bluez4.c ${bt_dir}/bluetooth_control.c ${bt_dir}/bt_codec_choice.c
    ${S}/src/core/subprocess.c ${D}/cJSON/cJSON.c)
target_compile_options(test_bluetooth PRIVATE -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wundef -fwrapv ${bt_san})
target_compile_definitions(test_bluetooth PRIVATE HOST_BUILD=1 BOARD_R1)
target_include_directories(test_bluetooth PRIVATE ${MT_NETWORK_INCLUDES} ${S}/src/core ${S}/src/audio
    ${D}/cJSON)
target_link_options(test_bluetooth PRIVATE ${bt_san})
target_link_libraries(test_bluetooth PRIVATE PkgConfig::TEST_DBUS pthread)
add_dependencies(tests test_bluetooth)
add_test(NAME bluetooth COMMAND test_bluetooth ${CMAKE_CURRENT_SOURCE_DIR}/bluetooth/bluez4_mock.py)
set_tests_properties(bluetooth PROPERTIES TIMEOUT 120)

# --- bt_codec_choice ------------------------------------------------------------------
# The per-device codec choice (pure logic, no bus).
add_executable(test_bt_codec_choice bluetooth/test_bt_codec_choice.c ${bt_dir}/bt_codec_choice.c)
target_compile_options(test_bt_codec_choice PRIVATE -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wundef -fwrapv ${bt_san})
target_include_directories(test_bt_codec_choice PRIVATE ${MT_NETWORK_INCLUDES})
target_link_options(test_bt_codec_choice PRIVATE ${bt_san})
add_dependencies(tests test_bt_codec_choice)
add_test(NAME bt_codec_choice COMMAND test_bt_codec_choice)
set_tests_properties(bt_codec_choice PROPERTIES TIMEOUT 30)

# --- usb ----------------------------------------------------------------------------
# HOST_BUILD: storage_paths.c's card is ./music (the test chdirs into its temp tree).
set(usb_san -fsanitize=address,undefined)
set(usb_flags -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wundef -fwrapv)
add_executable(test_usb usb/test_usb.c
    ${S}/src/hardware/usb/uac_sa.c ${S}/src/hardware/usb/usb_gadget.c ${S}/src/hardware/usb/usb_gadget_configfs.c
    ${S}/src/hardware/usb/usb_gadget_android_usb.c ${S}/src/hardware/usb/usb_storage_export.c
    ${S}/src/hardware/usb/usb_storage_lend.c ${S}/src/hardware/usb/usb_connection.c
    ${S}/src/network/services/hiby_sys_client.c ${S}/src/network/services/hiby_sys_server.c
    ${S}/src/core/storage_paths.c ${S}/src/core/sd_card.c ${S}/src/core/state_reset.c)
target_compile_options(test_usb PRIVATE ${usb_flags} ${usb_san})
# The fakes build paths from fixed-size buffers on purpose.
set_source_files_properties(usb/test_usb.c TARGET_DIRECTORY test_usb PROPERTIES COMPILE_OPTIONS -Wno-format-truncation)
target_compile_definitions(test_usb PRIVATE HOST_BUILD=1 BOARD_R1)
target_include_directories(test_usb PRIVATE ${S}/vendor/config ${MT_HARDWARE_INCLUDES} ${S}/src/core ${MT_NETWORK_INCLUDES}
    ${S}/src/library ${D}/mbedtls/include)
target_link_options(test_usb PRIVATE ${usb_san})
target_link_libraries(test_usb PRIVATE pthread)
add_dependencies(tests test_usb)
add_test(NAME usb COMMAND test_usb)
set_tests_properties(usb PROPERTIES TIMEOUT 180)

# The bridge as on the device (no HOST_BUILD), with a recording audio_output.
add_executable(test_uac_bridge usb/test_uac_bridge.c ${S}/src/hardware/usb/usb_dac_bridge.c ${S}/src/hardware/usb/uac_sa.c
    ${S}/src/hardware/usb/usb_dac_rate.c)
target_compile_options(test_uac_bridge PRIVATE ${usb_flags} ${usb_san})
target_compile_definitions(test_uac_bridge PRIVATE BOARD_R1)
target_include_directories(test_uac_bridge PRIVATE ${MT_HARDWARE_INCLUDES} ${S}/src/core ${S}/src/audio)
target_link_options(test_uac_bridge PRIVATE ${usb_san})
target_link_libraries(test_uac_bridge PRIVATE pthread m)
add_dependencies(tests test_uac_bridge)
add_test(NAME uac_bridge COMMAND test_uac_bridge)
set_tests_properties(uac_bridge PROPERTIES TIMEOUT 120)

# The bridge's drift handling (resampler and rate controller) on simulated time.
add_executable(test_usb_dac_rate usb/test_usb_dac_rate.c ${S}/src/hardware/usb/usb_dac_rate.c)
target_compile_options(test_usb_dac_rate PRIVATE ${usb_flags} ${usb_san})
target_include_directories(test_usb_dac_rate PRIVATE ${MT_HARDWARE_INCLUDES})
target_link_options(test_usb_dac_rate PRIVATE ${usb_san})
target_link_libraries(test_usb_dac_rate PRIVATE m)
add_dependencies(tests test_usb_dac_rate)
add_test(NAME usb_dac_rate COMMAND test_usb_dac_rate)
set_tests_properties(usb_dac_rate PROPERTIES TIMEOUT 120)

# --- shell state ----------------------------------------------------------------------
set(shell_san -fsanitize=address,undefined)
add_executable(test_shell_state shell/test_shell_state.c ${S}/src/ui/shell_state.c)
target_compile_options(test_shell_state PRIVATE -std=gnu11 -O1 -g -Wall -Wextra -Werror -fwrapv ${shell_san})
target_include_directories(test_shell_state PRIVATE ${S}/src/ui)
target_link_options(test_shell_state PRIVATE ${shell_san})
add_dependencies(tests test_shell_state)
add_test(NAME shell_state COMMAND test_shell_state)

# --- launcher -----------------------------------------------------------------------
# The launcher core (MT_LAUNCHER_CORE_SOURCES without the board hooks) with fake
# processes, clock, keys and battery, temporary directories and a memory screen.
set(lx_san -fsanitize=address,undefined)
set(lx_core ${MT_LAUNCHER_CORE_SOURCES})
list(FILTER lx_core EXCLUDE REGEX "/hooks/")
list(TRANSFORM lx_core PREPEND ${S}/)
add_executable(test_launcher launcher/test_launcher.c ${lx_core}
    ${D}/lvgl/src/libs/tjpgd/tjpgd.c
    ${D}/mbedtls/library/md5.c ${D}/mbedtls/library/sha256.c ${D}/mbedtls/library/platform_util.c)
target_compile_options(test_launcher PRIVATE -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wundef -fwrapv ${lx_san})
# Vendored code: no -Werror / -Wundef.
set_source_files_properties(${D}/lvgl/src/libs/tjpgd/tjpgd.c ${D}/mbedtls/library/md5.c
    ${D}/mbedtls/library/sha256.c ${D}/mbedtls/library/platform_util.c
    TARGET_DIRECTORY test_launcher PROPERTIES COMPILE_OPTIONS "-Wno-error;-Wno-undef")
set_source_files_properties(launcher/test_launcher.c TARGET_DIRECTORY test_launcher PROPERTIES
    COMPILE_OPTIONS -Wno-format-truncation)
target_compile_definitions(test_launcher PRIVATE LV_CONF_INCLUDE_SIMPLE=1 BOARD_R1)
target_include_directories(test_launcher PRIVATE ${D} ${S}/vendor/config ${S}/src/launcher ${S}/src/core ${D}/mbedtls/include)
target_link_options(test_launcher PRIVATE ${lx_san})
target_link_libraries(test_launcher PRIVATE pthread)
add_dependencies(tests test_launcher)
add_test(NAME launcher COMMAND test_launcher)
set_tests_properties(launcher PROPERTIES TIMEOUT 120)
# tools/release/make_update_package.sh's output, installed by the launcher's reader.
add_test(NAME launcher_package_script
    COMMAND sh -c "set -e; d=$(mktemp -d); trap 'rm -rf \"$d\"' EXIT; mkdir -p \"$d/slot/fonts\"; \
printf '#!/bin/sh\\n' > \"$d/slot/mellowtone\"; chmod 755 \"$d/slot/mellowtone\"; echo font > \"$d/slot/fonts/a.ttf\"; \
\"$1\" r3pro 0.2.0 \"$d/slot\" \"$d/sd/.mellowtone/update\" >/dev/null; \"$2\" --check-package \"$d\""
        sh ${S}/tools/release/make_update_package.sh $<TARGET_FILE:test_launcher>)
set_tests_properties(launcher_package_script PROPERTIES TIMEOUT 60)
