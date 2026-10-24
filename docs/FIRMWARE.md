# Firmware images

A firmware image replaces the whole root file system of the player. Most
people do not need one: on the R3 Pro, Mellowtone installs on `/data` without
flashing (`boards/r3pro/launcher/INSTALL.md`), and every board takes player
updates (`mellowtone-<board>-<ver>.tar`, made by
`tools/release/make_update_package.sh`) from the card through the launcher's
recovery menu. An image is needed for the first install on the X1600 boards,
and to change anything outside the player itself (the factory copy, the
firmware overlay).

Every image is built **from your own copy of the official HiBy firmware**.
It contains HiBy's software, so **images are never published** and must not
be committed or uploaded: everybody builds their own, on their own computer.
Player update packages contain only our own files and are published.

Before you flash anything: charge the battery, keep the official firmware for
your model on your computer so you can go back to it, and use only an image
built for your exact model.

## R3 Pro

The recipe unpacks HiBy's `r3pro.upt`, takes out the stock player, adds
Mellowtone, and packs it again in exactly the format the device's updater
expects. The images pass every check below but have not been flashed on
hardware by this project yet.

### What you need

- The official R3 Pro firmware **2.2** (`r3pro.upt`, 51,230,720 bytes, SHA-256
  `266ccc235564ccc84f6d67d9529a6ee62b67ef677e7b7162570a6712433ce1f2`).
  Any other file is refused.
- The containers: `./mt setup image` (and `build`, `bluez`).
- For the full image:
  - the player and launcher: `./mt build r3pro`;
  - the patched `bluetoothd`: `./mt bluez` (needs `MT_INGENIC_TC` and
    `MT_BT_SYSROOT`, see `docs/BUILDING.md`);
  - the Noto fonts listed in `boards/r3pro/image/fonts.sha256`, in one
    directory, passed with `--fonts DIR` (or `MT_FONTS_DIR` in `.mt.local`).

### Commands

```sh
./mt image r3pro --stock ~/hiby/r3pro.upt --null       # stock content, repacked by us
./mt image r3pro --stock ~/hiby/r3pro.upt --minimal    # + /etc/mellowtone-test
./mt image r3pro --stock ~/hiby/r3pro.upt --bt-only    # + our bluetoothd
./mt image r3pro --stock ~/hiby/r3pro.upt --fonts ~/fonts   # Mellowtone
```

Options: `--out DIR` (default `out/images/`, which git ignores), `--version
VER` (default `dev-<date>-<commit>`, with `-dirty` for uncommitted changes).

Each run writes three files, and only when every check passed:

| File | What |
|---|---|
| `mellowtone-r3pro-<ver>[-null\|-minimal\|-bt-only].upt` | The image |
| `….report.txt` | The check results (below) and the library-removal reasoning |
| `….manifest.txt` | Commit, container image, tool versions, stock SHA-256, `mkfs.ubifs` parameters, the ISO files and every input with SHA-256, the change list, and the whole rootfs (type, mode, owner, size, SHA-256 or link target) |

`./mt image` refuses to write to a memory card or a removable mount. Keep
images on your computer until you flash one (next section).

The four kinds exist so a new image is tried one step at a time: first
the official image itself, then `--null` (proves our
packing), `--minimal` (proves a change survives), `--bt-only`, and only then
the full image.

### What the full image changes

- **Removed:** `/usr/bin/hiby_player` and the libraries only it needs. These
  are found from the `NEEDED` entries of every ELF file left in the rootfs; a
  library another binary needs, or that any remaining file names (a
  `dlopen`), stays. On firmware 2.2 that removes `/usr/lib/libqrencode.so`.
  The decoder libraries the stock player loads by name (`libaac.so`,
  `libmp3.so`, …) are listed for review in the report but kept.
- **Replaced:** `/usr/bin/hiby_player.sh` (what init starts) by Mellowtone's
  init shim, `/usr/sbin/bluetoothd` by ours. Both keep the stock owner and mode.
- **Added, `/usr/mellowtone/`:**
  - `factory/mellowtone`, `factory/mellowtone-launcher`, `factory/VERSION`:
    the factory copy the launcher falls back to;
  - `themes/`: the theme (`boards/r3pro/tools/make_theme.sh`, stock art filling
    what the repository does not ship);
  - `fonts/`: Noto Sans and Noto Sans Hebrew;
  - `resource/ot_devices_0.json`, `ot_devices_1.json`: the stock volume
    tables for the gain modes (the stock player used to pick one at its first
    start; Mellowtone now picks by the kernel's `dac_choice`: ES9218 or CS43131).
- **Unchanged:** everything else, including `sys_server`, the Bluetooth ALSA
  plugin, `bt_codec`, the Wi-Fi scripts, `recovery_all.sh`, `/usr/resource`
  (stock fonts, `bt_name`, `hostname`) and the kernel.

Mellowtone tells the two installs apart at run time: when
`/usr/mellowtone/factory` exists it reads its theme, fonts and volume tables
from `/usr/mellowtone`; otherwise (the player-bundle install) from its
install root `/data/mellowtone`. In both cases the installed version
(`/data/mellowtone/current`) and the boot state (`/data/mellowtone-boot`)
are on flash; the previous version (`SD/.mellowtone/backup/player`),
settings and the library are on the card.

### The checks

| | Check |
|---|---|
| B1 | The input is firmware 2.2 (SHA-256) |
| B2 | The kernel in the image is HiBy's, bit for bit |
| B3 | The UBIFS superblock equals the stock one in every field but the used LEB count and the UUID; the rest of LEB 0 is erased as in stock; the file system fits the 488-LEB `rootfs` volume |
| B4 | The rootfs is unpacked again and compared with the stock rootfs plus the change list: every path, type, mode (setuid included), owner, size, SHA-256, link target, device number, extended attribute, inode flag, link count and time. Any difference fails |
| B5 | Every ELF file is MIPS32 little-endian, its interpreter and every `NEEDED` library exist in the rootfs; the new binaries run under `qemu-mipsel` (`--version`, `bluetoothd -v`). Problems an unchanged stock file already has are listed, not failed |
| B6 | Changed shell scripts pass `sh -n` and `shellcheck` |
| B7 | The ISO has the stock files in the stock order (plain ISO 9660, volume id `CDROM`, no Rock Ridge or Joliet); `UPDATE.TXT` and `VERSION.TXT` keep the stock text apart from the md5 values and `ver=`; the md5 values match; the rootfs fits the volume |
| B8 | The image is built twice from clean trees and the two must be bit-identical. The same commit, inputs and `SOURCE_DATE_EPOCH` give the same image on another run too |

### How the rootfs keeps its exact metadata

Every file in the stock rootfs belongs to uid/gid 1009 (HiBy's build user),
with its own mode and times; a plain unpack would lose that.
`ubireader_extract_files` is not used for unpacking: it cannot set symlink
owners or times, ignores extended attributes, and needs real root for device
nodes. Instead `boards/r3pro/image/ubifs_tree.py` reads the UBIFS image node
by node (checking every node's CRC), writes the content to a tmpfs inside
the container in a fixed order, and keeps the metadata in a manifest. Running
as root in the container (your own user under rootless podman), it gives
every file its exact owner and mode; device nodes and FIFOs would go to
`mkfs.ubifs` through a device table (`-D`; the stock rootfs has none).
`mkfs.ubifs` runs with the stock parameters (`-m 2048 -e 126976 -c 480 -x lzo
-f 8 -l 4 -p 1 -k r5 -j 7491584`). Afterwards the recipe writes the stock
access/change/modification times and a UUID derived from the inputs into the
image's inode nodes and superblock (recomputing their CRCs): `mkfs.ubifs`
copies the change time from the host, which cannot be set, and picks a random
UUID. B4 then proves the result.

### Flashing (outline)

Only with a charged battery and your backups made, and after trying the
images in the order above:
copy the image to the card root as `r3pro.upt` (or `update.upt`) and start the
update from the stock player's firmware update or Mellowtone's recovery menu
("Install firmware r3pro.upt"). The image lists only the kernel and the rootfs;
`/data` is a separate partition and not part of it (that the updater leaves
it alone is expected but not yet proven, so keep the backup). To go back, do
the same with HiBy's own `r3pro.upt`.

## R1, R3 Pro II and R3 II (2025)

These images are not tested on hardware by this project.

`boards/common/repack_upt.sh` turns a base image and one board's player and
launcher into an ISO 9660 `.upt` in the format the device's recovery updater
expects. It works in a temporary directory and changes neither the base nor
the source tree; the output path must differ from the base.

### The base image

The base is not the plain HiBy download: it is the official firmware for the
same model, prepared with

- the stock player removed and a player line in `/usr/bin/hiby_player.sh`
  that the packer can point at Mellowtone (the R1 base must already hand over
  to a launcher there, not start the stock player);
- the BlueALSA 5 / BlueZ runtime and the Speex rate-conversion files the
  player needs, built by the recipes in `boards/common/base-image/`
  (`README.md` there) and `boards/common/prepare_*overlay.sh`.

On the R3 Pro II and R3 II (2025) the Bluetooth runtime can instead come from
a separate overlay directory, made by
`boards/common/prepare_r3_pro_ii_bluetooth_overlay.sh` and passed as
`R3_RUNTIME_OVERLAY=<dir>` to the packer; without it or an upgraded base, the
packer stops with "runtime gate failed".

There are no step-by-step instructions for preparing a whole base yet. The packer
checks what it can and stops if any check fails:

| Board | `--board` | Identity check | Panel |
| --- | --- | --- | --- |
| HiBy R1 | `r1` | launcher hand-over in `hiby_player.sh` | 480×800 |
| HiBy R3 Pro II | `r3proii` | `"device": "R3PROII"` in `/usr/resource/config.json` | 480×720 |
| HiBy R3 II (2025) | `r3ii_2025` | `"device": "R3II_2025"` in `/usr/resource/config.json` | 320×480 |

It also checks the required runtime files, the firmware fonts (including the
untracked `Korean.ttf`) against their checksums, and the kernel and rootfs
chunks. Keep the base unchanged as your recovery image.

### What you need

Linux with `7z`, `unsquashfs`, `mksquashfs`, `genisoimage`, `file`,
`md5sum`, `sha256sum` and `split`. On Debian or Ubuntu:

```sh
sudo apt-get install genisoimage p7zip-full squashfs-tools file
```

and the player and launcher for the same board (`docs/BUILDING.md`):

| Board | Build | Player | Launcher |
| --- | --- | --- | --- |
| R1 | `./mt build r1` | `build/r1/mellowtone-r1` | `build/r1/mellowtone-launcher` |
| R3 Pro II | `./mt build r3proii` | `build/r3proii/mellowtone-r3proii` | `build/r3proii/mellowtone-launcher` |
| R3 II (2025) | `./mt build r3ii_2025` | `build/r3ii_2025/mellowtone-r3ii_2025` | `build/r3ii_2025/mellowtone-launcher` |

Do not mix binaries between boards.

### Commands

```sh
boards/common/repack_upt.sh --board r1 \
  /path/to/r1-base.upt build/r1/mellowtone-r1 build/r1/mellowtone-launcher out/r1.upt

boards/common/repack_upt.sh --board r3proii \
  /path/to/r3proii-base.upt build/r3proii/mellowtone-r3proii \
  build/r3proii/mellowtone-launcher out/r3proii.upt

boards/common/repack_upt.sh --board r3ii_2025 \
  /path/to/r3ii_2025-base.upt build/r3ii_2025/mellowtone-r3ii_2025 \
  build/r3ii_2025/mellowtone-launcher out/r3ii_2025.upt
```

On success the script prints the output size and SHA-256. The image must fit
45 MiB (47,185,920 bytes); the script refuses a larger one.

### What the image changes

- **Added, `/usr/mellowtone/`:** the factory copy: the player as
  `mellowtone`, `mellowtone-launcher`, and the init shim
  `mellowtone-shim.sh` (from `boards/<board>/launcher/`).
- **Changed:** the player line of `/usr/bin/hiby_player.sh` now runs the
  shim. Older `/usr/bin/<name>_bootloader` files and the
  `/usr/bin/<name>_player` they started are removed.
- **Copied over the base**, in this order, so later ones win:

| Repository path | On the device |
| --- | --- |
| `assets/themes/` | `/usr/resource/litegui/theme2/` (the theme the player reads) |
| `boards/<board>/assets/` | the same paths: board artwork, including the boot images |
| `assets/fonts/` | `/usr/resource/fonts/` |
| `boards/common/overlay/` | `/`: the NTP helper, the udhcpc NTP and DNS hooks, the Speex rate converter |

Only files tracked by git are copied, so a file you have locally but did not
commit does not ship, and a local stock-firmware dump (git-ignored) never
does. Stock art that the repository does not replace comes from the base. To
list exactly what will be copied:

```sh
git ls-files assets/themes assets/fonts boards/r1/assets boards/r3proii/assets boards/r3ii_2025/assets
find boards/common/overlay \( -type f -o -type l \) -print | sort
```

### Checking the result

```sh
file out/r1.upt                  # ISO 9660
7z t out/r1.upt                  # no errors
7z l out/r1.upt | sed -n '1,40p'
```

The image holds `mellowtone.in` (`board=<board>`; the recovery menu refuses
an image for another board), `ota_config.in` (with `current_version=0`),
`ota_v0/ota_update.in` (the kernel and rootfs images with their sizes and
MD5), `ota_v0/ota_v0.ok`, the `xImage` and `rootfs.squashfs` chunks and their
`ota_md5_*` files.

### Flashing

Copy the image to the card root and install it from Mellowtone's recovery menu
("Install firmware <file name>") or the stock firmware update. To go back,
install your base or the official firmware the same way.
