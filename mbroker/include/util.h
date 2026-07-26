#pragma once
#include <sys/stat.h>

// Create directory recursively (like mkdir -p)
int mkdir_p(const char *path, mode_t mode);
