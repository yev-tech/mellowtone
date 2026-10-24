/* Shared plumbing for the USB gadget and storage back ends: see usb_gadget.h. */
#include "usb_gadget.h"
#include "hiby_sys_client.h"
#include "subprocess.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const board_desc_t * test_board;
static const char * test_sysfs_root;
static const char * test_proc_root;
static usb_run_fn test_run;
static usb_sys_request_fn test_request;
static void (*test_trace)(const char * rel, const char * value);

void usb_gadget_test_trace(void (*trace)(const char * rel, const char * value)) { test_trace = trace; }

void usb_gadget_test_setup(const board_desc_t * board, const char * sysfs_root, const char * proc_root,
                           usb_run_fn run, usb_sys_request_fn request) {
    test_board = board;
    test_sysfs_root = sysfs_root;
    test_proc_root = proc_root;
    test_run = run;
    test_request = request;
}

const board_desc_t * usb_board(void) { return test_board ? test_board : &BOARD; }
const struct board_usb_gadget_ops * usb_gadget_ops(void) { return usb_board()->usb_gadget; }
const struct board_storage_export_ops * usb_storage_export_ops(void) { return usb_board()->storage_export; }
usb_sys_request_fn usb_sys_request(void) { return test_request ? test_request : hiby_sys_request; }

bool usb_sysfs_path(const char * rel, char * out, size_t out_size) {
    if (!rel || !rel[0]) return false;
    int n = snprintf(out, out_size, "%s/%s", test_sysfs_root ? test_sysfs_root : "/sys", rel);
    return n > 0 && (size_t) n < out_size;
}

bool usb_proc_path(const char * rel, char * out, size_t out_size) {
    int n = snprintf(out, out_size, "%s/%s", test_proc_root ? test_proc_root : "/proc", rel);
    return n > 0 && (size_t) n < out_size;
}

bool usb_sysfs_write(const char * rel, const char * value) {
    char path[PATH_MAX];
    if (!usb_sysfs_path(rel, path, sizeof(path))) return false;
    if (test_trace) test_trace(rel, value);
    /* O_TRUNC as a shell's "echo x > attr": harmless on sysfs, and a fake
     * tree's plain files then hold just the value. */
    int fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "usb_gadget: open %s: %s\n", path, strerror(errno));
        return false;
    }
    size_t len = strlen(value);
    ssize_t n = write(fd, value, len);
    bool ok = n == (ssize_t) len;
    if (!ok) fprintf(stderr, "usb_gadget: write \"%s\" > %s: %s\n", value, path, n < 0 ? strerror(errno) : "short");
    close(fd);
    return ok;
}

bool usb_sysfs_read(const char * rel, char * out, size_t out_size) {
    char path[PATH_MAX];
    if (!out_size || !usb_sysfs_path(rel, path, sizeof(path))) return false;
    out[0] = '\0';
    FILE * f = fopen(path, "re");
    if (!f) return false;
    bool ok = fgets(out, (int) out_size, f) != NULL || feof(f);
    fclose(f);
    out[strcspn(out, "\r\n")] = '\0';
    return ok;
}

int usb_run(char * const argv[], int timeout_ms) {
    if (test_run) return test_run(argv, timeout_ms);
#ifdef HOST_BUILD
    (void) argv;
    (void) timeout_ms;
    return -1;
#else
    int exit_code = -1;
    subprocess_run_checked(argv, NULL, 0, timeout_ms, &exit_code);
    return exit_code;
#endif
}

/* "under root": the path itself or something below it. */
static bool path_under(const char * path, const char * root) {
    size_t n = strlen(root);
    while (n > 1 && root[n - 1] == '/') n--;
    return strncmp(path, root, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

/* The kernel reports resolved paths: where the card root goes through a
 * symlink (/data -> /usr/data on the X1600 boards) both spellings count. */
typedef struct {
    const char * root;
    char real[PATH_MAX];
} card_root_t;

static bool under_card(const char * path, const card_root_t * card) {
    return path_under(path, card->root) || (card->real[0] && path_under(path, card->real));
}

static bool link_under(const char * link_path, const card_root_t * card) {
    char target[PATH_MAX];
    ssize_t n = readlink(link_path, target, sizeof(target) - 1);
    if (n <= 0) return false;
    target[n] = '\0';
    /* A file deleted while open reads "<path> (deleted)": still on the card. */
    return under_card(target, card);
}

/* A mapping of a file on the card (a library mapped with mmap, a file
 * mapped by a decoder) keeps it busy as an open file does, without an fd.
 * maps lines: "<range> <perms> <offset> <dev> <inode>   <path>". */
static bool maps_under(const char * maps_path, const card_root_t * card) {
    FILE * f = fopen(maps_path, "re");
    if (!f) return false;
    char line[PATH_MAX + 128];
    bool busy = false;
    while (!busy && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        char * path = strchr(line, '/');
        if (path) busy = under_card(path, card);
    }
    fclose(f);
    return busy;
}

int usb_busy_log_files(const char * root, int pid) {
    char proc[512], pid_dir[64];
    snprintf(pid_dir, sizeof(pid_dir), "%d", pid);
    if (!usb_proc_path(pid_dir, proc, sizeof(proc))) return 0;
    card_root_t card = { .root = root };
    if (!realpath(root, card.real) || strcmp(card.real, root) == 0) card.real[0] = '\0';
    int found = 0;
    char path[1024], target[PATH_MAX];
    snprintf(path, sizeof(path), "%s/cwd", proc);
    if (link_under(path, &card)) {
        fprintf(stderr, "usb_storage: pid %d works in a directory on the card\n", pid);
        found++;
    }
    snprintf(path, sizeof(path), "%s/fd", proc);
    DIR * fds = opendir(path);
    if (fds) {
        struct dirent * f;
        while ((f = readdir(fds)) != NULL) {
            if (f->d_name[0] == '.') continue;
            char fd_path[1024 + 300];
            snprintf(fd_path, sizeof(fd_path), "%s/%s", path, f->d_name);
            ssize_t n = readlink(fd_path, target, sizeof(target) - 1);
            if (n <= 0) continue;
            target[n] = '\0';
            if (!under_card(target, &card)) continue;
            fprintf(stderr, "usb_storage: pid %d fd %s: %s\n", pid, f->d_name, target);
            found++;
        }
        closedir(fds);
    }
    snprintf(path, sizeof(path), "%s/maps", proc);
    FILE * maps = fopen(path, "re");
    if (maps) {
        char line[PATH_MAX + 128];
        while (fgets(line, sizeof(line), maps)) {
            line[strcspn(line, "\n")] = '\0';
            char * mapped = strchr(line, '/');
            if (!mapped || !under_card(mapped, &card)) continue;
            fprintf(stderr, "usb_storage: pid %d maps %s\n", pid, mapped);
            found++;
        }
        fclose(maps);
    }
    return found;
}

int usb_busy_scan(const char * root, int * pids, char (*names)[32], int max) {
    char proc[512];
    if (!usb_proc_path("", proc, sizeof(proc))) return 0;
    card_root_t card = { .root = root };
    if (!realpath(root, card.real) || strcmp(card.real, root) == 0) card.real[0] = '\0';
    DIR * dir = opendir(proc);
    if (!dir) return 0;
    int found = 0;
    struct dirent * e;
    while ((e = readdir(dir)) != NULL) {
        if (!isdigit((unsigned char) e->d_name[0])) continue;
        char path[1024];
        bool busy = false;
        snprintf(path, sizeof(path), "%s%s/cwd", proc, e->d_name);
        busy = link_under(path, &card);
        snprintf(path, sizeof(path), "%s%s/fd", proc, e->d_name);
        DIR * fds = busy ? NULL : opendir(path);
        if (fds) {
            struct dirent * f;
            while (!busy && (f = readdir(fds)) != NULL) {
                if (f->d_name[0] == '.') continue;
                char fd_path[1024 + 300];
                snprintf(fd_path, sizeof(fd_path), "%s/%s", path, f->d_name);
                busy = link_under(fd_path, &card);
            }
            closedir(fds);
        }
        if (!busy) {
            snprintf(path, sizeof(path), "%s%s/maps", proc, e->d_name);
            busy = maps_under(path, &card);
        }
        if (!busy) continue;
        if (found < max) {
            if (pids) pids[found] = atoi(e->d_name);
            if (names) {
                names[found][0] = '\0';
                snprintf(path, sizeof(path), "%s%s/comm", proc, e->d_name);
                FILE * c = fopen(path, "re");
                if (c) {
                    if (fgets(names[found], 32, c)) names[found][strcspn(names[found], "\n")] = '\0';
                    fclose(c);
                }
            }
        }
        found++;
    }
    closedir(dir);
    return found;
}
