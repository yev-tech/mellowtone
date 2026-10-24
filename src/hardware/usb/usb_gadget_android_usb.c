/* Legacy android_usb gadget back end (kernel 3.10, R3 Pro): the stock
 * player's system_if_usb_select_device() sequence
 * with the values from the descriptor (BOARD.usb.android_usb):
 *
 *   enable 0, strings, bDeviceClass/SubClass/Protocol, idVendor, idProduct,
 *   functions, enable 1
 *
 * Every value is written as it is, without a newline, like the stock
 * player. ADB keeps going through the stock K90adb script (adbd, and
 * BOARD.paths.adb_flag for the boot launcher), exactly as before. The
 * mass-storage set-up is only presented while the card is lent
 * (set_storage_function(), from storage_export_sys_server_ops); otherwise
 * the port shows the idle set-up K90adb leaves (functions "adb", with adbd
 * running only while ADB is on). Charge only is that idle set-up: a
 * computer still configures it (state CONFIGURED), which is how it is told
 * from a wall charger, but without adbd it gets nothing. */
#include "usb_gadget.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define SCRIPT_TIMEOUT_MS 15000

static const board_android_usb_t * cfg(void) { return usb_board()->usb.android_usb; }

static bool attr_rel(const char * attr, char * out, size_t out_size) {
    const board_android_usb_t * c = cfg();
    if (!c || !c->gadget_dir) return false;
    int n = snprintf(out, out_size, "%s/%s", c->gadget_dir, attr);
    return n > 0 && (size_t) n < out_size;
}

static bool write_attr(const char * attr, const char * value) {
    char rel[160];
    return value && attr_rel(attr, rel, sizeof(rel)) && usb_sysfs_write(rel, value);
}

static bool read_attr(const char * attr, char * out, size_t out_size) {
    char rel[160];
    return attr_rel(attr, rel, sizeof(rel)) && usb_sysfs_read(rel, out, out_size);
}

/* "functions" reads back as the enabled names, comma-separated, sometimes
 * with a trailing comma or newline. */
static bool functions_are(const char * want) {
    char have[128];
    if (!read_attr("functions", have, sizeof(have))) return false;
    size_t len = strlen(have);
    while (len > 0 && (have[len - 1] == ',' || have[len - 1] == ' ')) have[--len] = '\0';
    return strcmp(have, want) == 0;
}

static bool functions_contain(const char * name) {
    char have[128];
    return read_attr("functions", have, sizeof(have)) && strstr(have, name) != NULL;
}

static void write_strings(bool storage) {
    const board_android_usb_t * c = cfg();
    /* String failures are logged by usb_sysfs_write() and never fatal. */
    if (c->uac_string_assoc && c->uac_string_assoc_value)
        usb_sysfs_write(c->uac_string_assoc, c->uac_string_assoc_value);
    if (c->product) write_attr("iProduct", c->product);
    if (c->manufacturer) write_attr("iManufacturer", c->manufacturer);
    if (c->serial) write_attr("iSerial", c->serial);
    if (storage && c->inquiry_vendor && c->inquiry_product) {
        /* SCSI INQUIRY: vendor (8), product (16), revision (4), space-padded. */
        char inquiry[32];
        snprintf(inquiry, sizeof(inquiry), "%-8.8s%-16.16s%04x", c->inquiry_vendor, c->inquiry_product, 0);
        write_attr("f_mass_storage/inquiry_string", inquiry);
    }
}

/* One set-up: the stock order, then the functions read back. */
static bool write_setup(const board_android_usb_config_t * set, bool storage) {
    if (!set || !set->functions) return false;
    bool ok = write_attr("enable", "0");
    write_strings(storage);
    if (set->device_class) ok = write_attr("bDeviceClass", set->device_class) && ok;
    if (set->device_subclass) ok = write_attr("bDeviceSubClass", set->device_subclass) && ok;
    if (set->device_protocol) ok = write_attr("bDeviceProtocol", set->device_protocol) && ok;
    ok = write_attr("idVendor", set->id_vendor) && ok;
    ok = write_attr("idProduct", set->id_product) && ok;
    ok = write_attr("functions", set->functions) && ok;
    ok = write_attr("enable", "1") && ok;
    ok = ok && functions_are(set->functions);
    fprintf(stderr, "usb_gadget: android_usb %s:%s functions=%s -> %s\n", set->id_vendor, set->id_product,
            set->functions, ok ? "ok" : "FAILED");
    return ok;
}

/* The idle set-up is already there (the usual case after K90adb): nothing
 * to rewrite, so the computer does not see a needless re-enumeration. */
static bool idle_in_place(void) {
    const board_android_usb_config_t * idle = &cfg()->idle;
    char vid[16], cls[8];
    return functions_are(idle->functions) && read_attr("idVendor", vid, sizeof(vid)) &&
           strcasecmp(vid, idle->id_vendor) == 0 &&
           (!idle->device_class || (read_attr("bDeviceClass", cls, sizeof(cls)) && strcmp(cls, idle->device_class) == 0));
}

static bool ensure_idle(void) {
    return idle_in_place() || write_setup(&cfg()->idle, false);
}

/* Leaving DAC or a lent card puts the idle set-up back (K90adb would not
 * reset bDeviceClass 239). Anything else -- the plain ADB on/off of today --
 * leaves the gadget to K90adb, exactly as before. */
static bool leave_dac_or_storage(void) {
    if (!functions_contain("uac_sa") && !functions_contain("mass_storage")) return true;
    return write_setup(&cfg()->idle, false);
}

static const char * adb_flag(void) { return usb_board()->paths.adb_flag; }

static int run_k90adb(bool start) {
    char * argv[] = { (char *) "/etc/init.d/K90adb", (char *) (start ? "start" : "stop"), NULL };
    int rc = usb_run(argv, SCRIPT_TIMEOUT_MS);
    fprintf(stderr, "usb_mode_control: K90adb %s exit=%d\n", start ? "start" : "stop", rc);
    return rc;
}

static void set_adb_flag(bool on) {
    const char * flag = adb_flag();
    if (!flag) return;
    if (on) {
        FILE * f = fopen(flag, "w");
        if (f) fclose(f);
    } else {
        unlink(flag);
    }
}

static bool android_usb_apply(usb_mode_t mode) {
    if (!cfg()) return false;
    switch (mode) {
        case USB_MODE_ADB:
            leave_dac_or_storage();
            set_adb_flag(true);
            return run_k90adb(true) == 0 && functions_are(cfg()->idle.functions);
        case USB_MODE_CHARGE:
        case USB_MODE_STORAGE: {
            /* ADB off, the idle set-up; in Storage the card is lent once a
             * computer configures the port (usb_storage_lend.h). */
            set_adb_flag(false);
            bool stopped = run_k90adb(false) == 0;
            return leave_dac_or_storage() && stopped;
        }
        case USB_MODE_DAC: {
            set_adb_flag(false);
            run_k90adb(false);
            const char * dsd = usb_board()->usb.dsd_native_enable;
            if (dsd) usb_sysfs_write(dsd, (usb_board()->caps & CAP_USB_DAC_DSD) ? "on" : "off");
            return write_setup(&cfg()->dac, false);
        }
    }
    return false;
}

static bool android_usb_detect(usb_mode_t * out_mode) {
    if (!cfg()) return false;
    if (functions_contain("uac_sa")) *out_mode = USB_MODE_DAC;
    else if (functions_contain("mass_storage")) *out_mode = USB_MODE_STORAGE; /* lent */
    else if (adb_flag() && access(adb_flag(), F_OK) == 0) *out_mode = USB_MODE_ADB;
    else *out_mode = USB_MODE_CHARGE;
    return true;
}

static bool android_usb_host_connected(void) {
    char state[32];
    return cfg() && read_attr("state", state, sizeof(state)) && strcmp(state, "CONFIGURED") == 0;
}

/* The stock usb_mgr_is_mass_storage_connect() without its soft-remove flag
 * (usb_storage_lend.c keeps that). */
static bool android_usb_storage_host_configured(void) {
    return android_usb_host_connected() && functions_contain("mass_storage");
}

static bool android_usb_set_storage_function(bool on) {
    if (!cfg()) return false;
    return on ? write_setup(&cfg()->storage, true) : ensure_idle();
}

const struct board_usb_gadget_ops usb_gadget_android_usb_ops = {
    .name = "android_usb",
    .apply = android_usb_apply,
    .detect = android_usb_detect,
    .storage_host_configured = android_usb_storage_host_configured,
    .host_connected = android_usb_host_connected,
    .set_storage_function = android_usb_set_storage_function,
    .eject_storage = NULL, /* the card is lent: reclaim empties the LUN */
};
