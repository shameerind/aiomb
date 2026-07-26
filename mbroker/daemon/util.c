#include "common.h"
#include "util.h"
#include "logger.h"

// Create directory recursively (like mkdir -p)
int mkdir_p(const char *path, mode_t mode) {
    char tmp[512];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '/')
        tmp[len - 1] = 0;
    
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, mode) < 0 && errno != EEXIST) {
                if (errno != EEXIST) {
                    log_write("Failed to create directory %s: %s\n", tmp, strerror(errno));
                }
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) < 0 && errno != EEXIST) {
        if (errno != EEXIST) {
            log_write("Failed to create directory %s: %s\n", tmp, strerror(errno));
        }
        return -1;
    }
    return 0;
}
