#include "board.h"
#include "board_config.h"
#include "hostname_apply.h"
#include "debug_log.h"

#include <stdio.h>
#include <string.h>

#ifndef HOST_BUILD
#include <sys/mount.h>
#include <unistd.h>

#define HOSTNAME_OVERRIDE_FILE BOARD_DATA_ROOT "/hostname_override.txt"
#define STOCK_HOSTNAME_PATH "/usr/resource/hostname"
#define STOCK_BT_NAME_PATH "/usr/resource/bt_name"

static bool bind_mount_over(const char * source, const char * target) {
    return mount(source, target, NULL, MS_BIND, NULL) == 0;
}
#endif

bool hostname_apply(const char * hostname) {
    if (!hostname || hostname[0] == '\0') return true;

#ifdef HOST_BUILD
    return true; /* no real /usr/resource or /usr/data on a dev machine -- see this file's own header comment */
#else
    bool ok_hostname = true, ok_bt_name = true;
    if (!BOARD.system.keep_stock_names) {
        /* The override is bind-mounted over the stock files, so it must stay on
         * the non-removable data partition (a bind source on the card would pin
         * the card). Written only when the name changes. */
        char current[128] = "";
        FILE * cur = fopen(HOSTNAME_OVERRIDE_FILE, "r");
        if (cur) {
            if (!fgets(current, sizeof(current), cur)) current[0] = '\0';
            fclose(cur);
        }
        if (strcmp(current, hostname) != 0) {
            FILE * f = fopen(HOSTNAME_OVERRIDE_FILE, "w");
            if (!f) {
                DBG_LOG("hostname_apply: can't open %s for writing\n", HOSTNAME_OVERRIDE_FILE);
                return false;
            }
            fputs(hostname, f);
            fclose(f);
        }
        ok_hostname = bind_mount_over(HOSTNAME_OVERRIDE_FILE, STOCK_HOSTNAME_PATH);
        ok_bt_name = bind_mount_over(HOSTNAME_OVERRIDE_FILE, STOCK_BT_NAME_PATH);
    }
    /* else (R3 Pro): keep the stock Bluetooth name: bt_init and paired cars
     * read /usr/resource/bt_name. Only the runtime hostname changes, so no
     * override file is needed (the name itself is in settings.txt). */
    /* Best-effort, non-fatal to the app either way -- worst case the stock
     * name stays in effect for whichever of the two this couldn't override,
     * same tolerant style as timezone_apply(). */
    sethostname(hostname, strlen(hostname));

    DBG_LOG("hostname_apply: '%s' -> %s (%s), %s (%s)\n", hostname, STOCK_HOSTNAME_PATH,
            ok_hostname ? "ok" : "failed", STOCK_BT_NAME_PATH, ok_bt_name ? "ok" : "failed");
    return ok_hostname && ok_bt_name;
#endif
}
