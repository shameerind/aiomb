#pragma once

#define MREPOD_CONF_PATH "/etc/mrepod/mrepod.conf"
#define MAX_POST_MOUNT_CHOWN_DIRS 32
#define MAX_POST_MOUNT_CHOWN_DIR_LEN 512
#define MAX_POST_MOUNT_WHITEOUT_DIRS 64
#define MAX_POST_MOUNT_WHITEOUT_DIR_LEN 512

struct daemon_config {
    char overlayroot[2048];
    int post_mount_chown_dir_count;
    char post_mount_chown_dirs[MAX_POST_MOUNT_CHOWN_DIRS][MAX_POST_MOUNT_CHOWN_DIR_LEN];
    int post_mount_whiteout_dir_count;
    char post_mount_whiteout_dirs[MAX_POST_MOUNT_WHITEOUT_DIRS][MAX_POST_MOUNT_WHITEOUT_DIR_LEN];
};

int  config_load(const char *path);
const struct daemon_config *config_get(void);
