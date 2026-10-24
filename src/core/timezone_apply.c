#include "board.h"
#include "board_config.h"
#include "timezone_apply.h"
#include "debug_log.h"
#include "timezone_posix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef HOST_BUILD
#define ZONEINFO_DIR "/usr/share/zoneinfo/"
#define LOCALTIME_PATH BOARD_DATA_ROOT "/localtime"
#define LOCALTIME_TMP_PATH LOCALTIME_PATH ".tmp"
#endif

bool timezone_apply(const char * iana_id) {
    if (!iana_id || iana_id[0] == '\0') return false;
    if (BOARD.system.posix_tz) {
        /* No zoneinfo tree in this firmware (R3 Pro: only a fixed CST-8
         * /etc/localtime, and no stock time zone setting): TZ is the zone's
         * POSIX rule, set for this process only. Unknown ids leave the
         * current TZ untouched. */
        const char * rule = timezone_posix_rule(iana_id);
        if (!rule) {
            DBG_LOG("timezone_apply: no rule for %s\n", iana_id);
            return false;
        }
        setenv("TZ", rule, 1);
        tzset();
        return true;
    }

    setenv("TZ", iana_id, 1);
    tzset();

#ifdef HOST_BUILD
    return true; /* in-process TZ only -- see this file's own header comment */
#else
    char src_path[256];
    snprintf(src_path, sizeof(src_path), "%s%s", ZONEINFO_DIR, iana_id);
    FILE * src = fopen(src_path, "rb");
    if (!src) {
        DBG_LOG("timezone_apply: can't open %s\n", src_path);
        return false;
    }

    FILE * dst = fopen(LOCALTIME_TMP_PATH, "wb");
    if (!dst) {
        DBG_LOG("timezone_apply: can't open %s for writing\n", LOCALTIME_TMP_PATH);
        fclose(src);
        return false;
    }

    bool ok = true;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        if (fwrite(buf, 1, n, dst) != n) {
            ok = false;
            break;
        }
    }
    fclose(src);
    fclose(dst);

    if (ok && rename(LOCALTIME_TMP_PATH, LOCALTIME_PATH) != 0) ok = false;
    if (!ok) remove(LOCALTIME_TMP_PATH);

    DBG_LOG("timezone_apply: %s -> %s (%s)\n", src_path, LOCALTIME_PATH, ok ? "ok" : "failed");
    return ok;
#endif
}
