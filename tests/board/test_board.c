/* Board descriptor checks, built once per board with that
 * board's BOARD_* define and feature presets, linked with its
 * boards/<name>/board.c: run as board_<name>.
 *
 *   - name, model non-empty; name is the expected board id
 *   - no unknown capability bits; every capability has a unique "hw." token
 *     that maps back to it
 *   - values that also exist in board_config.h agree (screen, paths)
 *   - the data the player reads from the descriptor is there (LED names, input devices,
 *     power nodes)
 *   - feature tokens are unique "feature." names; an enabled feature
 *     has its required capabilities -- also static-asserted in board.c */
#include "board.h"
#include "board_launcher.h"
#include "usb_gadget.h"
#include "charger.h"
#include "board_config.h"
#include "mt_features.h"

#include <stdio.h>
#include <string.h>
#include <linux/input.h>

#ifndef EXPECTED_BOARD_NAME
#error "EXPECTED_BOARD_NAME missing (tests/CMakeLists.txt)"
#endif

static int failures = 0;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);    \
            fprintf(stderr, __VA_ARGS__);                                      \
            fputc('\n', stderr);                                               \
            failures++;                                                        \
        }                                                                      \
    } while (0)

static bool str_eq(const char * a, const char * b) {
    return a && b && strcmp(a, b) == 0;
}

static void check_identity(void) {
    CHECK(BOARD.name && BOARD.name[0], "empty name");
    CHECK(str_eq(BOARD.name, EXPECTED_BOARD_NAME), "name '%s'", BOARD.name ? BOARD.name : "(null)");
    CHECK(BOARD.model && BOARD.model[0], "empty model");
}

static void check_caps(void) {
    CHECK((BOARD.caps & ~BOARD_CAP_ALL) == 0, "unknown capability bits 0x%x",
          (unsigned) (BOARD.caps & ~BOARD_CAP_ALL));
    for (size_t i = 0; i < BOARD_CAP_COUNT; i++) {
        board_cap_t cap = board_cap_at(i);
        const char * name = board_cap_name(cap);
        CHECK(name && strncmp(name, "hw.", 3) == 0, "capability bit %zu has no hw.* token", i);
        if (!name) continue;
        board_cap_t back = 0;
        CHECK(board_cap_from_name(name, &back) && back == cap, "token %s does not map back", name);
        for (size_t j = 0; j < i; j++)
            CHECK(!str_eq(name, board_cap_name(board_cap_at(j))), "token %s twice", name);
        CHECK(board_has(cap) == ((BOARD.caps & cap) != 0), "board_has(%s)", name);
    }
    CHECK(board_cap_name((board_cap_t) 0) == NULL, "name for 0");
    CHECK(board_cap_name((board_cap_t) (CAP_WIFI | CAP_LEDS)) == NULL, "name for two bits");
    CHECK(!board_cap_from_name("hw.nonexistent", NULL), "unknown token accepted");
    CHECK(!board_has((board_cap_t) 0), "board_has(0)");
    /* Every board has these. */
    const board_cap_t everywhere[] = { CAP_AUDIO_DAC, CAP_BT_AUDIO_OUT, CAP_WIFI, CAP_USB_STORAGE,
                                       CAP_USB_DAC, CAP_TOUCH, CAP_KEYS };
    for (size_t i = 0; i < sizeof(everywhere) / sizeof(everywhere[0]); i++)
        CHECK(board_has(everywhere[i]), "missing %s", board_cap_name(everywhere[i]));
}

static void check_board_config(void) {
    CHECK(BOARD.screen.w == BOARD_SCREEN_WIDTH && BOARD.screen.h == BOARD_SCREEN_HEIGHT,
          "screen %dx%d, board_config.h %dx%d", BOARD.screen.w, BOARD.screen.h,
          BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT);
    CHECK(BOARD.screen.rotation == 0 || BOARD.screen.rotation == 180, "rotation %d", BOARD.screen.rotation);
    CHECK(BOARD.screen.scale > 0 && BOARD.screen.scale <= 100, "scale %d", BOARD.screen.scale);
    CHECK(str_eq(BOARD.paths.sd_root, BOARD_SD_ROOT), "sd_root %s", BOARD.paths.sd_root);
    CHECK(str_eq(BOARD.paths.install_root, BOARD_DATA_ROOT), "install_root %s", BOARD.paths.install_root);
    CHECK(BOARD.paths.theme_root && BOARD.paths.theme_root[0] == '/' &&
              BOARD.paths.theme_root[strlen(BOARD.paths.theme_root) - 1] == '/',
          "theme_root %s", BOARD.paths.theme_root ? BOARD.paths.theme_root : "(null)");
}

static void check_consumer_data(void) {
    for (int i = 0; i < BOARD_LED_COUNT; i++)
        CHECK(!board_has(CAP_LEDS) || (BOARD.leds[i] && BOARD.leds[i][0]), "LED %d has no name", i);
    CHECK(BOARD.input.buttons && BOARD.input.buttons[0].name, "no key devices");
    size_t n = 0;
    while (BOARD.input.buttons && BOARD.input.buttons[n].name) n++;
    CHECK(n <= 12, "%zu key devices, hw_buttons.c polls 12 fds", n); /* HW_BUTTONS_MAX_FDS */
    CHECK(BOARD.input.touch && BOARD.input.touch[0], "no touch devices");
    CHECK(BOARD.power.battery_node && BOARD.power.battery_node[0], "no battery node");
    CHECK(!(BOARD.power.quirks & BOARD_POWER_STATUS_FROM_CHARGER) || BOARD.power.charger_node,
          "status quirk without charger node");
    CHECK(BOARD.alsa.mixer_volume_left && BOARD.alsa.mixer_volume_right, "no volume controls");
    CHECK(!board_has(CAP_GAIN_MODES) || BOARD.alsa.volume_curve, "gain modes without a volume table");
    CHECK(!board_has(CAP_INLINE_REMOTE) || BOARD.paths.inline_remote_sw, "in-line remote without its sysfs switch");
    /* The MP2731's status node is the one the status quirk reads. */
    CHECK(!(BOARD.power.charge_chips & BOARD_CHARGER_MP2731) ||
              ((BOARD.power.quirks & BOARD_POWER_STATUS_FROM_CHARGER) && str_eq(BOARD.power.charger_node, "mp2731-charger")),
          "MP2731 without its charger node");
    /* Battery and charger. */
    CHECK(!board_has(CAP_CHARGE_CONTROL) || (BOARD.charger && BOARD.power.charge_method != BOARD_CHARGE_METHOD_NONE),
          "charge control without a charger back end and method");
    CHECK(board_has(CAP_CHARGE_CONTROL) || BOARD.power.charge_method == BOARD_CHARGE_METHOD_NONE,
          "charge method without CAP_CHARGE_CONTROL");
    CHECK(BOARD.power.charge_method != BOARD_CHARGE_METHOD_VOLTAGE_CAP ||
              (BOARD.charger == &charger_axp2101_i2c_ops && (BOARD.power.charge_chips & BOARD_CHARGER_AXP2101)),
          "voltage cap without the AXP2101 back end");
    CHECK(!(BOARD.power.charge_chips & BOARD_CHARGER_AXP2101) || BOARD.charger == &charger_axp2101_i2c_ops,
          "AXP2101 board not on the AXP2101 back end");
    CHECK(!board_has(CAP_INPUT_CURRENT) ||
              (BOARD.power.input_current && BOARD.power.usb_node && BOARD.charger == &charger_sysfs_ops),
          "input current without a policy, usb node and the sysfs back end");
    CHECK(!board_has(CAP_BATTERY_CURRENT) || BOARD.power.current_node || BOARD.power.battery_node,
          "battery current without a node");
    if (str_eq(BOARD.name, "r3pro")) {
        /* The stock player's input-current values,
         * and charge control stays off until the device test. */
        const board_input_current_policy_t * p = BOARD.power.input_current;
        CHECK(p && str_eq(p->gadget_state, "class/android_usb/android0/state") &&
                  str_eq(p->gadget_max_power, "class/android_usb/android0/MaxPower"),
              "R3 Pro gadget paths");
        CHECK(p && p->host.max_power == 500 && p->host.limit_max == 500 && p->host.current_max == 500,
              "R3 Pro host row");
        CHECK(p && p->charger.max_power == 500 && p->charger.limit_max == 0 && p->charger.current_max == 1200,
              "R3 Pro charger row");
        CHECK(!board_has(CAP_CHARGE_CONTROL), "R3 Pro charge control is unverified");
        CHECK(str_eq(BOARD.power.current_node, "axp_battery"), "R3 Pro current node");
    }
    CHECK(!(BOARD.power.charge_chips & BOARD_CHARGER_AXP2101) ||
              (BOARD.power.axp_voltage_baseline >= 1 && BOARD.power.axp_voltage_baseline <= 5),
          "AXP2101 voltage baseline %u", BOARD.power.axp_voltage_baseline);
    CHECK(BOARD.paths.font_cjk && BOARD.paths.font_cjk[0] == '/', "no multilingual font");
    CHECK(!BOARD.ui.stock_font || BOARD.paths.font_ui, "stock_font without a UI face");
    CHECK(BOARD.input.power_key_device && BOARD.input.power_key_device[0], "no power key device");
    /* mellowtone-launcher: every board has its part, a recovery
     * menu key on a device it lists, and an image name the recovery uses. */
    {
        const struct board_launcher_ops * l = BOARD.launcher;
        CHECK(l && l->name && l->slots_root && l->slots_root[0] == '/' && l->boot_dir && l->boot_dir[0] == '/',
              "no launcher ops or slot/boot paths");
        CHECK(l && l->firmware_images && l->firmware_images[0] && strstr(l->firmware_images[0], ".upt"),
              "launcher without firmware image names");
        CHECK(l && l->enter_recovery && l->enable_adb, "launcher without recovery/ADB hooks");
        CHECK(!board_has(CAP_KEYS) || BOARD.input.menu_key > 0, "keys but no recovery-menu key");
        bool listed = false;
        for (const board_input_device_t * d = BOARD.input.buttons; d && d->name; d++)
            if (BOARD.input.menu_key_device && strcmp(d->name, BOARD.input.menu_key_device) == 0) listed = true;
        CHECK(!BOARD.input.menu_key || listed, "menu key device not among the key devices");
        CHECK(!l || !l->firmware_product || strcmp(l->firmware_product, BOARD.name) == 0,
              "firmware product is not this board");
        /* Image install: the factory player sits in <image_root>/factory,
         * where board_image_install() looks. (The X1600 boards' factory copy
         * predates image_root and has no separate install root yet.) */
        if (BOARD.paths.image_root) {
            char want[256];
            snprintf(want, sizeof(want), "%s/factory", BOARD.paths.image_root ? BOARD.paths.image_root : "");
            CHECK(BOARD.paths.image_root && BOARD.paths.image_root[0] == '/' && l && l->factory_dir &&
                      strcmp(l->factory_dir, want) == 0,
                  "image_root %s and factory_dir %s disagree", BOARD.paths.image_root ? BOARD.paths.image_root : "(null)",
                  l && l->factory_dir ? l->factory_dir : "(null)");
            CHECK(!board_image_install() || strcmp(board_install_root(), BOARD.paths.image_root) == 0,
                  "image install root");
            CHECK(board_image_install() || board_asset_path(BOARD.paths.theme_root) == BOARD.paths.theme_root,
                  "bundle install maps theme_root");
        }
        if (strcmp(BOARD.name, "r3pro") == 0) {
            CHECK(BOARD.input.menu_key == KEY_VOLUMEDOWN, "R3 Pro menu key is Vol-");
            CHECK(l == &launcher_hiby_x1000_ops && l->fb_slcd, "R3 Pro launcher ops");
        } else {
            CHECK(l == &launcher_x1600_ops && l->release_sd && l->early, "X1600 launcher ops");
        }
    }
    /* USB modes. */
    CHECK(!board_has(CAP_USB_STORAGE) || BOARD.storage_export, "USB storage without a storage back end");
    CHECK(!(board_has(CAP_USB_STORAGE) || board_has(CAP_USB_DAC)) || BOARD.usb_gadget, "USB modes without a gadget back end");
    CHECK(!board_has(CAP_USB_DAC_DSD) || board_has(CAP_USB_DAC), "USB DSD without USB DAC");
    CHECK(BOARD.usb_gadget != &usb_gadget_android_usb_ops ||
              (BOARD.paths.adb_flag && BOARD.usb.android_usb && BOARD.usb.android_usb->gadget_dir &&
               BOARD.usb.android_usb->dac.functions && BOARD.usb.android_usb->storage.functions &&
               BOARD.usb.android_usb->idle.functions),
          "android_usb without an ADB flag or its set-ups");
    CHECK(BOARD.storage_export != &storage_export_sys_server_ops ||
              (BOARD.system.hiby_sys_server && BOARD.usb_gadget == &usb_gadget_android_usb_ops &&
               BOARD.usb.storage_lun_file && BOARD.usb.storage_lun_file[0] == '/' && BOARD.usb.storage_disk &&
               BOARD.usb.storage_partition),
          "sys_server storage without sys_server, android_usb or the LUN/card paths");
    CHECK(!board_has(CAP_USB_DAC_DSD) || BOARD.usb.uac_abi == BOARD_UAC_SA_ABI_PREPARE_IOCTL,
          "native DSD over USB needs the PREPARE_IOCTL /dev/uac_sa ABI");
    if (strcmp(BOARD.name, "r3pro") == 0) {
        const board_android_usb_t * a = BOARD.usb.android_usb;
        /* The stock player's set-ups. */
        CHECK(a && str_eq(a->dac.id_vendor, "32BB") && str_eq(a->dac.id_product, "0004") &&
                  str_eq(a->dac.functions, "uac_sa,hid") && str_eq(a->dac.device_class, "239"),
              "R3 Pro DAC set-up");
        CHECK(a && str_eq(a->storage.id_product, "0101") && str_eq(a->storage.functions, "mass_storage"),
              "R3 Pro storage set-up");
        const board_input_current_policy_t * p = BOARD.power.input_current;
        CHECK(p && p->has_dac_no_charge && p->dac_no_charge.max_power == 10 && p->dac_no_charge.limit_max == 100 &&
                  p->dac_no_charge.current_max == 0,
              "R3 Pro dac_charge_disable row");
    }
    /* Wi-Fi power back end. */
    CHECK(!board_has(CAP_WIFI) || BOARD.wifi.backend != BOARD_WIFI_WPA_SCRIPTS ||
              (BOARD.wifi.power_on && BOARD.wifi.power_on[0] == '/' && BOARD.wifi.power_off &&
               BOARD.wifi.power_off[0] == '/'),
          "Wi-Fi scripts back end without its scripts");
    CHECK(BOARD.wifi.backend != BOARD_WIFI_HIBY_SYS_SERVER || BOARD.system.hiby_sys_server,
          "sys_server Wi-Fi back end without sys_server");
    /* Bluetooth platform: every Bluetooth board names one. */
    CHECK(!board_has(CAP_BT_AUDIO_OUT) || BOARD.bt_platform, "Bluetooth without a platform");
    CHECK(BOARD.bt_platform != &bt_platform_hiby_ops || BOARD.system.hiby_sys_server,
          "sys_server Bluetooth platform without sys_server");
    /* The impl-missing guard only lists caps the board has. */
    CHECK((BOARD.caps_impl_missing & ~BOARD.caps) == 0, "impl_missing outside caps 0x%x",
          (unsigned) (BOARD.caps_impl_missing & ~BOARD.caps));
    for (size_t i = 0; i < BOARD_CAP_COUNT; i++) {
        board_cap_t cap = board_cap_at(i);
        CHECK(board_impl_missing(cap) == (board_has(cap) && (BOARD.caps_impl_missing & cap)), "board_impl_missing(%s)",
              board_cap_name(cap));
    }
}

static void check_features(void) {
    for (size_t i = 0; i < MT_FEATURE_COUNT; i++) {
        const char * name = MT_FEATURES[i].name;
        CHECK(name && strncmp(name, "feature.", 8) == 0, "feature %zu token", i);
        for (size_t j = 0; j < i; j++) CHECK(!str_eq(name, MT_FEATURES[j].name), "feature %s twice", name);
        CHECK(mt_feature_enabled(name) == MT_FEATURES[i].enabled, "mt_feature_enabled(%s)", name);
    }
    CHECK(MT_FEATURE_COUNT == 7, "%zu features", MT_FEATURE_COUNT);
    CHECK(!mt_feature_enabled("feature.nonexistent"), "unknown feature enabled");
    CHECK(!MT_HAS_FEATURE(AIRPLAY) || board_has(CAP_WIFI), "airplay without its capability");
    CHECK(!MT_HAS_FEATURE(DLNA_RENDERER) || board_has(CAP_WIFI), "dlna without its capability");
    CHECK(!MT_HAS_FEATURE(WIFI_IMPORT) || board_has(CAP_WIFI), "import without its capability");
    CHECK(!MT_HAS_FEATURE(REMOTE) || board_has(CAP_WIFI) || board_has(CAP_BT_AUDIO_OUT), "remote without its capability");
}

int main(void) {
    check_identity();
    check_caps();
    check_board_config();
    check_consumer_data();
    check_features();
    if (failures) {
        fprintf(stderr, "board %s: %d failure(s)\n", EXPECTED_BOARD_NAME, failures);
        return 1;
    }
    printf("board %s: ok (caps 0x%05x)\n", BOARD.name, (unsigned) BOARD.caps);
    return 0;
}
