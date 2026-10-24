#!/bin/sh
# Mellowtone init shim, X1600 images (R1, R3 Pro II, R3 II 2025): the stock
# /usr/bin/hiby_player.sh runs it in place of the stock player
# (boards/common/repack_upt.sh). The launcher supervises the player and
# returns only when the system shuts down (exit 0). Missing or crashed: the
# factory player directly; that failing too: ADB, the way in.
for l in /data/mellowtone/current/mellowtone-launcher /usr/mellowtone/mellowtone-launcher; do
    [ -x "$l" ] && { "$l"; rc=$?; [ $rc -eq 0 ] && exit 0; echo "$(cut -d' ' -f1 /proc/uptime) shim: $l exit $rc" >> /tmp/mellowtone-shim.log; }
done
/usr/mellowtone/mellowtone
/etc/init.d/adb/S440adb start; sleep 2; /sbin/usb_adb_enable.sh
