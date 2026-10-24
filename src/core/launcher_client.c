#define _GNU_SOURCE
#include "launcher_client.h"
#include "board.h"
#include "board_launcher.h"
#include "storage_paths.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static bool regular(const char * dir, const char * name) {
    char path[PATH_MAX];
    struct stat st;
    return snprintf(path, sizeof(path), "%s/%s", dir, name) < (int) sizeof(path) && stat(path, &st) == 0 &&
           S_ISREG(st.st_mode);
}

static void put(char * out, size_t size, const char * name) {
    if (out && size) snprintf(out, size, "%s", name);
}

/* Roughly the launcher's search (src/launcher/lx_fw.c lx_fw_find, lx_pkg.c
 * lx_pkg_find): enough to tell the user an update is waiting; the name shown
 * can differ from the one the launcher picks when several are on the card. */
bool launcher_update_on_card(char * name, size_t size) {
    const struct board_launcher_ops * ops = BOARD.launcher;
    if (!ops) return false;
    char own[64];
    snprintf(own, sizeof(own), "%s.upt", BOARD.name);
    if (regular(MT_SD_ROOT, own)) return put(name, size, own), true;
    for (int i = 0; ops->firmware_images && ops->firmware_images[i]; i++)
        if (regular(MT_SD_ROOT, ops->firmware_images[i])) return put(name, size, ops->firmware_images[i]), true;
    bool found = false;
    DIR * d;
    if (ops->firmware_any_upt && (d = opendir(MT_SD_ROOT))) {
        struct dirent * e;
        while (!found && (e = readdir(d))) {
            const char * ext = strrchr(e->d_name, '.');
            if (e->d_name[0] != '.' && ext && strcasecmp(ext, ".upt") == 0 && regular(MT_SD_ROOT, e->d_name)) {
                put(name, size, e->d_name);
                found = true;
            }
        }
        closedir(d);
    }
    if (found) return true;
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "mellowtone-%s-", BOARD.name);
    if ((d = opendir(MT_UPDATE_DIR))) {
        struct dirent * e;
        size_t pl = strlen(prefix);
        while (!found && (e = readdir(d))) {
            size_t n = strlen(e->d_name);
            if (n > pl + 4 && strncmp(e->d_name, prefix, pl) == 0 && strcmp(e->d_name + n - 4, ".tar") == 0 &&
                regular(MT_UPDATE_DIR, e->d_name)) {
                put(name, size, e->d_name);
                found = true;
            }
        }
        closedir(d);
    }
    return found;
}

bool launcher_request_menu(void) {
    const struct board_launcher_ops * ops = BOARD.launcher;
#ifdef HOST_BUILD
    const char * dir = "./mellowtone-boot";
#else
    const char * dir = ops ? ops->boot_dir : NULL;
#endif
    if (!ops || !dir) return false;
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) return false;
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/" BOARD_LAUNCHER_MENU_FLAG, dir) >= (int) sizeof(path)) return false;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    bool ok = fsync(fd) == 0;
    if (close(fd) != 0) ok = false;
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        (void) fsync(dfd);
        close(dfd);
    }
    return ok;
}

const char * launcher_notice(void) {
    const char * n = getenv("MELLOWTONE_NOTICE");
    return n && n[0] ? n : NULL;
}
