/* HiBy R3 Pro II (Ingenic X1600). */
#include "board.h"
#include "board_launcher.h"
#include "board_config.h"
#include "mt_features.h"

#include <linux/input.h>

#define SCREEN_W 480
#define SCREEN_H 720
_Static_assert(SCREEN_W == BOARD_SCREEN_WIDTH && SCREEN_H == BOARD_SCREEN_HEIGHT,
               "board.c and board_config.h disagree on the screen size");

#define CAPS (CAP_AUDIO_DAC |                                                                      \
              /* CAP_GAIN_MODES: ? verify on device; no native gain modes today */                 \
              CAP_BT_AUDIO_OUT | CAP_BT_RECEIVER | CAP_WIFI |                                      \
              /* CAP_WIFI_5GHZ: ? verify on device */                                              \
              CAP_USB_STORAGE | CAP_USB_DAC |                                                      \
              /* CAP_USB_DAC_DSD: ? verify on device; the USB DAC bridge is PCM only */             \
              CAP_USB_HOST_AUDIO | /* ? verify on device; usb_audio_output.c today */              \
              CAP_CHARGE_CONTROL |                                                                 \
              /* CAP_INPUT_CURRENT: ? verify on device */                                          \
              /* CAP_BATTERY_CURRENT: ? verify on device */                                        \
              CAP_LEDS | /* ? verify on device; LED charge indicator offered today */              \
              CAP_INLINE_REMOTE | CAP_TOUCH | CAP_KEYS)
BOARD_CHECK_FEATURE_REQUIREMENTS(CAPS);

static const board_input_device_t buttons[] = {
    { "md-gpio-keys", false },
    { "jz adc keyboard", false },
    { "earpods_adc", false }, /* wired headphone in-line remote */
    { NULL, false },
};

static const char * const touch[] = { "GT9XX-TS", "hyn_ts", "goodix-ts", NULL };

const board_desc_t BOARD = {
    .name = "r3proii",
    .model = "HiBy R3 Pro II",
    .screen = { .w = SCREEN_W, .h = SCREEN_H, .rotation = 0, .scale = 100 },
    .paths = {
        .install_root = "/usr/data",
        .sd_root = "/data/mnt/sd_0",
        .theme_root = "/usr/resource/litegui/theme2/", /* stock theme + our assets/themes (repack_upt.sh) */
        .inline_remote_sw = "/sys/devices/platform/earpods_adc/earpods_adc/earpods_adc_sw",
        .font_cjk = "/usr/resource/fonts/cjk_cyrillic.ttf",
    },
    .input = {
        .buttons = buttons, .touch = touch, .keymap = NULL, .menu_key = KEY_VOLUMEUP,
        .power_key_device = "md-gpio-keys", .menu_key_device = "jz adc keyboard",
    },
    .alsa = {
        .card = 0, .device = 0,
        .mixer_volume_left = "Left Playback Volume",
        .mixer_volume_right = "Right Playback Volume",
        .mixer_dop_switch = "DOP_EN",
        .mixer_output_port = "Output Port Switch",
        .volume_curve = NULL,
    },
    /* The battery node's status is stuck at "Discharging" while charging;
     * the MP2731 charger IC's node reports it correctly. */
    .power = {
        .battery_node = "battery",
        .charger_node = "mp2731-charger",
        .usb_node = NULL,
        .quirks = BOARD_POWER_STATUS_FROM_CHARGER,
        .charge_chips = BOARD_CHARGER_AXP2101 | BOARD_CHARGER_MP2731,
        .axp_voltage_baseline = 5, /* stock axp2101.sh charge_voltage_limit=4400 */
        .charge_method = BOARD_CHARGE_METHOD_VOLTAGE_CAP,
    },
    .leds = { [BOARD_LED_RED] = "red", [BOARD_LED_BLUE] = "blue" },
    .caps = CAPS,
    .charger = &charger_axp2101_i2c_ops,
    .bt_platform = &bt_platform_scripts_ops, /* stock bt_resume/bt_enable/bt_disable */
    /* Card mount, version slots, <board>.upt through bootmode.sh. */
    .launcher = &launcher_x1600_ops,
    /* configfs gadgets through the stock scripts; the card stays mounted while
     * a computer has it (usb_mode_control.c's X1600 behaviour). */
    .usb_gadget = &usb_gadget_configfs_ops,
    .storage_export = &storage_export_shared_ops,
    .usb = { .uac_abi = BOARD_UAC_SA_ABI_READ_EPERM },
    /* The stock scripts bring up wlan0 + wpa_supplicant (and a first udhcpc). */
    .wifi = {
        .backend = BOARD_WIFI_WPA_SCRIPTS, .iface = "wlan0", .ctrl_dir = "/var/run/wpa_supplicant",
        .power_on = "/usr/bin/wifi_on.sh", .power_off = "/usr/bin/wifi_off.sh",
    },
};
