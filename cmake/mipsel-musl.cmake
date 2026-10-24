# Toolchain file for the HiBy players: mipsel-linux-musl-cross (musl.cc), the
# toolchain the mt-build container installs at /opt/toolchains (see
# tools/release/install_mips_toolchain.sh). Every board is built with it; the host
# simulator uses the native compiler instead (preset "host").
#
# Override the location with -DMT_TOOLCHAIN_ROOT=... or $MT_TOOLCHAIN_ROOT.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR mipsel)

if(NOT MT_TOOLCHAIN_ROOT)
    if(DEFINED ENV{MT_TOOLCHAIN_ROOT})
        set(MT_TOOLCHAIN_ROOT "$ENV{MT_TOOLCHAIN_ROOT}")
    else()
        set(MT_TOOLCHAIN_ROOT /opt/toolchains/mipsel-linux-musl-cross)
    endif()
endif()
set(MT_TOOLCHAIN_ROOT "${MT_TOOLCHAIN_ROOT}" CACHE PATH "mipsel-linux-musl-cross toolchain root")
# Keep the value visible to the compiler-check try_compile projects too.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES MT_TOOLCHAIN_ROOT)

set(_mt_prefix "${MT_TOOLCHAIN_ROOT}/bin/mipsel-linux-musl-")
set(CMAKE_C_COMPILER   "${_mt_prefix}gcc")
set(CMAKE_CXX_COMPILER "${_mt_prefix}g++")
set(CMAKE_AR           "${_mt_prefix}ar"      CACHE FILEPATH "")
set(CMAKE_RANLIB       "${_mt_prefix}ranlib"  CACHE FILEPATH "")
set(CMAKE_STRIP        "${_mt_prefix}strip"   CACHE FILEPATH "")
set(CMAKE_OBJCOPY      "${_mt_prefix}objcopy" CACHE FILEPATH "")

set(CMAKE_FIND_ROOT_PATH "${MT_TOOLCHAIN_ROOT}/mipsel-linux-musl")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# The player is a fully static executable; the compiler checks must not try
# to run anything or link shared objects.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
