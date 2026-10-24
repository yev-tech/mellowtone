#ifndef BOARD_H
#define BOARD_H

/* Board descriptor: what a board is, as run-time data. Each
 * board defines `const board_desc_t BOARD` in boards/<name>/board.c (added to
 * the build by its board.cmake); the host simulator uses the board it
 * models (preset "host" = r1).
 *
 * Capabilities are hardware: `board_has(CAP_X)`. Software
 * features are compile-time choices, see mt_features.h.
 *
 * Board data is read from the descriptor at run time (BOARD.paths.xxx,
 * BOARD.ui.xxx, ...); behaviour that is not hardware lives in the `system`,
 * `ui` and quirk fields, never in caps. board_config.h keeps only true
 * compile-time constants (screen size and the layout derived from it, the
 * install/SD path strings); board.c static-asserts the ones that exist in
 * both and tests/board checks them.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Hardware capability catalogue. A board
 * lists a capability when its hardware has the function and its kernel can
 * drive it; every listed one must be fully supported. */
typedef enum {
    CAP_AUDIO_DAC          = 1u << 0,  /* internal DAC (PCM, native DSD where the DAC has it) */
    CAP_GAIN_MODES         = 1u << 1,  /* selectable DAC/amp gain levels */
    CAP_BT_AUDIO_OUT       = 1u << 2,  /* A2DP source + AVRCP target */
    CAP_BT_RECEIVER        = 1u << 3,  /* A2DP sink */
    CAP_WIFI               = 1u << 4,  /* 2.4 GHz Wi-Fi */
    CAP_WIFI_5GHZ          = 1u << 5,  /* 5 GHz band */
    CAP_USB_STORAGE        = 1u << 6,  /* USB mass storage (SD card to a computer) */
    CAP_USB_DAC            = 1u << 7,  /* USB audio device (computer -> device DAC) */
    CAP_USB_DAC_DSD        = 1u << 8,  /* native DSD over USB */
    CAP_USB_HOST_AUDIO     = 1u << 9,  /* external USB DAC on the OTG port */
    CAP_CHARGE_CONTROL     = 1u << 10, /* charging can be switched or limited */
    CAP_INPUT_CURRENT      = 1u << 11, /* USB input-current limits */
    CAP_BATTERY_CURRENT    = 1u << 12, /* battery current reading */
    CAP_LEDS               = 1u << 13, /* status LEDs */
    CAP_INLINE_REMOTE      = 1u << 14, /* headphone in-line remote buttons */
    CAP_TOUCH              = 1u << 15, /* touch screen */
    CAP_KEYS               = 1u << 16, /* hardware keys (incl. the recovery-menu key) */
} board_cap_t;

#define BOARD_CAP_COUNT 17
#define BOARD_CAP_ALL ((1u << BOARD_CAP_COUNT) - 1u)

/* led_control.c's indices. */
enum { BOARD_LED_RED = 0, BOARD_LED_BLUE = 1, BOARD_LED_COUNT = 2 };

/* power.quirks */
enum {
    /* The battery node's "status" is stuck at "Discharging" while charging
     * (R3 Pro II); read "status" from power.charger_node instead. Capacity
     * still comes from the battery node. */
    BOARD_POWER_STATUS_FROM_CHARGER = 1u << 0,
    /* No ordinary idle suspend (R3 Pro). Developer Options "Allow suspend",
     * on by default, lifts it (power_suspend_set_allowed()) for the stock
     * player's 60 s standby suspend in gui.c; with it off, idle always
     * powers off and screen-off only darkens the panel. */
    BOARD_POWER_NO_SUSPEND = 1u << 1,
    /* Suspend-to-RAM follows the stock player's Bluetooth sequence
     * (R3 Pro): /usr/bin/bt_suspend stops the whole stack (bluetoothd,
     * bt-media, dbus-daemon, chip power), so /usr/bin/bt_resume runs after
     * every resume, whether or not Bluetooth was on. */
    BOARD_POWER_BT_RESUME_SCRIPT = 1u << 2,
};

/* power.charge_chips: the chips the AXP2101 charger back end
 * (charger_axp2101_i2c.c) drives over /dev/i2c-0 for CAP_CHARGE_CONTROL
 * (charge limit, safe charging). */
enum {
    BOARD_CHARGER_AXP2101 = 1u << 0, /* PMIC charger (R1, R3 Pro II, R3 II 2025) */
    BOARD_CHARGER_MP2731 = 1u << 1,  /* dedicated charger IC next to the AXP2101 (R3 Pro II) */
};

/* power.charge_method: how CAP_CHARGE_CONTROL holds the charge limit
 * (charge_control.c). */
typedef enum {
    BOARD_CHARGE_METHOD_NONE = 0,   /* no charge control (no CAP_CHARGE_CONTROL) */
    BOARD_CHARGE_METHOD_VOLTAGE_CAP, /* lower the charge-termination voltage (AXP2101: 4.2 V) */
    BOARD_CHARGE_METHOD_STOP_RESUME, /* charger only switches on/off: stop at the limit,
                                        resume a few percent below (hysteresis) */
} board_charge_method_t;

/* power.input_current: the USB input-current policy (CAP_INPUT_CURRENT),
 * applied by the sysfs charger back end. Values are written as they are
 * (the driver's units); -1 leaves an attribute alone. */
typedef struct {
    int max_power;   /* gadget MaxPower */
    int limit_max;   /* usb_node/charge_control_limit_max */
    int current_max; /* usb_node/constant_charge_current_max */
} board_input_current_row_t;

typedef struct {
    const char * gadget_state;     /* sysfs path (relative to /sys) of the gadget state:
                                      CONFIGURED/CONNECTED = a computer is on the cable */
    const char * gadget_max_power; /* sysfs path of the gadget's MaxPower, or NULL */
    board_input_current_row_t host;    /* a computer enumerated us */
    board_input_current_row_t charger; /* usb_node online, no enumeration (wall charger) */
    /* Unplugged: nothing is written. */
    /* A computer enumerated us in USB DAC mode and "Don't charge in USB DAC
     * mode" is on (charge_control_set_inhibit()): the stock R3 Pro's
     * dac_charge_disable row. Used only when has_dac_no_charge. */
    bool has_dac_no_charge;
    board_input_current_row_t dac_no_charge;
} board_input_current_policy_t;

/* usb.uac_abi: how the /dev/uac_sa driver (HiBy's f_uac_sa, USB DAC mode)
 * says the host stream is armed. Both answer ioctl 1 = GET_INFO int[3]
 * {format, rate, bits}. */
typedef enum {
    BOARD_UAC_SA_ABI_READ_EPERM = 0, /* read() fails with EPERM until the host arms the stream
                                        (X1600 kernels, seen on an R1) */
    BOARD_UAC_SA_ABI_PREPARE_IOCTL,  /* kernel 3.10 (R3 Pro): ioctl 0 "prepare" > 0 once armed,
                                        0 not yet, < 0 error; GET_INFO format 2 = native DSD */
} board_uac_sa_abi_t;

/* One android_usb gadget set-up (legacy android_usb, kernel 3.x): the values
 * written to /sys/class/android_usb/android0 between "enable 0" and "enable 1"
 * (the stock player's values on the R3 Pro). Strings are written as they are, no newline. */
typedef struct {
    const char * id_vendor;  /* idVendor, hex without 0x */
    const char * id_product; /* idProduct */
    const char * device_class, * device_subclass, * device_protocol; /* bDeviceClass/SubClass/Protocol */
    const char * functions;  /* functions, e.g. "uac_sa,hid" */
} board_android_usb_config_t;

typedef struct {
    const char * gadget_dir; /* relative to /sys: "class/android_usb/android0" */
    board_android_usb_config_t dac;     /* USB DAC (CAP_USB_DAC) */
    board_android_usb_config_t storage; /* the card lent to a computer (CAP_USB_STORAGE) */
    board_android_usb_config_t idle;    /* everything else: what K90adb sets; adbd runs only
                                           while ADB is on (BOARD.paths.adb_flag) */
    const char * manufacturer, * product, * serial; /* iManufacturer, iProduct, iSerial */
    const char * uac_string_assoc; /* f_uac_sa string_assoc (relative to /sys), or NULL */
    const char * uac_string_assoc_value;
    const char * inquiry_vendor;   /* SCSI INQUIRY vendor (<= 8 chars) and product (<= 16) */
    const char * inquiry_product;
} board_android_usb_t;

/* wifi.backend: who powers the radio and runs DHCP. Network
 * changes go over the wpa_supplicant control socket on every board. */
typedef enum {
    BOARD_WIFI_WPA_SCRIPTS = 0,  /* wifi.power_on/power_off scripts; DHCP by the player (X1600) */
    BOARD_WIFI_HIBY_SYS_SERVER,  /* sys_server WIFI:ON/OFF; DHCP by sys_server's monitor (R3 Pro) */
} board_wifi_backend_t;

typedef struct {
    const char * name; /* evdev name, matched exactly (/proc/bus/input/devices) */
    bool knob;         /* arrow keys are volume-knob detents (R3 II 2025) */
} board_input_device_t;

/* Back-end ops: NULL where a board has no back end for that subsystem.
 * Incomplete types on purpose. */
struct board_bt_platform_ops;
struct board_usb_gadget_ops;
struct board_storage_export_ops;
struct board_charger_ops;
struct board_launcher_ops;

typedef struct {
    const char * name;  /* board id: "r1", "r3pro", "r3proii", "r3ii_2025" (= boards/<name>) */
    const char * model; /* human-readable model, e.g. "HiBy R3 Pro" */

    struct {
        int w, h;        /* panel size in pixels (BOARD_SCREEN_WIDTH/HEIGHT) */
        int rotation;    /* degrees the panel is mounted rotated: 0 or 180 */
        int scale;       /* UI font scale in percent, 100 = none */
        bool no_fb_blank; /* never blank the framebuffer: screen-off is backlight-only (R3 Pro:
                             jzfb smart-LCD blank can misalign the panel until reboot) */
        bool fb_sleep;    /* screen-off also stops the smart-LCD controller (jzfb sleep ioctl, as the
                             stock UI does; R3 Pro): no vsync while dark, rendering stops first */
    } screen;

    struct {
        const char * install_root; /* flash: binary, fonts, theme (BOARD_DATA_ROOT) */
        const char * image_root;   /* rootfs install root of a firmware-image install, or NULL:
                                      when <image_root>/factory exists (the factory player)
                                      the read-only assets named below under
                                      install_root are read from here (board_asset_path()) */
        const char * sd_root;      /* SD card mount point (BOARD_SD_ROOT) */
        const char * theme_root;   /* theme asset tree, with a trailing '/' */
        const char * scan_spool_dir; /* library-scan spool files go here, or NULL: next to
                                        the data on the card (R3 Pro: its exFAT driver
                                        fails writes to unlinked files, so /tmp) */
        const char * inline_remote_sw; /* sysfs switch enabling the headphone in-line remote
                                          (CAP_INLINE_REMOTE), written "on"/"off" */
        const char * stock_gain_ini;   /* stock player's settings file holding its gain, kept
                                          in step with the gain modes (R3 Pro), or NULL */
        const char * launcher_debug_flag; /* old flash debug flag the shell launcher and bt_init
                                             read (R3 Pro): removed when logging is switched off.
                                             mellowtone-launcher reads the card's flag. Or NULL */
        const char * adb_flag;     /* flash flag "ADB on": the launchers start ADB at boot while it
                                      exists, the player's ADB switch and the recovery menu set
                                      it (R3 Pro), or NULL */
        const char * font_cjk;    /* stock multilingual (CJK/Cyrillic) face */
        const char * font_ui;     /* primary UI face with ui.stock_font, or NULL */
        const char * font_hebrew; /* Hebrew fallback face, or NULL */
    } paths;

    struct {
        const board_input_device_t * buttons; /* key devices, {NULL}-terminated, in open order */
        const char * const * touch;           /* touch panels, NULL-terminated, first found wins */
        const void * keymap;                  /* key remapping table: none today (NULL) */
        int menu_key;  /* held at power-on: mellowtone-launcher opens its recovery menu;
                          0 = none */
        const char * power_key_device; /* evdev reporting KEY_POWER */
        const char * menu_key_device;  /* evdev reporting menu_key, or NULL */
        int key_poll_ms; /* UI drain interval for queued key presses, or 0: the default
                            (25 ms power, 16 ms media); R3 Pro 50 (single core) */
    } input;

    struct {
        unsigned int card;   /* internal DAC */
        unsigned int device;
        const char * mixer_volume_left;  /* hardware volume controls */
        const char * mixer_volume_right;
        const char * mixer_dop_switch;   /* DoP enable switch (absent on some cards) */
        const char * mixer_output_port;  /* 3.5 mm / 4.4 mm route (absent on some cards) */
        const char * volume_curve;       /* stock volume table file the gain modes read, or
                                            NULL: the built-in calibrated taper */
        /* The table to read when volume_curve does not exist (R3 Pro image install:
         * the stock player that made it on first boot is gone), written to buf;
         * NULL: none. Or NULL. See board_volume_curve(). */
        const char * (*volume_curve_select)(char * buf, size_t size);
        int aplay_nice;      /* nice value for the aplay feeding Bluetooth/USB output, or 0
                                (R3 Pro -10: HiBy's BT plugin buffers only ~140 ms) */
    } alsa;

    struct {
        const char * battery_node; /* /sys/class/power_supply name preferred for the battery
                                      (otherwise the first node of type "Battery") */
        const char * charger_node; /* dedicated charger IC's node, or NULL */
        const char * usb_node;     /* USB input supply (input-current policy), or NULL */
        unsigned int quirks;       /* BOARD_POWER_* */
        unsigned int charge_chips; /* BOARD_CHARGER_*, for CAP_CHARGE_CONTROL */
        uint8_t axp_voltage_baseline; /* stock AXP2101 charge-voltage target, AXP_REG_VOLTAGE
                                         low bits (4 = 4.35 V, 5 = 4.40 V), from the stock
                                         axp2101.sh charge_voltage_limit */
        const char * current_node; /* node whose current_now is the battery current
                                      (CAP_BATTERY_CURRENT), or NULL: the battery node */
        board_charge_method_t charge_method; /* CAP_CHARGE_CONTROL: how the limit is held */
        const board_input_current_policy_t * input_current; /* CAP_INPUT_CURRENT, or NULL */
    } power;

    const char * leds[BOARD_LED_COUNT]; /* /sys/class/leds names, by BOARD_LED_* */

    uint32_t caps; /* board_cap_t bits */
    /* Listed caps whose implementation is still missing on this board (a
     * bug each, as every listed cap must be fully supported): see board_impl_missing(). */
    uint32_t caps_impl_missing;

    /* The base system the player runs on: not hardware, so not caps. */
    struct {
        bool mounts_sd;       /* the system mounts the card: the player never mounts,
                                 checks or formats it (R3 Pro) */
        bool hiby_sys_server; /* HiBy's sys_server owns radio power and talks to the player
                                 over /var/run/sys_client (R3 Pro) */
        bool reboot_via_init; /* reboot through /sbin/reboot so init unmounts the card
                                 (R3 Pro; X1600: the launcher unmounts the card itself) */
        bool posix_tz;        /* no zoneinfo tree: the time zone is a POSIX TZ rule */
        bool keep_stock_names; /* hostname only at run time: no bind-mounted override of
                                  the stock hostname / Bluetooth name files */
        /* Commands the player may run, NULL-terminated (anything else is refused and
         * logged), or NULL: no limit. With a list, subprocess_kill_all_matching()
         * only kills the player's own aplay. */
        const char * const * exec_allow;
    } system;

    /* UI choices per board; none is hardware. */
    struct {
        bool home_label_large; /* home tile labels one font step larger (row text) */
        bool no_marquee;      /* ellipsize long row text instead of scrolling marquees */
        bool stock_font;      /* primary UI face from the stock firmware's fonts */
        bool stable_topbar_volume; /* volume digits in fixed slots in the status bar */
        bool lyrics_small_default; /* small lyrics, applied once over the saved size */
        bool no_cover_loading_option; /* no "Load covers during playback" switch; covers
                                         stay off during playback */
    } ui;

    /* CAP_WIFI boards: the wpa_supplicant set-up. */
    struct {
        board_wifi_backend_t backend;
        const char * iface;      /* NULL: wlan0 */
        const char * ctrl_dir;   /* supplicant control sockets; NULL: /var/run/wpa_supplicant */
        const char * conf;       /* supplicant config file, or NULL: not read by the player */
        const char * power_on;   /* BOARD_WIFI_WPA_SCRIPTS: scripts run to switch the radio */
        const char * power_off;
    } wifi;

    /* USB device modes: back ends in .usb_gadget/.storage_export. */
    struct {
        board_uac_sa_abi_t uac_abi;
        const char * dsd_native_enable; /* sysfs (relative to /sys) switching native DSD over
                                           USB on/off (CAP_USB_DAC_DSD), or NULL */
        const board_android_usb_t * android_usb; /* usb_gadget_android_usb_ops' set-ups */
        /* storage_export_sys_server_ops: the mass-storage LUN backing file (absolute
         * path: sys_server writes it), the card's whole disk exported through it and
         * the partition mounted back (the disk when the partition does not exist). */
        const char * storage_lun_file;
        const char * storage_disk;
        const char * storage_partition;
    } usb;

    const struct board_bt_platform_ops * bt_platform; /* CAP_BT_AUDIO_OUT boards */
    const struct board_usb_gadget_ops * usb_gadget;         /* usb_gadget.h; NULL: no gadget */
    const struct board_storage_export_ops * storage_export; /* usb_gadget.h; NULL: no storage */
    const struct board_charger_ops * charger; /* charger.h; NULL: no charger control */
    const struct board_launcher_ops * launcher; /* board_launcher.h: mellowtone-launcher's part */
} board_desc_t;

extern const board_desc_t BOARD;

/* Bluetooth platforms (network/bluetooth/bt_backend.h) a
 * descriptor can point .bt_platform at: who powers the radio and owns the
 * codec policy. NULL: none (the stack's own Powered property). */
extern const struct board_bt_platform_ops bt_platform_hiby_ops;    /* sys_server + bt_codec (R3 Pro) */
extern const struct board_bt_platform_ops bt_platform_scripts_ops; /* bt_resume/bt_enable/bt_disable (X1600) */

/* USB gadget back ends (usb_gadget.h) for .usb_gadget. */
extern const struct board_usb_gadget_ops usb_gadget_configfs_ops;    /* configfs + stock scripts (X1600) */
extern const struct board_usb_gadget_ops usb_gadget_android_usb_ops; /* legacy android_usb (R3 Pro) */
/* Storage hand-over back ends for .storage_export. */
extern const struct board_storage_export_ops storage_export_shared_ops;     /* card stays mounted (X1600) */
extern const struct board_storage_export_ops storage_export_sys_server_ops; /* sys_server MOUNT:* (R3 Pro) */

/* Charger back ends (charger.h) a descriptor can point .charger at. */
extern const struct board_charger_ops charger_axp2101_i2c_ops; /* AXP2101 (+ MP2731) over i2c */
extern const struct board_charger_ops charger_sysfs_ops;       /* generic power_supply sysfs */

bool board_has(board_cap_t cap);

/* Every listed capability must be fully implemented. Until a
 * capability's implementation lands for this board, the UI for it stays
 * hidden behind this one temporary guard, so that every gap is listed in
 * one place (boards/<name>/board.c) with what is still missing. Always false for a
 * capability the board does not list. */
bool board_impl_missing(board_cap_t cap);

/* Install layout. A bundle install keeps everything in
 * paths.install_root (/data/mellowtone on the R3 Pro). A firmware-image install
 * carries the read-only part in the rootfs, paths.image_root
 * (/usr/mellowtone), with the factory player in <image_root>/factory; the
 * same player binary runs in both and tells them apart at run time. */
bool board_image_install(void);
/* image_root on an image install, else install_root (the player's working directory). */
const char * board_install_root(void);
/* A descriptor path under install_root (theme_root, font_ui, ...) as it
 * is on this install: the same path under image_root on an image install,
 * unchanged otherwise (and for any other path). The result stays valid. */
const char * board_asset_path(const char * path);
/* The gain modes' volume table: alsa.volume_curve, or when that file does
 * not exist alsa.volume_curve_select's pick; NULL: none. */
const char * board_volume_curve(void);

/* Capability tokens for plugins ("hw.bt_audio_out", ...). */
const char * board_cap_name(board_cap_t cap);          /* NULL for anything but one known bit */
bool board_cap_from_name(const char * name, board_cap_t * out);
/* The i-th catalogue entry, i < BOARD_CAP_COUNT. */
board_cap_t board_cap_at(size_t i);

/* Checked at compile time in each board.c: an enabled
 * feature's required capabilities must be in that board's caps. */
#define BOARD_CHECK_FEATURE_REQUIREMENTS(caps)                                                     \
    _Static_assert(!MT_HAS_FEATURE(AIRPLAY) || ((caps) & CAP_WIFI),                                \
                   "MT_FEATURE_AIRPLAY needs CAP_WIFI");                                           \
    _Static_assert(!MT_HAS_FEATURE(DLNA_RENDERER) || ((caps) & CAP_WIFI),                          \
                   "MT_FEATURE_DLNA_RENDERER needs CAP_WIFI");                                     \
    _Static_assert(!MT_HAS_FEATURE(WIFI_IMPORT) || ((caps) & CAP_WIFI),                            \
                   "MT_FEATURE_WIFI_IMPORT needs CAP_WIFI");                                       \
    _Static_assert(!MT_HAS_FEATURE(REMOTE) || ((caps) & (CAP_WIFI | CAP_BT_AUDIO_OUT)),            \
                   "MT_FEATURE_REMOTE needs CAP_WIFI or CAP_BT_AUDIO_OUT")

#endif /* BOARD_H */
