/* HiBy R3 Pro (Ingenic X1000), stock HiBy base system. */
#include "board.h"
#include "board_launcher.h"
#include "board_config.h"
#include "mt_features.h"

#include <dirent.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define SCREEN_W 360
#define SCREEN_H 480
_Static_assert(SCREEN_W == BOARD_SCREEN_WIDTH && SCREEN_H == BOARD_SCREEN_HEIGHT,
               "board.c and board_config.h disagree on the screen size");

/* From the on-device capability probe. Some listed caps are not yet
 * implemented by the player -- bugs (a listed capability must be fully
 * supported), not reasons to drop the bit; they are listed in IMPL_MISSING below. */
#define CAPS (CAP_AUDIO_DAC | CAP_GAIN_MODES | CAP_BT_AUDIO_OUT |                                  \
              /* CAP_BT_RECEIVER: ? verify on device; no receiver mode on BlueZ 4 yet */             \
              CAP_WIFI |                                                                           \
              CAP_WIFI_5GHZ | /* verified 2026-10-05: scan shows 5180/5200 MHz networks */         \
              CAP_USB_STORAGE | CAP_USB_DAC | CAP_USB_DAC_DSD |                                    \
              CAP_USB_HOST_AUDIO | /* ? verify on device; usb_audio_output.c today */              \
              /* CAP_CHARGE_CONTROL: ? verify on device: no known way to stop charging;             \
                 the axp_battery chgmilli* candidate in charger_sysfs.c needs the reversible     \
                 write test from the capability probe first */                                   \
              CAP_INPUT_CURRENT | CAP_BATTERY_CURRENT | CAP_LEDS |                                 \
              CAP_INLINE_REMOTE | /* earpods_adc.0 + its input device (capability probe); test   \
                                     with a remote headset */                                     \
              CAP_TOUCH | CAP_KEYS)
BOARD_CHECK_FEATURE_REQUIREMENTS(CAPS);

/* Listed caps the player does not implement on this board yet;
 * their UI stays hidden through board_impl_missing() until then. */
/* DSD over USB: implemented (DSD-to-DoP in the UAC bridge) but not yet
 * tried on the device. */
#define IMPL_MISSING (CAP_USB_DAC_DSD |                                                          \
                      CAP_BATTERY_CURRENT) /* battery_get_current_ua() reads axp_battery/current_now;
                                              no UI shows it yet, and the probe read 0
                                              while full: confirm while charging/discharging */
_Static_assert((IMPL_MISSING & ~CAPS) == 0, "IMPL_MISSING lists a capability the board does not have");

static const board_input_device_t buttons[] = {
    { "md-gpio-keys", false },
    { "jz adc keyboard", false },
    { "earpods_adc", false }, /* wired headphone in-line remote */
    /* POWER + PREVIOUSSONG on gpio-keys; NEXTSONG, PLAYPAUSE and
     * VOLUMEUP/DOWN on adc_key. */
    { "gpio-keys", false },
    { "adc_key", false },
    { NULL, false },
};

static const char * const touch[] = { "GT9XX-TS", "hyn_ts", "goodix-ts", NULL };

/* The stock player's USB input-current policy (hiby_player FUN_0046e820),
 * applied by the sysfs charger back end: without it the charger keeps its
 * boot defaults when Mellowtone is the boot player. */
static const board_input_current_policy_t input_current = {
    .gadget_state = "class/android_usb/android0/state",
    .gadget_max_power = "class/android_usb/android0/MaxPower",
    .host = { .max_power = 500, .limit_max = 500, .current_max = 500 },    /* computer: CONFIGURED/CONNECTED */
    .charger = { .max_power = 500, .limit_max = 0, .current_max = 1200 }, /* usb online, no enumeration */
    /* USB DAC mode with "Don't charge" on: the stock dac_charge_disable row
     * (FUN_0046e820). */
    .has_dac_no_charge = true,
    .dac_no_charge = { .max_power = 10, .limit_max = 100, .current_max = 0 },
};

/* The stock player's android_usb set-ups (system_if_usb_select_device
 * FUN_0046dda0). idle = K90adb's. */
static const board_android_usb_t android_usb = {
    .gadget_dir = "class/android_usb/android0",
    .dac = { .id_vendor = "32BB", .id_product = "0004", .device_class = "239", .device_subclass = "2",
             .device_protocol = "1", .functions = "uac_sa,hid" },
    .storage = { .id_vendor = "32BB", .id_product = "0101", .device_class = "0", .device_subclass = "0",
                 .device_protocol = "0", .functions = "mass_storage" },
    .idle = { .id_vendor = "18d1", .id_product = "d002", .device_class = "0", .device_subclass = "0",
              .device_protocol = "0", .functions = "adb" },
    .manufacturer = "HiBy", .product = "R3PRO", .serial = "HiBy R3PRO",
    .uac_string_assoc = "class/android_usb/f_uac_sa/string_assoc",
    .uac_string_assoc_value = "HiBy R3PRO",
    .inquiry_vendor = "HiBy", .inquiry_product = "R3PRO U-DISK",
};

/* The gain modes' volume table on an image install. The stock player
 * renames /usr/resource/ot_devices_<n>.json to ot_devices.json on its first
 * start (hiby_player FUN_0046e4e0); on an image install it never runs, so
 * pick the same way: the kernel's dac_choice attribute (somewhere under
 * /sys/devices/platform) reading "1" is the CS43131 variant
 * (ot_devices_1.json), anything else the ES9218 one (ot_devices_0.json). The
 * image recipe copies both tables to /usr/mellowtone/resource/. */
static bool find_attr(const char * dir, const char * name, int depth, char * out, size_t size) {
    DIR * d = opendir(dir);
    if (!d) return false;
    bool found = false;
    struct dirent * e;
    while (!found && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[256];
        if (snprintf(p, sizeof(p), "%s/%s", dir, e->d_name) >= (int) sizeof(p)) continue;
        if (strcmp(e->d_name, name) == 0) found = snprintf(out, size, "%s", p) < (int) size;
        else if (e->d_type == DT_DIR && depth > 0) found = find_attr(p, name, depth - 1, out, size);
    }
    closedir(d);
    return found;
}

static const char * r3pro_volume_curve(char * buf, size_t size) {
    char attr[256], v[8] = "";
    if (find_attr("/sys/devices/platform", "dac_choice", 3, attr, sizeof(attr))) {
        FILE * f = fopen(attr, "r");
        if (f) {
            if (!fgets(v, sizeof(v), f)) v[0] = '\0';
            fclose(f);
        }
    }
    snprintf(buf, size, "/usr/mellowtone/resource/ot_devices_%c.json", v[0] == '1' ? '1' : '0');
    return access(buf, R_OK) == 0 ? buf : NULL;
}

/* The stock BlueZ 4 / Wi-Fi / USB stack stays HiBy's (subprocess.c). Wi-Fi
 * needs no command: control socket, and sys_server for power. */
static const char * const exec_allow[] = {
    "aplay", "ps", "pgrep", "cp", "mkdir",
    /* stock BlueZ 4 tooling, used by network/bluetooth/bt_platform_hiby.c
     * exactly as the stock sys_server uses it */
    "bt-adapter", "bt-device", "bt-connect", "bt_codec", "bt_enable", "bt_disable",
    /* the stock suspend-to-RAM pair (hardware/power_suspend.c), only run
     * when Developer Options allows suspend */
    "bt_suspend", "bt_resume",
    /* stock ADB gadget script (Mellowtone ADB toggle) */
    "K90adb",
    NULL,
};

const board_desc_t BOARD = {
    .name = "r3pro",
    .model = "HiBy R3 Pro",
    /* The panel is mounted upside down: the stock UI writes its frames
     * rotated 180 degrees ("Screen upside down" flips back from this).
     * Montserrat reads larger than the stock UI's font at the same pixel
     * size; 70% gives title/row/body/subtext 20/16/14/12 px at the Small
     * tier. jzfb (smart LCD) blank/unblank reprograms DMA registers
     * mid-transfer and can misalign the panel until reboot. */
    .screen = { .w = SCREEN_W, .h = SCREEN_H, .rotation = 180, .scale = 70, .no_fb_blank = true,
                .fb_sleep = true },
    .paths = {
        .install_root = "/data/mellowtone",
        /* Firmware-image install (boards/r3pro/image/): theme, fonts and the
         * volume tables in the rootfs, the factory player in factory/. */
        .image_root = "/usr/mellowtone",
        .sd_root = "/mnt/sd_0",
        /* Assembled by boards/r3pro/tools/make_theme.sh and installed with the player. */
        .theme_root = "/data/mellowtone/themes/",
        /* The stock exFAT driver returns EIO when writing to an
         * open-but-unlinked file: the scan spool lives in RAM. */
        .scan_spool_dir = "/tmp",
        .inline_remote_sw = "/sys/devices/platform/earpods_adc.0/earpods_adc/earpods_adc_sw",
        .stock_gain_ini = "/data/user.ini",
        .launcher_debug_flag = "/data/mellowtone/debug_logging",
        /* In the install root, where the player's ADB switch and the
         * launcher both read it. */
        .adb_flag = "/data/mellowtone/adb_on",
        /* Stock UI font: Noto Sans SC DemiLight (Latin, Cyrillic, CJK; CFF);
         * msyh.ttf on this firmware is a reduced HeitiUI without CJK. The
         * primary UI face is Noto Sans, Latin/Greek/
         * Cyrillic, installed with the player; default.otf then serves CJK
         * only. A missing file falls back to the stock face. */
        .font_cjk = "/usr/resource/fonts/default.otf",
        .font_ui = "/data/mellowtone/fonts/NotoSans-Regular.ttf",
        .font_hebrew = "/data/mellowtone/fonts/NotoSansHebrew-Regular.ttf",
    },
    /* Vol- held at power-on opens the launcher's recovery menu (the shell
     * launcher used it to switch players). U-Boot's own recovery key is an
     * ADC key not identified yet. */
    .input = {
        .buttons = buttons, .touch = touch, .keymap = NULL, .menu_key = KEY_VOLUMEDOWN,
        .power_key_device = "gpio-keys", .menu_key_device = "adc_key",
        .key_poll_ms = 50, /* hw_buttons' thread queues presses; halves idle wakeups */
    },
    .alsa = {
        .card = 0, .device = 0,
        .mixer_volume_left = "Left Playback Volume",
        .mixer_volume_right = "Right Playback Volume",
        .mixer_dop_switch = "DOP_EN",
        .mixer_output_port = "Output Port Switch",
        /* The stock player's HDB/MDB/LDB tables (audio_load_board_volume_curve()). */
        .volume_curve = "/usr/resource/ot_devices.json",
        .volume_curve_select = r3pro_volume_curve,
        .aplay_nice = -10,
    },
    /* No AXP2101 on /dev/i2c-0: the PMIC is axp_mfd behind a kernel driver,
     * so the charger is driven through power_supply sysfs. Events: uevents,
     * and early_mount's CHARGE:* messages on our sys_client socket. */
    .power = {
        .battery_node = "battery", .charger_node = NULL, .usb_node = "usb",
        .quirks = BOARD_POWER_NO_SUSPEND | BOARD_POWER_BT_RESUME_SCRIPT, .charge_chips = 0, .axp_voltage_baseline = 0,
        .current_node = "axp_battery",
        .charge_method = BOARD_CHARGE_METHOD_NONE, /* CAP_CHARGE_CONTROL unverified, above */
        .input_current = &input_current,
    },
    /* LED class names are single letters: R G B W. */
    .leds = { [BOARD_LED_RED] = "R", [BOARD_LED_BLUE] = "B" },
    .caps = CAPS,
    .caps_impl_missing = IMPL_MISSING,
    .charger = &charger_sysfs_ops,
    /* Version slots in /data/mellowtone, boot state in /data/mellowtone-boot,
     * r3pro.upt / update.upt through the jz reset signature. */
    .launcher = &launcher_hiby_x1000_ops,
    /* HiBy's BlueZ 4.101 (ours, patches/bluez-4.101): sys_server powers the
     * radio and runs pairing/connections, bt_codec sets the codec. The stack
     * and the audio transport (HiBy's ALSA plugin) are detected at run time. */
    .bt_platform = &bt_platform_hiby_ops,
    .system = {
        .mounts_sd = true,  /* S91_early_mount.sh, exFAT at /mnt/sd_0 */
        .hiby_sys_server = true,
        .reboot_via_init = true,  /* busybox reboot runs rcK and unmounts the card */
        .posix_tz = true,         /* only a fixed CST-8 /etc/localtime */
        .keep_stock_names = true, /* bt_init and paired cars read /usr/resource/bt_name */
        .exec_allow = exec_allow,
    },
    /* Legacy android_usb gadget (kernel 3.10); the card is lent to a computer
     * through sys_server's MOUNT:UMOUNT / SETLUN / MOUNT. */
    .usb_gadget = &usb_gadget_android_usb_ops,
    .storage_export = &storage_export_sys_server_ops,
    .usb = {
        .uac_abi = BOARD_UAC_SA_ABI_PREPARE_IOCTL,
        .dsd_native_enable = "class/android_usb/f_uac_sa/dsd_native_enable",
        .android_usb = &android_usb,
        .storage_lun_file = "/sys/class/android_usb/android0/f_mass_storage/lun/file",
        .storage_disk = "/dev/mmcblk0",
        .storage_partition = "/dev/mmcblk0p1",
    },
    /* sys_server powers the radio (/sbin/wifi_on.sh) and runs udhcpc on
     * connect; the player talks to wpa_supplicant over its control socket. */
    .wifi = {
        .backend = BOARD_WIFI_HIBY_SYS_SERVER, .iface = "wlan0", .ctrl_dir = "/var/run/wpa_supplicant",
        .conf = "/data/wpa_supplicant.conf",
    },
    /* The modern UI: clock home, grouped lists, larger rows. */
    .ui = {
        .home_label_large = true,
        .no_marquee = true, /* every scrolling row redraws continuously on this single core */
        .stock_font = true, /* the stock default.otf (Noto Sans SC) */
        .stable_topbar_volume = true,
        .lyrics_small_default = true, /* small lyrics fit the 360 px panel */
        .no_cover_loading_option = true, /* removed on request */
    },
};
