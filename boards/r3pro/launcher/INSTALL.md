# R3 Pro: the bundle install (mellowtone-launcher over ADB)

The bundle install puts Mellowtone on the writable `/data` partition and
leaves the rootfs as HiBy ships it, except for one symlink: the stock
`S92_03_start_music_player` runs `/ui_data/player` when it exists, otherwise
`/usr/bin/hiby_player.sh`. `/ui_data` points to `/data/mellowtone/ui_data`,
where the init shim `boards/r3pro/launcher/player` runs `mellowtone-launcher`.
Nothing here flashes anything. The image install
(`hiby_player.sh` shim, factory copy in `/usr/mellowtone/factory/`, theme,
fonts and volume tables in `/usr/mellowtone/`) is built by `./mt image r3pro`
(`docs/FIRMWARE.md`).

## Layout on the device

| | Path |
|---|---|
| Started by | `/ui_data/player` = the shim, which runs the launcher |
| Install root (`BOARD.paths.install_root`, the player's working directory) | `/data/mellowtone`: `themes/`, `fonts/`, `Radio.txt`, `alsa/`, `lib/`, `adb_on` |
| Player binary | `/data/mellowtone/current/mellowtone` (the one installed version) |
| Previous version | on the card: `SD/.mellowtone/backup/player/` (saved at each update, checked by SHA-256) |
| Fixed launcher copy (the shim's fallback) | `/data/mellowtone/mellowtone-launcher` |
| Fallback | 3 quick crashes or 3 short boots: roll back to the version on the card, then the stock player (`/usr/bin/hiby_player`), then the recovery menu |
| Vol- held at power-on | opens the recovery menu |
| Boot counter | `/data/mellowtone-boot/boot_attempts` (2 writes per boot) |
| ADB flag | `/data/mellowtone/adb_on` (`BOARD.paths.adb_flag`: the player's ADB switch and the launcher agree) |
| One-shot menu flag | `/data/mellowtone-boot/open_menu` (the player's "Restart into recovery menu") |
| Logs | `SD/.mellowtone/logs/boot.log`, `player.out`, `update.log` (`/tmp` without a card) |
| Debug flag (power log) | `SD/.mellowtone/config/debug_logging` (Developer Options > Enable debug logging) |
| NTP, USB device-mode fix, ADB watch | in the launcher |
| Firmware update | recovery menu > Install firmware (r3pro.upt / update.upt) |

## Install (over ADB)

Build first: `./mt build r3pro` gives `build/r3pro/mellowtone-r3pro` and
`build/r3pro/mellowtone-launcher`. The theme comes from
`boards/r3pro/tools/make_theme.sh`; the fonts are listed in
`boards/r3pro/image/fonts.sha256`.

```sh
# 1. The install root, the current slot and a fixed launcher copy.
adb shell 'mkdir -p /data/mellowtone/current /data/mellowtone/ui_data'
adb push build/r3pro/mellowtone-r3pro /data/mellowtone/current/mellowtone
adb push build/r3pro/mellowtone-launcher /data/mellowtone/current/mellowtone-launcher
adb push build/r3pro/mellowtone-launcher /data/mellowtone/mellowtone-launcher
adb shell 'cd /data/mellowtone && chmod 755 current/mellowtone current/mellowtone-launcher mellowtone-launcher &&
           date +dev-%Y%m%d > current/VERSION && sync'
# Theme and fonts: push the make_theme.sh output to /data/mellowtone/themes/
# and the fonts to /data/mellowtone/fonts/.

# 2. The shim, swapped in atomically, and the hook.
adb push boards/r3pro/launcher/player /data/mellowtone/ui_data/player.new
adb shell 'chmod 755 /data/mellowtone/ui_data/player.new &&
           mv /data/mellowtone/ui_data/player.new /data/mellowtone/ui_data/player && sync'
# The hook, the only rootfs change (remount / read-write first if it is not).
adb shell 'ln -sfn /data/mellowtone/ui_data /ui_data && sync'

# 3. Restart and check.
adb shell reboot
adb shell 'ps | grep -E "mellowtone|hiby_player" | grep -v grep; tail -n 30 /mnt/sd_0/.mellowtone/logs/boot.log'
```

Later player versions: `tools/release/make_update_package.sh r3pro <version> <slot dir>
<out dir>`, copy the `.tar` to `SD/.mellowtone/update/`, then About > "Player
update available" (or restart); the recovery menu offers "Install player
update". The install checks the package, saves the installed version to the
card (read back and checked), then replaces it in flash, so flash needs room
for one version only. If unpacking fails, or power is cut, the version on the
card comes back. "Roll back to <version>" swaps the two. During development, push to
`/data/mellowtone/current/mellowtone` and touch `/tmp/mellowtone_redeploy`
before killing the player: the launcher then restarts it without counting a
crash.

## Rollback

```sh
adb shell 'rm /ui_data && sync && reboot'
```

Without `/ui_data` the stock player starts. `/data/mellowtone*` can stay or go
(`rm -rf /data/mellowtone /data/mellowtone-boot`).

If ADB is not reachable:
- **The launcher runs:** hold **Vol-** while powering on, then "Enable ADB" in the
  recovery menu (it also writes `/data/mellowtone/adb_on`).
- **The launcher is missing or crashes:** the shim starts the stock player
  (`/usr/bin/hiby_player.sh`), then `K90adb start`.
- **Nothing starts:** the official `r3pro.upt` on the card and U-Boot's recovery
  restore the rootfs, which removes `/ui_data`; `/data` is not
  part of the image.

## Checks on the device (first run)

1. Normal boot: `boot.log` shows `dwc2: ... -> device mode`, `usb: ...` lines,
   `chain: starting current`; after 90 s `/data/mellowtone-boot/boot_attempts` is gone.
2. Wi-Fi on: `ntp: clock set` in `boot.log`.
3. Vol- held at power-on: the menu, readable (rotation, both SLCD pages, no
   shifted picture); Vol+/Vol- move, Play selects, Power goes back; touch selects
   a row.
4. Developer Options > Restart into recovery menu: the menu opens without a
   reboot; "Start Mellowtone" returns to the player.
5. Device info shows the versions, battery and card.
6. Enable ADB from the menu with ADB off: `adb devices` sees the device.
7. Crash fallback: `adb shell 'kill -SEGV $(pidof mellowtone)'` three times
   within a minute: the version on the card is rolled back and starts (the
   crashing one takes its place on the card), or the stock player starts when
   the card has none; "automatic rollback" in `update.log`.
8. Firmware item, read-only first: with the official `r3pro.upt` on the card the
   menu offers it; check the shown model (r3pro), version and battery, run the
   md5 check, then **Cancel** at the confirmation (flash only as `docs/FIRMWARE.md` describes).
9. Power off and reboot from the menu.
