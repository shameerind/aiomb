#pragma once

#define MREPOD_CONF_PATH "/etc/mrepod/mrepod.conf"
#define MAX_POST_MOUNT_CHOWN_DIRS 32
#define MAX_POST_MOUNT_CHOWN_DIR_LEN 512
#define MAX_POST_MOUNT_WHITEOUT_DIRS 64
#define MAX_POST_MOUNT_WHITEOUT_DIR_LEN 512
#define MAX_NFS_SERVERS 16
#define MAX_NFS_SERVER_HOST_LEN 256

struct daemon_config {
    char overlayroot[2048];
    int post_mount_chown_dir_count;
    char post_mount_chown_dirs[MAX_POST_MOUNT_CHOWN_DIRS][MAX_POST_MOUNT_CHOWN_DIR_LEN];
    int post_mount_whiteout_dir_count;
    char post_mount_whiteout_dirs[MAX_POST_MOUNT_WHITEOUT_DIRS][MAX_POST_MOUNT_WHITEOUT_DIR_LEN];

    /* ML inference sidecar settings */
    int  ml_enabled;                                          /* 0=disabled, 1=enabled */
    char model_socket[256];                                   /* UNIX socket path      */
    int  nfs_server_count;
    char nfs_servers[MAX_NFS_SERVERS][MAX_NFS_SERVER_HOST_LEN];
};

int  config_load(const char *path);
const struct daemon_config *config_get(void);
