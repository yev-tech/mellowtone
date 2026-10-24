# Mellowtone: the player for HiBy R1 / R3 Pro / R3 Pro II / R3 II (2025), and
# a host simulator (SDL2).
#
#   cmake --preset <r1|r3pro|r3proii|r3ii_2025|host> && cmake --build --preset <same>
#   (or ./mt build <board>|host, which runs exactly that in the mt-build container)
#
# Board specifics live in boards/<name>/board.cmake, dependencies (pinned
# archives + patches) in cmake/dependencies.cmake. Flags reproduce the old
# Makefile's exactly, per group of sources; see the group comments below.
cmake_minimum_required(VERSION 3.28)

set(MT_BOARD "" CACHE STRING "Board: r1, r3pro, r3proii or r3ii_2025")
option(MT_HOST "Build the SDL host simulator instead of the device player" OFF)
if(NOT MT_BOARD)
    message(FATAL_ERROR "Set MT_BOARD (use a preset: cmake --preset r1|r3pro|r3proii|r3ii_2025|host)")
endif()
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/boards/${MT_BOARD}/board.cmake")
    message(FATAL_ERROR "Unknown board '${MT_BOARD}' (no boards/${MT_BOARD}/board.cmake)")
endif()

project(mellowtone LANGUAGES C CXX)

if(CMAKE_BUILD_TYPE)
    message(FATAL_ERROR "CMAKE_BUILD_TYPE must stay empty: the optimisation flags are set per source group")
endif()

include(boards/${MT_BOARD}/board.cmake)
include(cmake/features.cmake)
include(cmake/dependencies.cmake)
include(src/sources.cmake)

# --- Optional build identity (device builds) / UI diagnostics (all) ---------
set(RELEASE_LABEL "" CACHE STRING "Human-facing release label shown in About (APP_RELEASE_LABEL)")
set(TEST_BUILD_TAG "" CACHE STRING "Conspicuous test-build identity (TEST_BUILD_TAG)")
option(UI_PERF_TRACE "UI timing diagnostics" OFF)
option(UI_GESTURE_TRACE "Gesture diagnostics" OFF)
option(UI_HITBOX_DEBUG "Outline the transport-row hit areas" OFF)

# BUILD_STAMP: the fixed-width build identifier (About, the launcher's --version).
# Configure time; honours SOURCE_DATE_EPOCH (then rendered in UTC). ./mt
# re-runs the configure step on every build, so the stamp stays current and
# only app_version.c, the one file that reads it, is recompiled.
if(DEFINED ENV{SOURCE_DATE_EPOCH})
    string(TIMESTAMP MT_BUILD_STAMP "%Y-%m-%d_%H:%M" UTC)
else()
    string(TIMESTAMP MT_BUILD_STAMP "%Y-%m-%d_%H:%M")
endif()

set(S ${CMAKE_SOURCE_DIR})
set(D ${MT_DEPS_SRC})

# --- Reproducible paths -------------------------------------------------------
# __FILE__ (assert(), mbedTLS/dbus diagnostics) and debug info name sources
# relative to the checkout, exactly as the Makefile's relative paths did, so
# the binary does not depend on where the tree or the build directory is.
# GCC applies the last matching map, so the dependency map wins for _deps.
add_library(mt_paths INTERFACE)
target_compile_options(mt_paths INTERFACE
    -ffile-prefix-map=${S}/=
    -ffile-prefix-map=${D}/=)

# --- Player flags (Makefile CFLAGS + TARGET_CFLAGS / HOST_CFLAGS) -----------
# ${D} comes first so `#include "lvgl/lvgl.h"` resolves to the pinned, patched
# tree and never to a stale clone left in the source directory by the old
# Makefile.
add_library(mt_player_flags INTERFACE)
target_link_libraries(mt_player_flags INTERFACE mt_paths)
target_include_directories(mt_player_flags INTERFACE
    ${D} ${S}/vendor/config
    ${S}/src/audio ${MT_NETWORK_INCLUDES} ${S}/src/library ${MT_HARDWARE_INCLUDES} ${S}/src/ui ${S}/src/core ${S}/src/plugins
    ${D}/lvgl ${D}/dr_libs ${D}/faad2/include ${D}/alac/codec ${D}/mbedtls/include ${D}/cJSON
    $<$<BOOL:${MT_CODEC_OPUS}>:${D}/opus/include> ${D}/lua/src ${S}/vendor/stb_vorbis ${S}/vendor/config/jpeg_vendor_config ${D}/jpeg)
target_compile_options(mt_player_flags INTERFACE
    -O3 -g $<$<COMPILE_LANGUAGE:C>:-Wall> -fwrapv $<$<COMPILE_LANGUAGE:CXX>:-std=c++11>)
target_compile_definitions(mt_player_flags INTERFACE LV_CONF_INCLUDE_SIMPLE=1 ${MT_BOARD_DEFINE})

# --- Optional features (cmake/features.cmake) ---------------------------------
mt_feature_defines(mt_feature_defs)
target_compile_definitions(mt_player_flags INTERFACE ${mt_feature_defs})
message(STATUS "Features for ${MT_BOARD}: ${mt_feature_defs}")
if(MT_HOST)
    find_program(SDL2_CONFIG sdl2-config REQUIRED)
    execute_process(COMMAND ${SDL2_CONFIG} --cflags OUTPUT_VARIABLE sdl_cflags OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND ${SDL2_CONFIG} --libs OUTPUT_VARIABLE sdl_libs OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    separate_arguments(sdl_cflags UNIX_COMMAND "${sdl_cflags}")
    separate_arguments(MT_SDL_LIBS UNIX_COMMAND "${sdl_libs}")
    target_compile_definitions(mt_player_flags INTERFACE HOST_BUILD=1)
    # libdbus: the PC's own (Bluetooth back ends); the device
    # links the vendored client library (mt_dbus below).
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(MT_HOST_DBUS REQUIRED IMPORTED_TARGET dbus-1)
    target_link_libraries(mt_player_flags INTERFACE PkgConfig::MT_HOST_DBUS)
    foreach(f IN LISTS sdl_cflags)
        if(f MATCHES "^-I(.+)")
            target_include_directories(mt_player_flags INTERFACE ${CMAKE_MATCH_1})
        elseif(f MATCHES "^-D(.+)")
            target_compile_definitions(mt_player_flags INTERFACE ${CMAKE_MATCH_1})
        else()
            target_compile_options(mt_player_flags INTERFACE ${f})
        endif()
    endforeach()
else()
    # libexecinfo first: musl has no <execinfo.h> of its own.
    target_include_directories(mt_player_flags INTERFACE
        ${D}/libexecinfo ${D}/lvgl/src ${D}/tinyalsa/include ${S}/vendor/config/dbus_vendor_config ${D}/dbus)
    if(TEST_BUILD_TAG)
        target_compile_definitions(mt_player_flags INTERFACE "TEST_BUILD_TAG=\"${TEST_BUILD_TAG}\"")
    endif()
endif()
# UI diagnostics apply to the host simulator as well as the devices.
foreach(opt UI_PERF_TRACE UI_GESTURE_TRACE UI_HITBOX_DEBUG)
    if(${opt})
        target_compile_definitions(mt_player_flags INTERFACE ${opt}=1)
    endif()
endforeach()
target_include_directories(mt_player_flags INTERFACE ${S}/vendor/tinfl)
target_compile_definitions(mt_player_flags INTERFACE MINIZ_NO_DEFLATE_APIS MINIZ_NO_ARCHIVE_APIS)

# mt_objects(<name> [FLAGS <interface lib>] SOURCES ... [OPTIONS ...] [INCLUDES ...]
#            [DEFINES ...] [APPEND_OPTIONS ...])
# One object library per Makefile flag group; the player links their objects
# in the Makefile's order (not archives), so the binary is laid out the same.
# APPEND_OPTIONS go after the FLAGS group's options (the Makefile's
# `$(TARGET_CFLAGS) $(LV_XML_CFLAGS)`: the later -Os wins over -O3).
function(mt_objects name)
    cmake_parse_arguments(G "" "" "FLAGS;SOURCES;OPTIONS;INCLUDES;DEFINES;APPEND_OPTIONS" ${ARGN})
    add_library(${name} OBJECT ${G_SOURCES})
    target_link_libraries(${name} PRIVATE mt_paths ${G_FLAGS})
    if(G_APPEND_OPTIONS)
        add_library(${name}_append INTERFACE)
        target_compile_options(${name}_append INTERFACE ${G_APPEND_OPTIONS})
        target_link_libraries(${name} PRIVATE ${name}_append)
    endif()
    target_include_directories(${name} PRIVATE ${G_INCLUDES})
    target_compile_options(${name} PRIVATE ${G_OPTIONS})
    target_compile_definitions(${name} PRIVATE ${G_DEFINES})
endfunction()

function(mt_glob out)
    file(GLOB_RECURSE files LIST_DIRECTORIES false ${ARGN})
    list(SORT files)
    set(${out} ${files} PARENT_SCOPE)
endfunction()

# ALAC build configuration (see the ALAC group below), shared by the library
# and src/audio/alac_decoder.cpp so both read its headers the same way.
set(MT_ALAC_DEFINES TARGET_RT_LITTLE_ENDIAN=1 TARGET_RT_BIG_ENDIAN=0
    PRAGMA_ONCE=0 PRAGMA_STRUCT_ALIGN=0 PRAGMA_STRUCT_PACKPUSH=0 PRAGMA_STRUCT_PACK=0)

# --- Application ------------------------------------------------------------
set(app_sources ${MT_APP_SOURCES})
if(MT_BOARD_SOURCES_REMOVE)
    list(REMOVE_ITEM app_sources ${MT_BOARD_SOURCES_REMOVE})
endif()
list(APPEND app_sources ${MT_BOARD_SOURCES_ADD} ${MT_APP_CXX_SOURCES})
if(NOT MT_HOST)
    list(APPEND app_sources ${MT_TARGET_ONLY_APP_SOURCES})
endif()
mt_feature_sources(app_sources)
mt_objects(mt_app FLAGS mt_player_flags SOURCES ${app_sources})
# Application code (src/, boards/) only: a macro tested with #if must be
# defined, so a misspelled or missing flag fails the build instead of quietly
# evaluating to 0. Plain on/off switches are tested with #ifdef / defined().
set(MT_APP_WARNINGS -Wundef -Werror=undef)
target_compile_options(mt_app PRIVATE ${MT_APP_WARNINGS})
# BUILD_STAMP and APP_RELEASE_LABEL are read by app_version.c only, so they
# are set on that file alone (the Makefile put them on every device TU): a new
# stamp or label recompiles one file, not the whole player.
if(NOT MT_HOST)
    set(version_defines "BUILD_STAMP=\"${MT_BUILD_STAMP}\"")
    if(RELEASE_LABEL)
        list(APPEND version_defines "APP_RELEASE_LABEL=\"${RELEASE_LABEL}\"")
    endif()
    set_source_files_properties(src/core/app_version.c TARGET_DIRECTORY mt_app PROPERTIES
        COMPILE_DEFINITIONS "${version_defines}")
endif()
# The one application file that includes the vendored lv_xml headers. As a
# compile option, not an include property, so it comes last as in the
# Makefile (the 9.4 relative includes must not shadow anything else).
set_source_files_properties(src/ui/player_layouts.c TARGET_DIRECTORY mt_app PROPERTIES
    COMPILE_OPTIONS -I${D}/lvgl/src/others/translation)
# The ALAC headers' build configuration (MT_ALAC_DEFINES, above) for the
# one application file that includes them.
set_source_files_properties(src/audio/alac_decoder.cpp TARGET_DIRECTORY mt_app PROPERTIES
    COMPILE_DEFINITIONS "${MT_ALAC_DEFINES}")

# --- LVGL and the vendored 9.4 XML engine -------------------------------------
mt_glob(lvgl_sources ${D}/lvgl/src/*.c)
mt_objects(mt_lvgl FLAGS mt_player_flags SOURCES ${lvgl_sources})

mt_glob(lv_xml_sources ${S}/vendor/lv_xml/src/*.c)
list(FILTER lv_xml_sources EXCLUDE REGEX "/(lv_xml_test|xmltok_impl|xmltok_ns)\\.c$")
# LVGL 9.5 dropped the XML engine; v9.4.0's is vendored (vendor/lv_xml/
# README.md). Its "../../misc/..." includes resolve through others/translation,
# two levels below lvgl/src. The expat *_impl/*_ns files are #included.
mt_objects(mt_lv_xml FLAGS mt_player_flags SOURCES ${lv_xml_sources}
    APPEND_OPTIONS -Os -I${D}/lvgl/src/others/translation)

# --- Codecs and libraries -----------------------------------------------------
if(NOT MT_HOST)
    mt_glob(tinyalsa_sources ${D}/tinyalsa/src/*.c)
    mt_objects(mt_tinyalsa SOURCES ${tinyalsa_sources}
        OPTIONS -O3 -g -Wall INCLUDES ${D}/tinyalsa/include ${D}/tinyalsa/src)
endif()

# Matches FAAD2's own CMakeLists's non-fixed-point configuration.
mt_glob(faad2_sources ${D}/faad2/libfaad/*.c)
mt_objects(mt_faad2 SOURCES ${faad2_sources}
    OPTIONS -O3 -g -Wall INCLUDES ${D}/faad2/include ${D}/faad2/libfaad
    DEFINES HAVE_INTTYPES_H=1 HAVE_MEMCPY=1 HAVE_STRING_H=1 HAVE_STRINGS_H=1 HAVE_SYS_STAT_H=1
            HAVE_SYS_TYPES_H=1 HAVE_LRINTF=1 APPLY_DRC "PACKAGE_VERSION=\"2.11.2\"")

# Decoder-only ALAC. TARGET_RT_LITTLE_ENDIAN: EndianPortable.c only detects
# little-endian x86 by itself; mipsel needs it said. MT_ALAC_DEFINES (set
# before the application group, which shares it) gives the rest of Apple's
# configuration macros the headers test with #if an explicit 0.
mt_objects(mt_alac
    SOURCES ${D}/alac/codec/ag_dec.c ${D}/alac/codec/dp_dec.c ${D}/alac/codec/matrix_dec.c
            ${D}/alac/codec/ALACBitUtilities.c ${D}/alac/codec/EndianPortable.c
            ${D}/alac/codec/ALACDecoder.cpp
    OPTIONS -O3 -g $<$<COMPILE_LANGUAGE:C>:-Wall> $<$<COMPILE_LANGUAGE:CXX>:-std=c++11>
    INCLUDES ${D}/alac/codec DEFINES ${MT_ALAC_DEFINES})

mt_glob(mbedtls_sources ${D}/mbedtls/library/*.c)
mt_objects(mt_mbedtls FLAGS mt_player_flags SOURCES ${mbedtls_sources})

mt_objects(mt_cjson FLAGS mt_player_flags SOURCES ${D}/cJSON/cJSON.c)

# libdbus client side only (device: Bluetooth back ends and the AVRCP media-player service): no
# Windows sources, no DBusServer (listening), service spawning, test harness,
# uuidgen main, epoll set, message fuzz util.
if(NOT MT_HOST)
    file(GLOB dbus_sources LIST_DIRECTORIES false ${D}/dbus/dbus/*.c)
    list(SORT dbus_sources)
    list(FILTER dbus_sources EXCLUDE REGEX
        "(-win|-win32|wince-glue)\\.c$|/dbus/dbus-server[^/]*\\.c$|/dbus/dbus-spawn-unix\\.c$|/dbus/dbus-test[^/]*\\.c$|/dbus/dbus-uuidgen\\.c$|/dbus/dbus-pollable-set-epoll\\.c$|/dbus/dbus-message-util\\.c$")
    mt_objects(mt_dbus SOURCES ${dbus_sources}
        OPTIONS -O2 -Wall INCLUDES ${S}/vendor/config/dbus_vendor_config ${D}/dbus
        DEFINES _GNU_SOURCE DBUS_COMPILATION DBUS_STATIC_BUILD)
endif()

# libopus float SILK+CELT decoder (no SIMD, no DNN, no fixed point), the set
# libopus's own CMake build uses by default. Only with MT_CODEC_OPUS.
if(MT_CODEC_OPUS)
    file(GLOB opus_sources LIST_DIRECTORIES false ${D}/opus/src/*.c ${D}/opus/celt/*.c ${D}/opus/silk/*.c)
    list(SORT opus_sources)
    list(FILTER opus_sources EXCLUDE REGEX "/(repacketizer_demo|opus_demo|opus_compare|opus_custom_demo)\\.c$")
    file(GLOB opus_float_sources LIST_DIRECTORIES false ${D}/opus/silk/float/*.c)
    list(SORT opus_float_sources)
    mt_objects(mt_opus SOURCES ${opus_sources} ${opus_float_sources}
        OPTIONS -O3 -g -Wall
        INCLUDES ${D}/opus/include ${D}/opus/celt ${D}/opus/silk ${D}/opus/silk/float
        DEFINES OPUS_BUILD VAR_ARRAYS HAVE_LRINTF=1 HAVE_LRINT=1)
endif()

mt_objects(mt_stb_vorbis FLAGS mt_player_flags SOURCES ${S}/vendor/stb_vorbis/stb_vorbis.c)

# Lua library only (no lua.c/luac.c mains).
set(lua_sources)
foreach(f lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser lstate
          lstring ltable ltm lundump lvm lzio lauxlib lbaselib lcorolib ldblib liolib lmathlib
          loadlib loslib lstrlib ltablib lutf8lib linit)
    list(APPEND lua_sources ${D}/lua/src/${f}.c)
endforeach()
mt_objects(mt_lua SOURCES ${lua_sources}
    OPTIONS -O2 -g -Wall INCLUDES ${D}/lua/src DEFINES $<$<BOOL:${MT_HOST}>:HOST_BUILD=1>)

# backtrace() for main.c's crash handler; musl has none (glibc on the host does).
if(NOT MT_HOST)
    mt_objects(mt_libexecinfo
        SOURCES ${D}/libexecinfo/execinfo.c ${D}/libexecinfo/stacktraverse.c
        OPTIONS -O2 -g -Wall -Wno-frame-address INCLUDES ${D}/libexecinfo)
endif()

# IJG libjpeg 9f decoder only (progressive cover-art fallback); jmemnobs.c is
# the only memory manager, so a decode never spills to storage.
set(jpeg_sources)
foreach(f jdapimin jdapistd jdatasrc jdcoefct jdcolor jddctmgr jdhuff jdinput jdmainct jdmarker
          jdmaster jdmerge jdpostct jdsample jdtrans jdarith jaricom jcomapi jerror jmemmgr
          jmemnobs jutils jidctint jidctfst jidctflt jquant1 jquant2)
    list(APPEND jpeg_sources ${D}/jpeg/${f}.c)
endforeach()
mt_objects(mt_jpeg SOURCES ${jpeg_sources}
    OPTIONS -O2 -g -Wall INCLUDES ${S}/vendor/config/jpeg_vendor_config ${D}/jpeg)

mt_objects(mt_tinfl FLAGS mt_player_flags SOURCES ${S}/vendor/tinfl/miniz_tinfl.c)

# --- The player ----------------------------------------------------------------
if(MT_HOST)
    set(object_groups mt_app mt_lvgl mt_lv_xml mt_faad2 mt_alac mt_mbedtls mt_cjson
                      mt_opus mt_stb_vorbis mt_lua mt_jpeg mt_tinfl)
    set(player ${MT_HOST_PLAYER_NAME})
else()
    set(object_groups mt_app mt_lvgl mt_lv_xml mt_tinyalsa mt_faad2 mt_alac mt_mbedtls mt_cjson
                      mt_dbus mt_opus mt_stb_vorbis mt_lua mt_libexecinfo mt_jpeg mt_tinfl)
    set(player ${MT_TARGET_PLAYER_NAME})
endif()
if(NOT MT_CODEC_OPUS)
    list(REMOVE_ITEM object_groups mt_opus)
endif()
set(player_objects)
foreach(g IN LISTS object_groups)
    list(APPEND player_objects $<TARGET_OBJECTS:${g}>)
endforeach()
add_executable(player ${player_objects})
set_target_properties(player PROPERTIES LINKER_LANGUAGE CXX)
if(MT_HOST)
    set_target_properties(player PROPERTIES OUTPUT_NAME ${player})
    target_link_libraries(player PRIVATE ${MT_SDL_LIBS} PkgConfig::MT_HOST_DBUS pthread m)
else()
    # The unstripped binary keeps the symbols for crash backtraces; the
    # stripped one is what goes on the device.
    set_target_properties(player PROPERTIES OUTPUT_NAME ${player}_unstripped)
    target_link_options(player PRIVATE -static -no-pie)
    target_link_libraries(player PRIVATE pthread m)
    add_custom_command(TARGET player POST_BUILD
        COMMAND ${CMAKE_STRIP} -s -o ${CMAKE_BINARY_DIR}/${player} $<TARGET_FILE:player>
        BYPRODUCTS ${CMAKE_BINARY_DIR}/${player}
        COMMENT "Stripping ${player}")
endif()

# --- mellowtone-launcher -------------------------------------------------------
# Every preset builds it. On the device a tiny static binary (musl), started
# by the board's init shim (boards/<board>/launcher/); never links LVGL or the
# player. Function/data sections + --gc-sections keep it small. The host
# preset builds it too (dynamic), and tests/launcher drives its core with fakes.
set(MT_LAUNCHER_NAME mellowtone-launcher)
add_library(mt_launcher_vendor OBJECT
    ${D}/lvgl/src/libs/tjpgd/tjpgd.c
    ${D}/mbedtls/library/md5.c ${D}/mbedtls/library/sha256.c ${D}/mbedtls/library/platform_util.c)
target_link_libraries(mt_launcher_vendor PRIVATE mt_paths)
# tjpgd reads LVGL's configuration (lv_conf.h, which tests the board define).
target_include_directories(mt_launcher_vendor PRIVATE ${D} ${S}/vendor/config ${D}/mbedtls/include ${D}/mbedtls/library)
target_compile_options(mt_launcher_vendor PRIVATE -Os -ffunction-sections -fdata-sections)
target_compile_definitions(mt_launcher_vendor PRIVATE LV_CONF_INCLUDE_SIMPLE=1 ${MT_BOARD_DEFINE})
add_executable(launcher ${MT_LAUNCHER_SOURCES} ${MT_BOARD_SOURCES_ADD} $<TARGET_OBJECTS:mt_launcher_vendor>)
target_link_libraries(launcher PRIVATE mt_paths pthread)
target_include_directories(launcher PRIVATE ${D} ${S}/vendor/config ${S}/src/launcher ${S}/src/core ${MT_HARDWARE_INCLUDES}
    ${MT_NETWORK_INCLUDES} ${D}/mbedtls/include)
target_compile_options(launcher PRIVATE -Os -g -Wall -Wextra ${MT_APP_WARNINGS} -ffunction-sections -fdata-sections)
target_compile_definitions(launcher PRIVATE LV_CONF_INCLUDE_SIMPLE=1 ${MT_BOARD_DEFINE} ${mt_feature_defs})
set_source_files_properties(src/launcher/lx_main.c TARGET_DIRECTORY launcher PROPERTIES
    COMPILE_DEFINITIONS "LX_BUILD_STAMP=\"${MT_BUILD_STAMP}\"")
if(MT_HOST)
    set_target_properties(launcher PROPERTIES OUTPUT_NAME ${MT_LAUNCHER_NAME})
else()
    target_link_options(launcher PRIVATE -static -no-pie -Wl,--gc-sections)
    set_target_properties(launcher PROPERTIES OUTPUT_NAME ${MT_LAUNCHER_NAME}_unstripped)
    add_custom_command(TARGET launcher POST_BUILD
        COMMAND ${CMAKE_STRIP} -s -o ${CMAKE_BINARY_DIR}/${MT_LAUNCHER_NAME} $<TARGET_FILE:launcher>
        BYPRODUCTS ${CMAKE_BINARY_DIR}/${MT_LAUNCHER_NAME}
        COMMENT "Stripping ${MT_LAUNCHER_NAME}")
endif()

# --- Translations -------------------------------------------------------------
# `i18n` re-extracts po/mellowtone.pot, merges every po/*.po and regenerates
# src/ui/i18n_catalog.c (checked in); `i18n-check` only checks.
add_custom_target(i18n
    COMMAND sh ${S}/tools/i18n/run.sh update ${CMAKE_BINARY_DIR}/i18n
    WORKING_DIRECTORY ${S} USES_TERMINAL VERBATIM)
add_custom_target(i18n-check
    COMMAND sh ${S}/tools/i18n/run.sh check ${CMAKE_BINARY_DIR}/i18n
    WORKING_DIRECTORY ${S} USES_TERMINAL VERBATIM)

# --- Tests (host only) ----------------------------------------------------------
if(MT_HOST)
    enable_testing()
    add_subdirectory(tests)
    add_test(NAME i18n-check
        COMMAND sh ${S}/tools/i18n/run.sh check ${CMAKE_BINARY_DIR}/i18n
        WORKING_DIRECTORY ${S})
endif()
