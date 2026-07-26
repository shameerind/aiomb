#ifndef MOUNT_UTIL_H
#define MOUNT_UTIL_H

#include <sys/stat.h>

int is_path_mounted(const char *path);
void ensure_tmp_log_mode(const char *path);

#endif // MOUNT_UTIL_H
