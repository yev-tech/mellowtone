# Third-party sources, pinned by archive URL + SHA-256.
#
# Each dependency is the exact upstream commit the old Makefile's clone was
# built from. Archives are downloaded once into MT_DOWNLOAD_DIR (inside ./mt
# that is /cache/downloads, a host directory, so rebuilds work offline) and
# extracted per build directory into ${MT_DEPS_SRC}/<name>, where the patches
# for this board are applied (cmake/apply_patches.cmake, --fuzz=0).
#
# Nothing upstream is built with its own build system: SOURCE_SUBDIR points at
# a directory that does not exist, so FetchContent only downloads, extracts
# and patches. The source subsets and flags live in CMakeLists.txt.
include(FetchContent)

set(MT_DEPS_SRC "${CMAKE_BINARY_DIR}/_deps/src")
if(DEFINED ENV{MT_CACHE_DIR})
    set(_mt_dl_default "$ENV{MT_CACHE_DIR}/downloads")
else()
    set(_mt_dl_default "${CMAKE_SOURCE_DIR}/build/downloads")
endif()
set(MT_DOWNLOAD_DIR "${_mt_dl_default}" CACHE PATH "Where dependency archives are cached")

# mt_dependency(<name> URL <url> SHA256 <hash> [STRIP <n>] [PATCHES ...]
#               [COPY_FROM <dir> COPY_TO <subdir> COPY_FILES ...] [POST cmd args...])
function(mt_dependency name)
    cmake_parse_arguments(D "" "URL;SHA256;STRIP;COPY_FROM;COPY_TO;ARCHIVE_NAME" "PATCHES;COPY_FILES;POST" ${ARGN})
    if(NOT D_STRIP)
        set(D_STRIP 1)
    endif()
    if(NOT D_ARCHIVE_NAME)
        get_filename_component(D_ARCHIVE_NAME "${D_URL}" NAME)
        set(D_ARCHIVE_NAME "${name}-${D_ARCHIVE_NAME}")
    endif()

    # Everything that changes the patched tree goes into the stamp, so a
    # changed patch file re-extracts and re-patches instead of stacking.
    set(stamp_input "${D_SHA256};${D_STRIP};${D_POST};${D_COPY_TO}")
    foreach(p IN LISTS D_PATCHES)
        file(SHA256 "${p}" h)
        string(APPEND stamp_input ";${p}=${h}")
    endforeach()
    foreach(f IN LISTS D_COPY_FILES)
        file(SHA256 "${D_COPY_FROM}/${f}" h)
        string(APPEND stamp_input ";${f}=${h}")
    endforeach()
    string(SHA256 stamp "${stamp_input}")

    string(REPLACE ";" "," patches "${D_PATCHES}")
    string(REPLACE ";" "," copy_files "${D_COPY_FILES}")
    string(REPLACE ";" "," post "${D_POST}")
    set(patch_cmd ${CMAKE_COMMAND}
        -DSRC=${MT_DEPS_SRC}/${name}
        -DARCHIVE=${MT_DOWNLOAD_DIR}/${D_ARCHIVE_NAME}
        -DSTAMP=${stamp}
        -DSTRIP=${D_STRIP})
    if(patches)
        list(APPEND patch_cmd -DPATCHES=${patches})
    endif()
    if(copy_files)
        list(APPEND patch_cmd -DCOPY_FROM=${D_COPY_FROM} -DCOPY_TO=${D_COPY_TO} -DCOPY_FILES=${copy_files})
    endif()
    if(post)
        list(APPEND patch_cmd -DPOST=${post})
    endif()
    list(APPEND patch_cmd -P ${CMAKE_SOURCE_DIR}/cmake/apply_patches.cmake)

    FetchContent_Declare(${name}
        URL "${D_URL}"
        URL_HASH SHA256=${D_SHA256}
        DOWNLOAD_NAME "${D_ARCHIVE_NAME}"
        DOWNLOAD_DIR "${MT_DOWNLOAD_DIR}"
        SOURCE_DIR "${MT_DEPS_SRC}/${name}"
        SOURCE_SUBDIR mt-no-upstream-build
        PATCH_COMMAND ${patch_cmd})
    FetchContent_MakeAvailable(${name})
    set(MT_${name}_DIR "${MT_DEPS_SRC}/${name}" PARENT_SCOPE)
endfunction()

# LVGL v9.5.0 (tag commit). Patched with the fbdev compositor and the runtime
# fixes, the regenerated Montserrat fonts installed over upstream's, plus the
# board's own patches (MT_LVGL_BOARD_PATCHES, boards/<name>/board.cmake).
set(MT_LVGL_GENERATED_FONTS
    lv_font_montserrat_12.c lv_font_montserrat_14.c lv_font_montserrat_16.c lv_font_montserrat_18.c
    lv_font_montserrat_20.c lv_font_montserrat_22.c lv_font_montserrat_24.c lv_font_montserrat_26.c
    lv_font_montserrat_28.c lv_font_montserrat_30.c lv_font_montserrat_32.c lv_font_montserrat_34.c
    lv_font_montserrat_40.c)
mt_dependency(lvgl
    URL https://github.com/lvgl/lvgl/archive/85aa60d18b3d5e5588d7b247abf90198f07c8a63.tar.gz
    SHA256 b32747cf6568d86a5cb9d30f53970bec1c7a322cb624c4158d4b0e348324146b
    PATCHES ${CMAKE_SOURCE_DIR}/patches/lvgl_fbdev_compositor.patch
            ${CMAKE_SOURCE_DIR}/patches/lvgl_runtime_fixes.patch
            ${MT_LVGL_BOARD_PATCHES}
    COPY_FROM ${CMAKE_SOURCE_DIR}/patches/lvgl_generated_fonts
    COPY_TO src/font
    COPY_FILES ${MT_LVGL_GENERATED_FONTS})

# dr_libs (dr_flac/dr_wav/dr_mp3), with the WAV/AIFF metadata-seek hardening.
mt_dependency(dr_libs
    URL https://github.com/mackron/dr_libs/archive/dfe8377631000664666519fdb83da193fd8037f4.tar.gz
    SHA256 4654acb029f4f2a43ac2edb60c4cb09f40615b4b5bee9709954f910cb979e5fd
    STRIP 2
    PATCHES ${CMAKE_SOURCE_DIR}/patches/dr_libs_metadata_hardening.patch)

mt_dependency(tinyalsa
    URL https://github.com/tinyalsa/tinyalsa/archive/961babfe962e71d952ae734074cc89576b28a9e7.tar.gz
    SHA256 067268cbe4e56e94fdeccc7e9d1283c92d5f6642ae62202445dafa43db339200)

# FAAD2 (GPL-2.0-or-later) AAC decoder.
mt_dependency(faad2
    URL https://github.com/knik0/faad2/archive/521d105979795b887ea11e1f36565e97a8fbfc53.tar.gz
    SHA256 9284bd9c5ef998f636b233055f4d988aadf0d5a97acdb3109bcb9d4e3c7cfdd8)

# Apple's ALAC reference decoder (mikebrady's mirror).
mt_dependency(alac
    URL https://github.com/mikebrady/alac/archive/5d8c5db0dfcadd5872f28e665cf4f4303447352a.tar.gz
    SHA256 f35e19628b4e5ad7f73a04c5b884fa85b0fe44ff99602209b9e6f2d13d9842c2)

# libopus v1.5.2 (MT_CODEC_OPUS only).
if(MT_CODEC_OPUS)
    mt_dependency(opus
        URL https://github.com/xiph/opus/archive/ddbe48383984d56acd9e1ab6a090c54ca6b735a6.tar.gz
        SHA256 18e9d9e26565e279753aafb6823a482ef85ea9fddbfdcdbff8fd095b12dd96b6)
endif()

# IJG libjpeg 9f (libjpeg-turbo's "ijg" mirror, tag jpeg-9f).
mt_dependency(jpeg
    URL https://github.com/libjpeg-turbo/ijg/archive/7f0b6f5d6767c25dd4585786f59999b503bdd04b.tar.gz
    SHA256 aa2bfb0d7f26b54b2d0a4e17b54212854e5a29a85def77e11ec64287f666b5e4)

# mbedTLS v3.6.2.
mt_dependency(mbedtls
    URL https://github.com/Mbed-TLS/mbedtls/archive/107ea89daaefb9867ea9121002fbbdf926780e98.tar.gz
    SHA256 7dc3219b00ab2d33d5108a6cfb1b6d11c1476f882493acdf0be5bcc9a181336b)

mt_dependency(cJSON
    URL https://github.com/DaveGamble/cJSON/archive/6d9f2443ab071f86e5d9b43025a40929ec41c46c.tar.gz
    SHA256 2ee53560eb76b6d84d96989689ff46ccc4cd1a9c786967486b3105a7087d1756)

# libdbus 1.16.2 (tag commit 958bf9db2100553bcd2fe2a854e1ebb42e886054). The
# official release tarball rather than a GitLab archive: GitLab does not
# promise byte-stable archives, and the release's dbus/ directory is
# identical to the tag's.
mt_dependency(dbus
    URL https://dbus.freedesktop.org/releases/dbus/dbus-1.16.2.tar.xz
    SHA256 0ba2a1a4b16afe7bceb2c07e9ce99a8c2c3508e5dec290dbb643384bd6beb7e2
    ARCHIVE_NAME dbus-1.16.2.tar.xz)

# libexecinfo (backtrace() for musl). stacktraverse.c is generated, as its
# own Makefile does.
mt_dependency(libexecinfo
    URL https://github.com/fam007e/libexecinfo/archive/13976c7221c89d3d7904043c119720c26a1c36a9.tar.gz
    SHA256 40c671a6f7bf870be2a107e5b5578fcf671699f898cef9193e104cff6d9012ef
    POST python3 gen.py --max-depth 128 --output stacktraverse.c)

# Lua 5.5.1, official source tarball.
mt_dependency(lua
    URL https://www.lua.org/ftp/lua-5.5.1.tar.gz
    SHA256 1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce
    ARCHIVE_NAME lua-5.5.1.tar.gz)
