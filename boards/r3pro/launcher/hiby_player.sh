#!/bin/sh
# Mellowtone init shim, R3 Pro image install: replaces /usr/bin/hiby_player.sh,
# which S92_03_start_music_player runs. The launcher supervises
# the player and returns only when the system shuts down (exit 0). Missing or
# crashed: the factory player directly; that failing too: ADB, the way in.
for l in /data/mellowtone/current/mellowtone-launcher /usr/mellowtone/factory/mellowtone-launcher; do
    [ -x "$l" ] && { "$l"; rc=$?; [ $rc -eq 0 ] && exit 0; echo "$(cut -d' ' -f1 /proc/uptime) shim: $l exit $rc" >> /tmp/mellowtone-shim.log; }
done
cd /usr/mellowtone && /usr/mellowtone/factory/mellowtone
/etc/init.d/K90adb start
