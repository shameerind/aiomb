#include "common.h"
#include "daemon_config.h"
#include "logger.h"
#include <ctype.h>

static struct daemon_config cfg;

static char *strip(char *s);

static int is_valid_relative_dir(const char *path)
{
    if (!path || !path[0])
        return 0;
    if (path[0] == '/')
        return 0;
    if (strstr(path, ".."))
        return 0;
    return 1;
}

static int is_valid_relative_pattern(const char *path)
{
    if (!path || !path[0])
        return 0;
    if (path[0] == '/')
        return 0;
    if (strstr(path, ".."))
        return 0;
    return 1;
}

static void parse_post_mount_chown_dirs(const char *val)
{
    cfg.post_mount_chown_dir_count = 0;
    if (!val || !val[0])
        return;

    char buf[4096];
    strncpy(buf, val, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *saveptr = NULL;
    char *tok = strtok_r(buf, ",", &saveptr);
    while (tok && cfg.post_mount_chown_dir_count < MAX_POST_MOUNT_CHOWN_DIRS) {
        char *item = strip(tok);
        if (is_valid_relative_dir(item)) {
            strncpy(cfg.post_mount_chown_dirs[cfg.post_mount_chown_dir_count],
                    item, MAX_POST_MOUNT_CHOWN_DIR_LEN - 1);
            cfg.post_mount_chown_dirs[cfg.post_mount_chown_dir_count][MAX_POST_MOUNT_CHOWN_DIR_LEN - 1] = '\0';
            cfg.post_mount_chown_dir_count++;
        } else {
            log_write("Warning: ignoring invalid post_mount_chown_dirs entry '%s'\n",
                      item ? item : "");
        }
        tok = strtok_r(NULL, ",", &saveptr);
    }

    if (cfg.post_mount_chown_dir_count == MAX_POST_MOUNT_CHOWN_DIRS && tok) {
        log_write("Warning: post_mount_chown_dirs truncated to %d entries\n",
                  MAX_POST_MOUNT_CHOWN_DIRS);
    }
}

static void parse_post_mount_whiteout_dirs(const char *val)
{
    cfg.post_mount_whiteout_dir_count = 0;
    if (!val || !val[0])
        return;

    char buf[4096];
    strncpy(buf, val, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *saveptr = NULL;
    char *tok = strtok_r(buf, ",", &saveptr);
    while (tok && cfg.post_mount_whiteout_dir_count < MAX_POST_MOUNT_WHITEOUT_DIRS) {
        char *item = strip(tok);
        if (is_valid_relative_pattern(item)) {
            strncpy(cfg.post_mount_whiteout_dirs[cfg.post_mount_whiteout_dir_count],
                    item, MAX_POST_MOUNT_WHITEOUT_DIR_LEN - 1);
            cfg.post_mount_whiteout_dirs[cfg.post_mount_whiteout_dir_count][MAX_POST_MOUNT_WHITEOUT_DIR_LEN - 1] = '\0';
            cfg.post_mount_whiteout_dir_count++;
        } else {
            log_write("Warning: ignoring invalid post_mount_whiteout_dirs entry '%s'\n",
                      item ? item : "");
        }
        tok = strtok_r(NULL, ",", &saveptr);
    }

    if (cfg.post_mount_whiteout_dir_count == MAX_POST_MOUNT_WHITEOUT_DIRS && tok) {
        log_write("Warning: post_mount_whiteout_dirs truncated to %d entries\n",
                  MAX_POST_MOUNT_WHITEOUT_DIRS);
    }
}

static char *strip(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return s;
}

int config_load(const char *path)
{
    memset(&cfg, 0, sizeof(cfg));

    FILE *f = fopen(path, "r");
    if (!f) {
        log_write("Warning: cannot open config file '%s': %s\n",
                  path, strerror(errno));
        return -1;
    }

    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        /* Strip trailing newline */
        line[strcspn(line, "\n\r")] = '\0';

        char *s = strip(line);
        if (!*s || *s == '#')
            continue;

        char *eq = strchr(s, '=');
        if (!eq)
            continue;

        *eq = '\0';
        char *key = strip(s);
        char *val = strip(eq + 1);

        if (strcmp(key, "overlayroot") == 0) {
            strncpy(cfg.overlayroot, val, sizeof(cfg.overlayroot) - 1);
            cfg.overlayroot[sizeof(cfg.overlayroot) - 1] = '\0';
        } else if (strcmp(key, "post_mount_chown_dirs") == 0) {
            parse_post_mount_chown_dirs(val);
        } else if (strcmp(key, "post_mount_whiteout_dirs") == 0) {
            parse_post_mount_whiteout_dirs(val);
        } else if (strcmp(key, "ml_enabled") == 0) {
            cfg.ml_enabled = atoi(val);
        } else if (strcmp(key, "model_socket") == 0) {
            strncpy(cfg.model_socket, val, sizeof(cfg.model_socket) - 1);
            cfg.model_socket[sizeof(cfg.model_socket) - 1] = '\0';
        } else if (strcmp(key, "nfs_servers") == 0) {
            /* Comma-separated list of NFS server hostnames */
            cfg.nfs_server_count = 0;
            char sbuf[4096];
            strncpy(sbuf, val, sizeof(sbuf) - 1);
            sbuf[sizeof(sbuf) - 1] = '\0';
            char *sp = NULL;
            char *stok = strtok_r(sbuf, ",", &sp);
            while (stok && cfg.nfs_server_count < MAX_NFS_SERVERS) {
                char *item = strip(stok);
                if (item[0]) {
                    strncpy(cfg.nfs_servers[cfg.nfs_server_count], item,
                            MAX_NFS_SERVER_HOST_LEN - 1);
                    cfg.nfs_servers[cfg.nfs_server_count][MAX_NFS_SERVER_HOST_LEN - 1] = '\0';
                    cfg.nfs_server_count++;
                }
                stok = strtok_r(NULL, ",", &sp);
            }
        }
        /* Additional keys can be added here */
    }

    fclose(f);
    /* Defaults for ML config if not set */
    if (!cfg.model_socket[0])
        strncpy(cfg.model_socket, "/run/mrepod/model.sock", sizeof(cfg.model_socket) - 1);

    log_write("Loaded config from '%s': overlayroot='%s', post_mount_chown_dirs=%d, "
              "post_mount_whiteout_dirs=%d, ml_enabled=%d, model_socket='%s', nfs_servers=%d\n",
              path, cfg.overlayroot, cfg.post_mount_chown_dir_count,
              cfg.post_mount_whiteout_dir_count, cfg.ml_enabled, cfg.model_socket,
              cfg.nfs_server_count);
    return 0;
}

const struct daemon_config *config_get(void)
{
    return &cfg;
}
