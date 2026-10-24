# External tools and files

Things the build uses that are not in this repository. Most are fetched
automatically; the rest you provide yourself. See [BUILDING.md](BUILDING.md)
for how `./mt` uses them.

## Fetched automatically

| What | Fetched by | Pinned by |
| --- | --- | --- |
| musl cross toolchain `mipsel-linux-musl-cross` (builds the player and launcher for every board) | `tools/release/install_mips_toolchain.sh` while `./mt setup build` builds the container, from the [musl.cc GitHub release mirror](https://github.com/musl-cc/musl.cc/releases/tag/v0.0.1), falling back to [musl.cc](https://musl.cc/) | SHA-256 in the script |
| Library sources (LVGL, Lua, mbedTLS, D-Bus, FAAD2, ALAC, Opus, libjpeg, ...) | CMake at configure time, into `build/<preset>/_deps/` | URLs and SHA-256 in `cmake/dependencies.cmake` |
| BlueZ 4.101 tarball | `./mt bluez` (`containers/bluez/build.sh`), from [kernel.org](https://www.kernel.org/pub/linux/bluetooth/) | SHA-256 in the script |
| Debian packages | the `Containerfile`s | Debian base image digest |

## Install yourself

| What | Needed for | Where to get it |
| --- | --- | --- |
| Podman (or Docker) | everything | [podman.io](https://podman.io/docs/installation), or [Docker Engine](https://docs.docker.com/engine/install/) |
| Android platform-tools (`adb`) | installing on a device over ADB, reading logs | [developer.android.com](https://developer.android.com/tools/releases/platform-tools) |

## Provide yourself (never committed)

| What | Needed for | Notes |
| --- | --- | --- |
| Official HiBy firmware (`r3pro.upt` 2.2 for the R3 Pro; the board's firmware for the others) | `./mt image r3pro --stock`, the X1600 base images, a device sysroot | From [HiBy's download and help center](https://store.hiby.com/apps/help-center). The R3 Pro image accepts only the 2.2 file whose SHA-256 is listed in [FIRMWARE.md](FIRMWARE.md). Images built from it contain HiBy's software and stay on your computer. |
| Ingenic `mips-gcc472-glibc216` toolchain (`MT_INGENIC_TC`) | `./mt bluez` | Part of Ingenic's Halley2 Linux SDK (`release_halley2_v1.0-20151221`, `toolchains/mips-gcc472-glibc216`), distributed by Ingenic to developers; this project knows of no official public download. Point `MT_INGENIC_TC` at the directory holding `bin/mips-linux-gnu-gcc`. |
| Device sysroot (`MT_BT_SYSROOT`) | `./mt bluez` | glib, D-Bus, libffi and zlib libraries and headers matching the R3 Pro's firmware, laid out as `usr/lib` and `usr/include`. Build it from your own firmware dump. |
| Noto Sans fonts (`MT_FONTS_DIR`) | `./mt image r3pro` (full image), `boards/r3pro/tools/gen_drawer_icons.py` | The files and SHA-256 sums in `boards/r3pro/image/fonts.sha256`, from [Noto](https://notofonts.github.io/) (SIL Open Font License 1.1). |
| Ingenic toolchain v5.2 (`mips-linux-gnu-`, GCC 5.2, glibc 2.22) | `boards/common/base-image/build_base_*.sh` (X1600 base-image libraries) only | Commit and URL in `boards/common/base-image/README.md`; the scripts expect it in `scratch/ingenic-toolchain-v5.2` or `BASE_CROSS_PREFIX`. |
| Python 3 with Pillow | regenerating icons (`tools/assets/generate_*.py`, `boards/r3pro/tools/gen_drawer_icons.py`) only | Not in the build container; run the scripts in any container or environment that has it. The generated images are committed. |
