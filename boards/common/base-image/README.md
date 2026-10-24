# X1600 base-image runtime upgrade

These recipes build the newer userspace libraries and tools that the
Mellowtone player needs in an X1600 base image (written for the R1; the
R3 Pro II and R3 II (2025) Bluetooth runtime can also come from
`boards/common/prepare_r3_pro_ii_bluetooth_overlay.sh`, see
`docs/FIRMWARE.md`).

This is a staged **userspace** upgrade, not a replacement distribution or
kernel. The player uses a static musl build; the extracted firmware uses
glibc 2.22. Build its shared libraries with the Ingenic glibc toolchain,
not `mipsel-linux-musl-gcc`.

## Component set

| Component | Version / scope |
| --- | --- |
| BlueALSA | 5.0.0: daemon, control tools, ALSA plugins |
| ALSA library | 1.2.16; retain device ALSA configuration |
| SBC | 2.2 |
| AAC | fdk-aac 2.0.3 |
| aptX | rebuild libopenaptx 0.2.0; retain LGPL version |
| BlueZ | 5.87 public `libbluetooth` only; retain vendor 5.54 daemon/tools |
| D-Bus | 1.16.2 library, daemon, tools and activation helper together |
| GLib | 2.84.4, with libffi 3.4.8 and PCRE2 10.46 |
| zlib | 1.3.2 |
| Expat | 2.8.4 |

Retain the matched vendor LDAC encoder/ABR pair. The LDAC decoder is rebuilt
from source: the vendor libldacdec.so.1 segfaults (decodeSpectrumFine,
decodeScaleFactors) about a second into real LDAC reception in DAC mode.
`libldacdec-hiby.patch` bounds every read to the packet and frame, validates
stream fields and the negotiated rate/channel mode, commits IMDCT history only
for fully valid frames, and fixes upstream decoding bugs (fine-precision cap
and dequantization, descending gradient, dual-channel blocks, mode-2 scale
factor wrap, scale factor 0, S32 gain). Soname, exports and ABI match the
vendor build. During development, a temporary local host harness encoded with
the pinned AOSP encoder and decoded through `dlopen`, reporting rejects and
SNR; that harness is not a repository build input. The small decoder
compatibility header supplies the declarations BlueALSA needs.
Keep libc/loader, OpenSSL, curl, Wi-Fi drivers/firmware and the working player
mbedTLS version unchanged. Shared-library SONAME filenames do
not establish upstream source versions.

## Sources and build order

Host tools: GCC/C++, make, autoconf, automake, libtool, gettext development
tools, CMake, Meson, Ninja, pkg-config, Git, curl, archive utilities, readelf,
and qemu-user (`qemu-mipsel`). GLib development tools may also be required
for host-side generators.

Archive-based recipes download pinned archives and verify SHA-256. Git-based
recipes require the following checkouts; they reject unexpected revisions.
Clone missing checkouts and select the pinned commit, without resetting an
existing dirty checkout:

| Directory relative to repository | Upstream | Commit |
| --- | --- | --- |
| `scratch/ingenic-toolchain-v5.2` | https://github.com/tobunto/ingenic-toolchain-v5.2 | `4de21963b0f10c19118045f78c3e853a5e2c0be6` |
| `scratch/bluez-alsa-5.0.0` | https://github.com/arkq/bluez-alsa | `1935d6dcb8975f2d7a51aaafe61538d157224623` |
| `scratch/base-upgrade/fdk-aac` | https://github.com/mstorsjo/fdk-aac | `716f4394641d53f0d79c9ddac3fa93b03a49f278` |
| `scratch/base-upgrade/libopenaptx` | https://github.com/pali/libopenaptx | `2459ed4686eaef0a19dfa3f330a960813c5f60de` |
| `scratch/base-upgrade/ldacBT` | https://github.com/EHfive/ldacBT | `6579bd585a618f2e1612b3c1650d2b7fcfb1d43f` |
| `scratch/base-upgrade/libldacdec-anonymix007` | https://github.com/anonymix007/libldacdec | `c90094b15e25aef0e47c6d775fa94aceb36cabbc` |
| `$BASE_DBUS_SOURCE` (any directory) | https://gitlab.freedesktop.org/dbus/dbus | `958bf9db2100553bcd2fe2a854e1ebb42e886054` |

Initialize LDAC's submodule: its `libldac` revision must be
`82b6a1abee84787b8fa167efe20290073f60db2d`. The LDAC decoder is built from
anonymix007/libldacdec `c90094b15e25aef0e47c6d775fa94aceb36cabbc` plus
`libldacdec-hiby.patch` (its own `libldac` submodule is not needed).

From the repository root:

```sh
bash boards/common/base-image/build_base_audio_deps.sh
bash boards/common/base-image/build_base_zlib.sh
bash boards/common/base-image/build_base_expat.sh
bash boards/common/base-image/build_base_bluez_library.sh
bash boards/common/base-image/build_base_glib.sh
bash boards/common/base-image/build_base_dbus.sh
bash boards/common/base-image/build_base_bt_codecs.sh
bash boards/common/base-image/build_base_bluealsa.sh
./mt build r1
bash boards/common/prepare_base_image_overlay.sh
```

Set `BASE_STOCK_ROOT` to your own extracted stock firmware root (the
`squashfs-root` of the board's official update); the scripts stop with an
error when it is unset. Override `BASE_CROSS_PREFIX` where needed. Outputs stay
under `scratch/base-upgrade`; the overlay script does not install or flash.
Keep the downloaded sources, notices and these recipes with release source
materials. Runtime notices are included in the overlay.

## Local validation and installation

These base-image recipes are local development tools; this guide does not
depend on a checked-in test runner or fixture files. After merging the overlay
into a disposable copy of the matching stock root, check that the target
binaries have the expected architecture and can report their versions or
capabilities under QEMU user emulation:

```sh
root=/absolute/path/to/candidate/root
file "$root/usr/bin/bluealsad" "$root/usr/bin/bluealsactl"
qemu-mipsel -L "$root" "$root/usr/bin/bluealsad" --version
qemu-mipsel -L "$root" "$root/usr/bin/bluealsactl" --help
```

For an R3 Bluetooth overlay, also check the staged BlueZ daemon and CLI:

```sh
qemu-mipsel -L "$root" "$root/usr/libexec/bluetooth/bluetoothd" --version
qemu-mipsel -L "$root" "$root/usr/bin/bluetoothctl" --help
```

For a Wi-Fi overlay, check the staged supplicant's version and compiled
capabilities:

```sh
qemu-mipsel -L "$root" "$root/usr/sbin/wpa_supplicant" -v
qemu-mipsel -L "$root" "$root/usr/sbin/wpa_supplicant" -h
```

Use the actual paths present in the candidate root for components not included
in a particular overlay. These checks exercise the built target binaries and
libraries, but QEMU uses the host kernel. They do not validate kernel boot,
audio quality, Bluetooth pairing, Wi-Fi association, suspend, or radio
firmware behavior; those require testing on the matching hardware.

Back up the extracted stock tree on the workstation before applying the
overlay to the stock tree. Do not put backups in `/usr/data`. Preserve existing D-Bus
policies/init configuration and helper permissions. The existing stock-tree packer
uses `mksquashfs -all-root`; this matters because staging files are owned by
the build user. The stock activation helper is 0755; this upgrade does not
silently add setuid privileges. Service activation requiring elevated
privileges is not enabled or newly validated here.

BlueALSA 5 renames `bluealsa` to `bluealsad` and `bluealsa-cli` to
`bluealsactl`. The overlay updates device startup scripts and includes the
matching player. Old executables are
retained for rollback/loader testing, not intended to control the new daemon.
The old and new BlueALSA policy filenames are deliberately both retained.

Before a release, test SBC/SBC-XQ/AAC/aptX/aptX-HD/LDAC playback, Bluetooth
DAC mode, volume sync on/off, reconnect, local audio, Wi-Fi, and suspend/wake
on the actual R1. Repacking/flashing is a separate step.

## Kernel boundary

The stock kernel is Linux 4.4.94+ and the 29 vendor modules require its exact
MIPS32_R2 ABI. The available mainline/Letux tree lacks an R1 board definition
and validated audio, display/touch and other device support. Replacing the
kernel safely requires the vendor source/configuration or a full board port;
it is not a drop-in package upgrade. Keep `xImage` and modules unchanged.

## Player-facing overlays

These overlays are separate from the shared libraries above. Build and test
them one at a time:

```sh
bash boards/common/base-image/build_base_alsa_utils.sh
bash boards/common/prepare_player_base_overlay.sh alsa

bash boards/common/base-image/build_base_bluez.sh
./mt build r1
bash boards/common/prepare_player_base_overlay.sh bluetooth

bash boards/common/base-image/build_base_wifi.sh
bash boards/common/prepare_player_base_overlay.sh wifi
```

Outputs: `scratch/base-upgrade/player-overlays/{alsa,bluetooth,wifi}/current`.
Each has a sibling `files.txt` manifest. Nothing is installed or flashed by
these scripts. The Bluetooth overlay assumes the GLib/D-Bus
libraries above are already present and includes the matching player binary.

- **ALSA utilities 1.2.16:** only `aplay`, `arecord`, and `amixer` are installed.
  Keep the vendor mixer/state configuration and existing `alsamixer`.
- **BlueZ 5.87:** upgrade `bluetoothd`, `bluetoothctl`, `hciconfig`, `hcitool`
  and `btmon`. Preserve UART/firmware loaders (`hciattach`,
  `brcm_patchram_plus`, etc.), init scripts, configuration and pairing data.
  The daemon's generated `STORAGEDIR` is redirected to `/usr/data/bluetooth`,
  matching the device's persistent stock pairing directory; `/var/lib/bluetooth`
  remains the unrelated configure-time state default and is not used for
  pairing data. The build recipe verifies both `config.h` and staged daemon
  strings before publishing its stage.
  Mesh and LE Audio/related new audio profiles are disabled for Linux 4.4;
  classic A2DP/AVRCP remain. The player detects the supported paired-device
  command from CLI help, retaining compatibility with stock BlueZ 5.54.
- **wpa_supplicant 2.12 / libnl 3.12.0:** install supplicant, CLI, client
  library, and only libnl core/generic-netlink modules. Preserve saved
  networks, DHCP and vendor drivers. The checked-in `wpa.config` keeps
  nl80211/WEXT/wired, enterprise EAP, WPS, HS20/interworking and CLI control;
  it does not inherit upstream's AP/P2P/MACsec/D-Bus defaults. The `none`
  backend supports isolated control-interface testing without touching a
  physical interface. WPA3/other capabilities still depend on device support.

The radio recipes obtain pinned development headers rather than installing
host development packages. Readline 8.0 and OpenSSL 1.1.1f **runtime libraries
are retained**, not upgraded. In particular, this does not fix vulnerabilities
in the retained old OpenSSL itself. The OpenSSL sources are used only to
generate matching public headers. The player's separate TLS implementation
is unchanged.

What has been checked: library-level tests (dynamic linking, both BlueALSA
plugins, SBC, ALSA null playback and capture, BlueZ and wpa_supplicant
read-only queries) passed under QEMU and on an R1, using temporary helpers
that are not part of this repository. Headphone pairing, audible playback,
Wi-Fi association and suspend have not been tested with these overlays, and
the Mellowtone player has not been tested on the R1 by this project. Keep the
radio overlays out of an image until those device tests pass.

RAM caution: `/tmp` is RAM-backed and the R1 exposes about 56 MiB total RAM.
Keeping several batches of test binaries/libraries there exhausted enough
headroom to prevent ADB shell creation during testing. Removing unused test
copies restored access without a reboot. Stage one batch at a time, keep at
least 8 MiB available before the isolated supplicant test, and remove test
copies afterward. This is not a substitute for memory testing the final
flash-backed image during real playback and radio activity.
