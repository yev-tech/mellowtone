/* The descriptor's charger back ends, as empty tables: test_board checks
 * which one a board points at, not what they do (tests/power does). */
#include "charger.h"

const struct board_charger_ops charger_axp2101_i2c_ops = { .name = "axp2101-i2c" };
const struct board_charger_ops charger_sysfs_ops = { .name = "power_supply-sysfs" };

/* The Bluetooth platforms, likewise (tests/bluetooth tests the back ends). */
#include "bt_backend.h"

const struct board_bt_platform_ops bt_platform_hiby_ops = { .name = "hiby-sys_server" };
const struct board_bt_platform_ops bt_platform_scripts_ops = { .name = "board-scripts" };

/* The USB gadget and storage back ends, likewise (tests/usb tests them). */
#include "usb_gadget.h"

const struct board_usb_gadget_ops usb_gadget_configfs_ops = { .name = "configfs" };
const struct board_usb_gadget_ops usb_gadget_android_usb_ops = { .name = "android_usb" };
const struct board_storage_export_ops storage_export_shared_ops = { .name = "shared" };
const struct board_storage_export_ops storage_export_sys_server_ops = { .name = "sys_server", .lends_card = true };
