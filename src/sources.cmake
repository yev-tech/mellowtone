# Player sources shared by every board and the host simulator, in the link
# order the Makefile used (board.cmake can still add or remove some, see
# boards/).
set(MT_APP_SOURCES
    src/main.c
    src/ui/gui.c
    src/ui/gui_subsonic.c
    src/ui/gui_settings.c
    src/ui/gui_network.c
    src/ui/gui_wifi.c
    src/ui/gui_bluetooth.c
    src/ui/gui_usb.c
    src/ui/gui_airplay.c
    src/ui/gui_dlna.c
    src/ui/gui_remote.c
    src/ui/gui_wifi_import.c
    src/ui/gui_settings_playback.c
    src/ui/gui_theme.c
    src/ui/gui_notifications.c
    src/ui/gui_library.c
    src/ui/gui_queue.c
    src/ui/gui_player.c
    src/ui/gui_plugins.c
    src/ui/gui_shell.c
    src/ui/gui_navigation.c
    src/ui/gui_radio.c
    src/ui/gui_home_clock.c
    src/ui/gui_now_panel.c
    src/ui/gui_retag.c
    src/ui/gui_stream_home.c
    src/ui/gui_text_input.c
    src/ui/gui_lyrics.c
    src/ui/gui_reload.c
    src/audio/audio.c
    src/library/file_browser.c
    src/hardware/input/hw_buttons.c
    src/hardware/input/input_device_utils.c
    src/library/metadata.c
    src/library/metadata_db.c
    src/core/settings.c
    src/core/app_version.c
    src/core/branding.c
    src/audio/aiff_decoder.c
    src/audio/dsd_filter.c
    src/audio/dsd_decoder.c
    src/audio/aac_decoder.c
    src/audio/mp4_demux.c
    src/audio/ape_demux.c
    src/audio/ape_decoder.c
    src/audio/peq.c
    src/audio/eq_presets.c
    src/ui/assets.c
    src/ui/screen_builders.c
    src/hardware/power/battery.c
    src/network/http/ca_bundle.c
    src/network/http/http_conn.c
    src/network/http/http_client.c
    src/network/online/lrclib.c
    src/network/online/musicbrainz.c
    src/network/services/dlna_client.c
    src/library/retag_match.c
    src/library/tag_writer.c
    src/network/http/http_stream.c
    src/network/online/subsonic_client.c
    src/library/cover_decode.c
    src/library/image_thumb.c
    src/library/lyrics.c
    src/audio/asf_demux.c
    src/audio/wma_decoder.c
    src/audio/ogg_probe.c
    src/audio/ogg_demux.c
    src/audio/opus_decoder.c
    src/audio/vorbis_decoder.c
    src/library/cue_parser.c
    src/ui/fallback_font.c
    src/ui/gui_text_view.c
    src/core/subprocess.c
    # Wi-Fi: every board builds both back ends; BOARD.wifi.backend picks one.
    src/network/wifi/wpa_ctrl.c
    src/network/wifi/wifi_core.c
    src/network/wifi/wifi_backend_wpa.c
    src/network/wifi/wifi_backend_hiby.c
    src/network/wifi/wifi_control.c
    # Bluetooth: every board builds every back end; the stack and
    # the audio transport are detected at run time, BOARD.bt_platform picks
    # the platform glue.
    src/network/bluetooth/bt_dbus.c
    src/network/bluetooth/bt_stack.c
    src/network/bluetooth/bt_stack_bluez4.c
    src/network/bluetooth/bt_stack_bluez5.c
    src/network/bluetooth/bt_platform_hiby.c
    src/network/bluetooth/bt_codec_choice.c
    src/network/bluetooth/bt_platform_scripts.c
    src/network/bluetooth/bt_audio_bluealsa.c
    src/network/bluetooth/bt_audio_hiby.c
    src/network/bluetooth/bt_avrcp_bluez4.c
    src/network/bluetooth/bluetooth_control.c
    src/network/bluetooth/bt_media_player.c
    src/network/bluetooth/bt_remote_control.c
    src/network/services/hiby_sys_server.c
    src/hardware/power/backlight.c
    src/network/services/import_web.c
    src/network/services/airplay_control.c
    src/network/services/airplay_bridge.c
    src/network/services/airplay_metadata.c
    src/hardware/input/headphone_status.c
    src/hardware/device_config.c
    src/hardware/led_control.c
    # Battery and charger: one reader, power events, the charge
    # control logic and both charger back ends (BOARD.charger picks one).
    src/hardware/power/power_sysfs.c
    src/hardware/power/power_events.c
    src/hardware/power/charger.c
    src/hardware/power/charger_axp2101_i2c.c
    src/hardware/power/charger_sysfs.c
    src/hardware/power/charge_control.c
    src/core/idle_shutdown.c
    src/hardware/power/power_suspend.c
    # USB modes: every board builds every back end; BOARD.usb_gadget
    # and BOARD.storage_export pick them.
    src/hardware/usb/usb_mode_control.c
    src/hardware/usb/usb_connection.c
    src/hardware/usb/usb_gadget.c
    src/hardware/usb/usb_gadget_configfs.c
    src/hardware/usb/usb_gadget_android_usb.c
    src/hardware/usb/usb_storage_export.c
    src/hardware/usb/usb_storage_lend.c
    src/hardware/usb/uac_sa.c
    src/hardware/usb/usb_dac_bridge.c
    src/hardware/usb/usb_dac_rate.c
    src/hardware/usb/usb_audio_output.c
    src/library/playlist_files.c
    src/library/favorite_writer.c
    src/core/timezone_data.c
    src/core/timezone_apply.c
    src/core/hostname_apply.c
    src/network/services/dlna_control.c
    src/network/services/remote_control.c
    src/network/online/catalog_source_cache.c
    src/network/services/remote_control_mdns.c
    src/plugins/plugin_manager.c
    src/ui/gui_setup.c
    src/core/timezone_location.c
    src/ui/lyrics_layout.c
    src/ui/transition_compositor.c
    src/ui/image_filters.c
    src/ui/hw_volume_coalesce.c
    src/ui/shell_state.c
    src/core/sd_card.c
    src/core/storage_paths.c
    src/library/radio_list.c
    src/plugins/plugin_json.c
    src/plugins/plugin_storage.c
    src/plugins/plugin_disabled_list.c
    src/ui/gui_plugin_manage.c
    src/library/remote_track.c
    src/library/queue_resume.c
    src/audio/track_probe.c
    src/library/albumart.c
    src/library/tagcache.c
    src/library/path_cache.c
    src/library/remote_state.c
    src/library/subsonic_saved_servers.c
    src/library/artwork_coordinator.c
    src/core/utf8_util.c
    src/core/app_clock.c
    src/core/loop_wake.c
    src/core/db_log.c
    src/ui/gesture_detector.c
    src/ui/i18n.c
    src/ui/i18n_catalog.c
    src/ui/player_layouts.c
    src/network/bluetooth/bluetooth_reconnect.c
    src/core/board.c
    src/core/mt_features.c
    # Stock-HiBy-system glue, chosen at run time by the board descriptor
    # (sys_server, stock gain file, POSIX time zones).
    src/network/services/hiby_sys_client.c
    src/hardware/r3pro_stock_ini.c
    src/core/timezone_posix.c
    # The player's side of mellowtone-launcher and the board hooks
    # BOARD.launcher points at (libc only).
    src/core/launcher_client.c
    src/core/state_reset.c
    src/launcher/hooks/hooks_common.c
    src/launcher/hooks/launcher_hiby_x1000.c
    src/launcher/hooks/launcher_x1600.c
)

set(MT_APP_CXX_SOURCES
    src/audio/alac_decoder.cpp)

# Target only: it talks to the real audio devices.
set(MT_TARGET_ONLY_APP_SOURCES
    src/audio/audio_output.c)

# mellowtone-launcher, every board: its own small static binary,
# never linking LVGL or the player. The board's descriptor (board.cmake's
# MT_BOARD_SOURCES_ADD) and src/core/board.c come on top; tjpgd.c (boot
# splash) from the patched LVGL tree; md5/sha256 from mbedTLS.
set(MT_LAUNCHER_CORE_SOURCES
    src/launcher/lx_util.c
    src/launcher/lx_boot.c
    src/launcher/lx_menu.c
    src/launcher/lx_slots.c
    src/launcher/lx_pkg.c
    src/launcher/lx_fw.c
    src/launcher/lx_fb.c
    src/core/state_reset.c
    src/launcher/hooks/hooks_common.c
    src/launcher/hooks/launcher_hiby_x1000.c
    src/launcher/hooks/launcher_x1600.c)
set(MT_LAUNCHER_SOURCES
    ${MT_LAUNCHER_CORE_SOURCES}
    src/launcher/lx_main.c
    src/launcher/lx_input.c
    src/launcher/lx_board_stubs.c
    src/core/board.c)

# Include directories of src/network and src/hardware: one per subfolder, so
# `#include "http_client.h"` works from anywhere in the player, the launcher
# and the tests.
set(MT_NETWORK_INCLUDES
    ${CMAKE_SOURCE_DIR}/src/network/http
    ${CMAKE_SOURCE_DIR}/src/network/wifi
    ${CMAKE_SOURCE_DIR}/src/network/bluetooth
    ${CMAKE_SOURCE_DIR}/src/network/services
    ${CMAKE_SOURCE_DIR}/src/network/online)
set(MT_HARDWARE_INCLUDES
    ${CMAKE_SOURCE_DIR}/src/hardware
    ${CMAKE_SOURCE_DIR}/src/hardware/power
    ${CMAKE_SOURCE_DIR}/src/hardware/usb
    ${CMAKE_SOURCE_DIR}/src/hardware/input)
