# HiBy R1 (Ingenic X1600). Also the board the host simulator models.
set(MT_BOARD_DEFINE BOARD_R1)
set(MT_TARGET_PLAYER_NAME mellowtone-r1)
set(MT_HOST_PLAYER_NAME mellowtone-host)
set(MT_LVGL_BOARD_PATCHES)
set(MT_BOARD_SOURCES_REMOVE)
set(MT_BOARD_SOURCES_ADD boards/r1/board.c)
# Optional features: the CMakeLists.txt defaults (all ON) apply.
