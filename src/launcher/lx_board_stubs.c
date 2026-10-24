/* boards/<board>/board.c points at the player's back ends (charger,
 * Bluetooth platform, USB gadget and storage). The launcher reads only the
 * descriptor's data and BOARD.launcher, so these are named, empty tables:
 * the player's code stays out of the launcher. */
#include "charger.h"
#include "bt_backend.h"
#include "usb_gadget.h"

const struct board_charger_ops charger_axp2101_i2c_ops = { .name = "axp2101-i2c" };
const struct board_charger_ops charger_sysfs_ops = { .name = "power_supply-sysfs" };
const struct board_bt_platform_ops bt_platform_hiby_ops = { .name = "hiby-sys_server" };
const struct board_bt_platform_ops bt_platform_scripts_ops = { .name = "board-scripts" };
const struct board_usb_gadget_ops usb_gadget_configfs_ops = { .name = "configfs" };
const struct board_usb_gadget_ops usb_gadget_android_usb_ops = { .name = "android_usb" };
const struct board_storage_export_ops storage_export_shared_ops = { .name = "shared" };
const struct board_storage_export_ops storage_export_sys_server_ops = { .name = "sys_server", .lends_card = true };
