#include "common.h"
#include <sys/mount.h>
#include <libgen.h>
#include <pthread.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include "mount_ops.h"
#include "umount_ops.h"
#include "mountinfo.h"
#include "locks.h"
#include "namespace.h"
#include "logger.h"
#include "daemon_config.h"
#include "protocol.h"

/* Local copy — mirrors the identical helper in mount_ops.c */
static int line_matches_mountpath(const char *line, const char *mountpath)
{
    while (*line && isspace((unsigned char)*line))
        line++;

    size_t path_len = strlen(mountpath);
    if (strncmp(line, mountpath, path_len) != 0)
        return 0;

    return line[path_len] == '\0' || isspace((unsigned char)line[path_len]);
}

static int remove_overlay_export(const char *mountpath) {
    FILE *in = fopen(EXPORTS_FILE, "r");
    if (!in) {
        log_write("Failed to open %s for read: %s\n", EXPORTS_FILE, strerror(errno));
        return -1;
    }

    char tmpfile[256];
    snprintf(tmpfile, sizeof(tmpfile), "/etc/exports.mrepod.%d.tmp", (int)getpid());
    FILE *out = fopen(tmpfile, "w");
    if (!out) {
        log_write("Failed to open temp file %s: %s\n", tmpfile, strerror(errno));
        fclose(in);
        return -1;
    }

    char line[8192];
    int removed = 0;
    while (fgets(line, sizeof(line), in)) {
        if (line_matches_mountpath(line, mountpath)) {
            removed = 1;
            continue;
        }
        fputs(line, out);
    }

    fclose(in);
    if (fclose(out) != 0) {
        log_write("Failed to flush temp file %s: %s\n", tmpfile, strerror(errno));
        unlink(tmpfile);
        return -1;
    }

    if (rename(tmpfile, EXPORTS_FILE) < 0) {
        log_write("Failed to replace %s with %s: %s\n", EXPORTS_FILE, tmpfile, strerror(errno));
        unlink(tmpfile);
        return -1;
    }

    if (removed) {
        log_write("Removed export entry for %s\n", mountpath);
        return run_exportfs_refresh();
    }

    log_write("No export entry found for %s\n", mountpath);
    return 0;
}

static int remove_dir_recursive(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) {
        if (errno == ENOENT)
            return 0;
        log_write("Failed to open directory %s for cleanup: %s\n", path, strerror(errno));
        return -1;
    }

    struct dirent *entry;
    int ret = 0;

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char child[4096];
        int n = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (n < 0 || n >= (int)sizeof(child)) {
            log_write("Cleanup path too long under %s\n", path);
            ret = -1;
            continue;
        }

        struct stat st;
        if (lstat(child, &st) < 0) {
            if (errno != ENOENT) {
                log_write("Failed to stat %s during cleanup: %s\n", child, strerror(errno));
                ret = -1;
            }
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            if (remove_dir_recursive(child) < 0)
                ret = -1;
        } else {
            if (unlink(child) < 0 && errno != ENOENT) {
                log_write("Failed to remove file %s during cleanup: %s\n", child, strerror(errno));
                ret = -1;
            }
        }
    }

    closedir(dir);

    if (rmdir(path) < 0 && errno != ENOENT) {
        log_write("Failed to remove directory %s during cleanup: %s\n", path, strerror(errno));
        ret = -1;
    }

    return ret;
}

struct async_remove_req {
    char path[4096];
    char label[32];
};

static void *async_remove_dir_thread(void *arg) {
    struct async_remove_req *req = (struct async_remove_req *)arg;
    int rc = remove_dir_recursive(req->path);
    if (rc < 0)
        log_write("Warning: async cleanup failed for %s %s\n", req->label, req->path);
    else
        log_write("Async cleanup completed for %s %s\n", req->label, req->path);
    free(req);
    return NULL;
}

static int remove_dir_async(const char *path, const char *label) {
    if (!path || !path[0])
        return -EINVAL;

    struct async_remove_req *req = calloc(1, sizeof(*req));
    if (!req)
        return -ENOMEM;

    snprintf(req->path, sizeof(req->path), "%s", path);
    snprintf(req->label, sizeof(req->label), "%s", label ? label : "dir");

    pthread_t tid;
    if (pthread_create(&tid, NULL, async_remove_dir_thread, req) != 0) {
        int saved = errno;
        free(req);
        return -saved;
    }
    pthread_detach(tid);
    return 0;
}

void cleanup_custom_nfs_mounts(const char *overlayroot,
                                const char *sandboxname)
{
    char sb_copy[2048];
    strncpy(sb_copy, sandboxname, sizeof(sb_copy) - 1);
    sb_copy[sizeof(sb_copy) - 1] = '\0';
    char *sb_base = basename(sb_copy);

    for (int i = 0; i < MAX_CUSTOM_LOWERDIR_COMPONENTS; i++) {
        char target[4096];
        snprintf(target, sizeof(target), "%s/%s-clower-%d",
                 overlayroot, sb_base, i);
        if (!mountinfo_is_mounted(target))
            break;  /* No more custom NFS mounts */
        log_write("Unmounting custom NFS lowerdir at %s\n", target);
        if (umount2(target, MNT_DETACH) < 0)
            log_write("Warning: failed to unmount custom NFS lowerdir %s: %s\n",
                      target, strerror(errno));
    }
}

int umount_sandbox(const char *sandboxname, int flags, uid_t requester_uid) {
    log_write("Unmounting sandbox '%s'\n", sandboxname);

    struct stat st;
    if (stat(sandboxname, &st) < 0) {
        log_write("Failed to stat sandbox mountpoint '%s': %s\n", sandboxname, strerror(errno));
        return -errno;
    }

    if (requester_uid != 0 && st.st_uid != requester_uid) {
        log_write("Destroy denied for '%s': requester uid=%d owner uid=%d\n",
                  sandboxname, (int)requester_uid, (int)st.st_uid);
        return -EPERM;
    }

    /* Get overlayroot from tracking or fall back to daemon config */
    struct mount_ctx ctx = {0};
    const char *tracked_overlayroot = mountinfo_overlayroot_for(sandboxname);
    if (tracked_overlayroot && tracked_overlayroot[0]) {
        strncpy(ctx.overlayroot, tracked_overlayroot, sizeof(ctx.overlayroot) - 1);
        ctx.overlayroot[sizeof(ctx.overlayroot) - 1] = '\0';
    } else {
        const struct daemon_config *dcfg = config_get();
        strncpy(ctx.overlayroot, dcfg->overlayroot, sizeof(ctx.overlayroot) - 1);
        ctx.overlayroot[sizeof(ctx.overlayroot) - 1] = '\0';
    }

    if (!ctx.overlayroot[0]) {
        log_write("Error: cannot determine overlayroot for '%s'\n", sandboxname);
        return -EINVAL;
    }

    lock_sandbox(sandboxname);
    enter_namespace();

    int umount_flags = (flags & FLAG_LAZY ? MNT_DETACH : 0) |
                       (flags & FLAG_FORCE ? MNT_FORCE : 0);

    if (umount2(sandboxname, umount_flags) < 0) {
        int saved = errno;
        log_write("Failed to unmount overlay at %s: %s\n", sandboxname, strerror(saved));
        if (!(umount_flags & MNT_DETACH)) {
            log_write("Retrying unmount of %s with MNT_DETACH...\n", sandboxname);
            if (umount2(sandboxname, MNT_DETACH) < 0) {
                log_write("Lazy unmount of %s also failed: %s\n", sandboxname, strerror(errno));
                exit_namespace();
                unlock_sandbox(sandboxname);
                return -saved;
            }
            log_write("Lazy unmount of %s succeeded\n", sandboxname);
        } else {
            exit_namespace();
            unlock_sandbox(sandboxname);
            return -saved;
        }
    }

    if (setup_overlay_dirs(&ctx, sandboxname) < 0) {
        log_write("Warning: failed to resolve overlay dirs for cleanup on %s\n", sandboxname);
    } else {
        char work_gc_path[4096];
        int wn = snprintf(work_gc_path, sizeof(work_gc_path),
                          "%s.mrepod.gc.%ld.%d",
                          ctx.workdir, (long)time(NULL), (int)getpid());
        if (wn <= 0 || wn >= (int)sizeof(work_gc_path)) {
            log_write("Warning: async cleanup name too long for workdir %s; using sync cleanup\n", ctx.workdir);
            if (remove_dir_recursive(ctx.workdir) < 0)
                log_write("Warning: failed to cleanup workdir %s\n", ctx.workdir);
        } else if (rename(ctx.workdir, work_gc_path) < 0) {
            if (errno != ENOENT) {
                log_write("Warning: failed to stage workdir %s for async cleanup: %s; using sync cleanup\n",
                          ctx.workdir, strerror(errno));
                if (remove_dir_recursive(ctx.workdir) < 0)
                    log_write("Warning: failed to cleanup workdir %s\n", ctx.workdir);
            }
        } else {
            int wrc = remove_dir_async(work_gc_path, "workdir");
            if (wrc < 0) {
                log_write("Warning: failed to start async cleanup for workdir %s: %s; cleaning synchronously\n",
                          work_gc_path, strerror(-wrc));
                if (remove_dir_recursive(work_gc_path) < 0)
                    log_write("Warning: failed to cleanup staged workdir %s\n", work_gc_path);
            }
        }

        char upper_gc_path[4096];
        int rn = snprintf(upper_gc_path, sizeof(upper_gc_path),
                          "%s.mrepod.gc.%ld.%d",
                          ctx.upperdir, (long)time(NULL), (int)getpid());
        if (rn <= 0 || rn >= (int)sizeof(upper_gc_path)) {
            log_write("Warning: async cleanup name too long for upperdir %s; using sync cleanup\n", ctx.upperdir);
            if (remove_dir_recursive(ctx.upperdir) < 0)
                log_write("Warning: failed to cleanup upperdir %s\n", ctx.upperdir);
        } else if (rename(ctx.upperdir, upper_gc_path) < 0) {
            if (errno != ENOENT) {
                log_write("Warning: failed to stage upperdir %s for async cleanup: %s; using sync cleanup\n",
                          ctx.upperdir, strerror(errno));
                if (remove_dir_recursive(ctx.upperdir) < 0)
                    log_write("Warning: failed to cleanup upperdir %s\n", ctx.upperdir);
            }
        } else {
            int arc = remove_dir_async(upper_gc_path, "upperdir");
            if (arc < 0) {
                log_write("Warning: failed to start async cleanup for upperdir %s: %s; cleaning synchronously\n",
                          upper_gc_path, strerror(-arc));
                if (remove_dir_recursive(upper_gc_path) < 0)
                    log_write("Warning: failed to cleanup staged upperdir %s\n", upper_gc_path);
            }
        }
    }

    /* Remove mountinfo.json and the sandbox directory under overlayroot */
    {
        char sb_copy2[2048];
        snprintf(sb_copy2, sizeof(sb_copy2), "%s", sandboxname);
        char *sb_base2 = basename(sb_copy2);
        char record_path[4096];
        int rpn = snprintf(record_path, sizeof(record_path),
                           "%s/%s/mountinfo.json", ctx.overlayroot, sb_base2);
        if (rpn > 0 && rpn < (int)sizeof(record_path)) {
            if (unlink(record_path) < 0 && errno != ENOENT)
                log_write("Warning: failed to remove mount record %s: %s\n",
                          record_path, strerror(errno));
        }
        /* Try to remove the now-empty sandbox directory */
        char sb_dir[4096];
        int sdn = snprintf(sb_dir, sizeof(sb_dir),
                           "%s/%s", ctx.overlayroot, sb_base2);
        if (sdn > 0 && sdn < (int)sizeof(sb_dir)) {
            if (rmdir(sb_dir) < 0 && errno != ENOENT && errno != ENOTEMPTY)
                log_write("Warning: failed to remove sandbox dir %s: %s\n",
                          sb_dir, strerror(errno));
        }
    }

    int tracked_flags = mountinfo_flags_for(sandboxname);
    mountinfo_untrack(sandboxname);
    if (remove_overlay_export(sandboxname) < 0) {
        log_write("Warning: failed to update %s during unmount of %s\n", EXPORTS_FILE, sandboxname);
    }
    /* Unmount any NFS lower mounts created for this sandbox */
    cleanup_custom_nfs_mounts(ctx.overlayroot, sandboxname);
    (void)tracked_flags;
    exit_namespace();

    unlock_sandbox(sandboxname);

    return 0;
}
