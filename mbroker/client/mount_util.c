#include "mount_util.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

int is_path_mounted(const char *path) {
    FILE *f = fopen("/proc/mounts", "r");
    if (!f)
        return 0;
    char line[8192];
    while (fgets(line, sizeof(line), f)) {
        char mountpoint[4096] = {0};
        if (sscanf(line, "%*s %4095s %*s %*s %*d %*d", mountpoint) == 1) {
            if (strcmp(mountpoint, path) == 0) {
                fclose(f);
                return 1;
            }
        }
    }
    fclose(f);
    return 0;
}

void ensure_tmp_log_mode(const char *path) {
    if (!path || strncmp(path, "/tmp/", 5) != 0)
        return;
    if (chmod(path, 0777) < 0 && errno != ENOENT) {
        fprintf(stderr, "Warning: failed to chmod 0777 on %s: %s\n",
                path, strerror(errno));
    }
}
