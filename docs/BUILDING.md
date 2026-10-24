# Building Mellowtone

Every build step runs in a container through `./mt`, so the only program you
need on your computer is a container engine. External files that are not in
this repository (toolchains, the stock firmware, fonts) are listed in
[TOOLS.md](TOOLS.md).

## Prerequisites

- Linux, `git` and `bash`.
- [Podman](https://podman.io/docs/installation) (preferred; rootless works)
  or Docker. `./mt` picks Podman when it is installed; set `MT_ENGINE` to
  choose.
- Network access for the first build: the containers install Debian packages,
  the build container downloads the pinned musl cross toolchain, and CMake
  downloads the pinned dependency archives (`cmake/dependencies.cmake`).

## Container images

```sh
./mt setup            # all three images
./mt setup build      # mt-build: player, launcher, host simulator, tests, translations
./mt setup bluez      # mt-bluez: the R3 Pro bluetoothd (BlueZ 4.101)
./mt setup image      # mt-image: the R3 Pro firmware image
```

The recipes are `containers/build/Containerfile`,
`containers/bluez/Containerfile` and `containers/image/Containerfile`. The
build image runs `tools/release/install_mips_toolchain.sh`, which downloads the
musl cross toolchain and checks its SHA-256. `./mt shell [build|bluez|image]`
opens a shell in one of them.

## Commands

| Command | What it does | Output |
| --- | --- | --- |
| `./mt build <board>` | Player and `mellowtone-launcher` for `r3pro`, `r1`, `r3proii` or `r3ii_2025` (CMake preset `<board>`) | `build/<board>/mellowtone-<board>`, `build/<board>/mellowtone-launcher` |
| `./mt build host` | The SDL simulator (needs a display) | `build/host/mellowtone-host` |
| `./mt build host-minimal` | The simulator with every optional feature switched off | `build/host-minimal/` |
| `./mt test [host\|host-minimal]` | Unit tests and the translation check, through `ctest` (default `host`) | test results on the terminal |
| `./mt i18n` | Extracts strings, merges `po/*.po`, regenerates `src/ui/i18n_catalog.c` (see `po/README.md`) | files under `po/` and `src/ui/` |
| `./mt i18n check` | Fails on a stale template, bad printf conversions or unwrapped UI strings | |
| `./mt bluez [OUT_DIR]` | R3 Pro `bluetoothd` from the BlueZ 4.101 tarball plus `patches/bluez-4.101/` | `out/r3pro/bluetoothd` by default |
| `./mt bluez-dev [DIR]` / `./mt bluez-export [DIR]` | An editable BlueZ tree with one commit per patch, and regenerating the patches from it | `.bluez-dev/` by default |
| `./mt image r3pro --stock <r3pro.upt> ...` | The R3 Pro firmware image from your own official firmware (see [FIRMWARE.md](FIRMWARE.md)) | `out/images/` by default |

`./mt build` passes `-DVAR=value` arguments to the configure step (for
example `-DRELEASE_LABEL="My build"`, `-DUI_PERF_TRACE=ON`) and everything
else to `cmake --build` (for example `--target launcher` or `-v`). Optional
features are CMake options as well (`cmake/features.cmake`); the player's
`--features` flag prints what a binary has compiled in. The configure step
runs on every build, so `RELEASE_LABEL`, `TEST_BUILD_TAG` and the UI trace
options apply to that build only.

Firmware packages for the X1600 boards (R1, R3 Pro II, R3 II 2025) are made
with `boards/common/repack_upt.sh` from a base image you supply; see
[FIRMWARE.md](FIRMWARE.md). Installing on the R3
Pro over ADB is described in `boards/r3pro/launcher/INSTALL.md`.

## Settings (`.mt.local`)

`./mt` reads these from the environment, or from a `.mt.local` file in the
repository root (shell syntax, git-ignored, never committed):

| Variable | Used by | Meaning |
| --- | --- | --- |
| `MT_ENGINE` | all | `podman` (default when installed) or `docker` |
| `MT_CACHE` | all | Download cache on your computer, default `~/.cache/mellowtone` (mounted at `/cache` in the containers) |
| `MT_INGENIC_TC` | `./mt bluez` | The Ingenic `mips-gcc472-glibc216` toolchain directory (the one holding `bin/mips-linux-gnu-gcc`) |
| `MT_BT_SYSROOT` | `./mt bluez` | A device sysroot with the glib, D-Bus, libffi and zlib libraries and headers (`usr/lib`, `usr/include`) |
| `MT_FONTS_DIR` | `./mt image` | The Noto fonts listed in `boards/r3pro/image/fonts.sha256` (same as `--fonts DIR`) |
| `SOURCE_DATE_EPOCH` | `./mt build`, `./mt image` | Fixes the build stamp for reproducible builds |

Example:

```sh
MT_INGENIC_TC=$HOME/ingenic/mips-gcc472-glibc216
MT_BT_SYSROOT=$HOME/r3pro/sysroot
MT_FONTS_DIR=$HOME/r3pro/fonts
```

The toolchain and sysroot are mounted read-only into the bluez container.

## Source tree

| Path | Contents |
| --- | --- |
| `src/` | The player and the launcher: `core/`, `audio/`, `library/`, `ui/`, `plugins/`, `launcher/`, `network/` (`http/`, `wifi/`, `bluetooth/`, `services/`, `online/`) and `hardware/` (`power/`, `usb/`, `input/`) |
| `boards/<board>/` | Everything that differs per device: the descriptor (`board.c`, `board.cmake`), the launcher shim, `assets/`; for the R3 Pro also the image recipe (`image/`) and `tools/` |
| `boards/common/` | Firmware tooling shared by the X1600 boards: `repack_upt.sh`, the `prepare_*_overlay.sh` scripts, the `base-image/` recipes and the root-file-system `overlay/` |
| `assets/` | The shared theme (`themes/`), fonts, and the SVG sources of generated icons (`sources/`) |
| `tools/` | Developer scripts: `assets/` (icon generators), `i18n/`, `release/`, `dev/` |
| `vendor/` | Third-party code compiled from the tree (`tinfl/`, `stb_vorbis/`, `lv_xml/`) and the configuration of vendored libraries (`config/`: `lv_conf.h`, D-Bus, libjpeg) |
| `patches/` | Patch series for downloaded dependencies (LVGL, dr_libs, BlueZ 4.101) |
| `examples/plugins/` | Example Lua plugins (`docs/PLUGINS.md`) |
| `cmake/`, `containers/`, `po/`, `tests/` | Build helpers, container recipes, translations, unit tests |

## Where things land

| Path | Contents |
| --- | --- |
| `build/<preset>/` | Each CMake preset's build tree and binaries; `build/<preset>/_deps/` holds the pinned dependency sources |
| `out/` | `./mt bluez` and `./mt image` outputs |
| `~/.cache/mellowtone` (`MT_CACHE`) | Downloaded archives, shared by all builds |
| `scratch/` | Work area of the base-image recipes in `boards/common/base-image/build_base_*.sh` (see `boards/common/base-image/README.md`) |

All of these are git-ignored.

## Theme assets

The icons the player loads, under `assets/` (shared) and
`boards/<board>/assets/` (per board), are tracked in git, and only tracked
files ship (`boards/common/repack_upt.sh`, `boards/r3pro/tools/make_theme.sh`).
Each directory's `SOURCES.txt` records where its images come from. Icons
drawn by this project are regenerated with the generators in `tools/assets/`
(SVG sources in `assets/sources/`) and `boards/r3pro/tools/` (for example
`boards/r3pro/tools/gen_drawer_icons.py FONTS_DIR` and
`tools/assets/generate_stream_media_icons.py`); they need Python 3 with Pillow.
Images from the stock HiBy firmware are never committed.

## Releases

`tools/release/build_release.sh VERSION [OUT_DIR]` builds a release locally:
for each released board the player update package `mellowtone-<board>-VERSION.tar`
(the player, the launcher and `VERSION`, made by
`tools/release/make_update_package.sh`), an archive of the unstripped
binaries for reading crash reports, and `SHA256SUMS`. The default output is
`out/release/`. The same commit gives the same files: `SOURCE_DATE_EPOCH`
defaults to the commit time. Only boards tested on hardware are released,
the R3 Pro for now: `RELEASE_BOARDS` in the script lists them, and
`MT_RELEASE_BOARDS="r1 r3pro"` overrides it for a local test build.

On GitHub, `.github/workflows/ci.yml` runs the tests and builds every board
for each push to `main` and each pull request. Pushing a tag `v<version>`
runs `.github/workflows/release.yml`, which builds the release with that
script and creates a **draft** release with the files; check it and publish
it on the Releases page:

```sh
git tag -a v0.1.0 -m "Mellowtone 0.1.0"
git push origin v0.1.0
```

Running the Release workflow by hand (Actions > Release > Run workflow) builds
the files as a workflow artifact without creating a release. Firmware images
are never built there; they contain HiBy's software (FIRMWARE.md).
