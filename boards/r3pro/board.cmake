# HiBy R3 Pro (Ingenic X1000), stock HiBy base system.
set(MT_BOARD_DEFINE BOARD_R3PRO)
set(MT_TARGET_PLAYER_NAME mellowtone-r3pro)
set(MT_HOST_PLAYER_NAME mellowtone-host)
# Smart-LCD panel: pan after every frame (only compiled in where lv_conf.h
# sets LV_LINUX_FBDEV_PAN_EACH_FRAME, i.e. BOARD_R3PRO).
set(MT_LVGL_BOARD_PATCHES ${CMAKE_SOURCE_DIR}/patches/lvgl_r3pro_slcd_pan.patch)
# Bluetooth (BlueZ 4 + HiBy's ALSA plugin, BOARD.bt_platform) and Wi-Fi
# (BOARD.wifi.backend) are chosen at run time like everything else that
# differs: run-time data in board.c.
set(MT_BOARD_SOURCES_REMOVE)
set(MT_BOARD_SOURCES_ADD boards/r3pro/board.c)
# Optional features. Mellowtone's AirPlay (shairport), DLNA
# renderer (dmrd) and Wi-Fi Import (thttpd) helpers are not built for this
# board, so the player is built without those features.
set(MT_PRESET_MT_FEATURE_AIRPLAY OFF)
set(MT_PRESET_MT_FEATURE_DLNA_RENDERER OFF)
set(MT_PRESET_MT_FEATURE_WIFI_IMPORT OFF)
