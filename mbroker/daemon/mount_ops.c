#include "common.h"
#include <sys/mount.h>
#include <libgen.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <jansson.h>
#include <pthread.h>
#include "mount_ops.h"
#include "util.h"
#include "protocol.h"
#include "mountinfo.h"
#include "locks.h"
#include "namespace.h"
#include "fault.h"
#include "logger.h"
#include "daemon_config.h"
#include "post_mount.h"
#include "umount_ops.h"
#include <dirent.h>
#include <ctype.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/vfs.h>
#include <sys/xattr.h>
#include <fnmatch.h>
#include <glob.h>
#include <pthread.h>

#define OVERLAYFS_SUPER_MAGIC 0x794c7630

int run_exportfs_refresh(void) {
    int rc = system("timeout 20s /sbin/exportfs -ra >/dev/null 2>&1");
    if (rc == -1) {
        log_write("Failed to invoke shell for 'exportfs -ra': %s\n", strerror(errno));
        return -1;
    }
    if (WIFSIGNALED(rc)) {
        log_write("'exportfs -ra' terminated by signal %d\n", WTERMSIG(rc));
        return -1;
    }
    if (!WIFEXITED(rc) || WEXITSTATUS(rc) != 0) {
        log_write("Failed to run 'exportfs -ra' (exit=%d raw=%d)\n",
                  WIFEXITED(rc) ? WEXITSTATUS(rc) : -1, rc);
        return -1;
    }
    log_write("Successfully refreshed NFS exports with 'exportfs -ra'\n");
    return 0;
}

int refresh_exports(void) {
    return run_exportfs_refresh();
}

static int line_matches_mountpath(const char *line, const char *mountpath) {
    while (*line && isspace((unsigned char)*line))
        line++;

    size_t path_len = strlen(mountpath);
    if (strncmp(line, mountpath, path_len) != 0)
        return 0;

    return line[path_len] == '\0' || isspace((unsigned char)line[path_len]);
}

static int generate_fsid_uuid(char *buf, size_t n) {
    int have_uuidgen = (access("/bin/uuidgen", X_OK) == 0);

    if (!have_uuidgen) {
        unsigned int seed = (unsigned int)time(NULL) ^ (unsigned int)getpid();
        srand(seed);
        int written = snprintf(
            buf,
            n,
            "%08x-%04x-%04x-%04x-%08x%04x",
            (unsigned int)rand(),
            (unsigned int)(rand() & 0xFFFF),
            (unsigned int)(rand() & 0xFFFF),
            (unsigned int)(rand() & 0xFFFF),
            (unsigned int)rand(),
            (unsigned int)(rand() & 0xFFFF));
        if (written <= 0 || (size_t)written >= n) {
            log_write("Failed to generate fallback random fsid\n");
            return -1;
        }
        log_write("uuidgen not found; using random fsid '%s'\n", buf);
        return 0;
    }

    FILE *fp = popen("uuidgen -r 2>/dev/null", "r");
    if (!fp) {
        log_write("Failed to execute 'uuidgen -r'\n");
        return -1;
    }

    if (!fgets(buf, n, fp)) {
        pclose(fp);
        log_write("Failed to read UUID from 'uuidgen -r'\n");
        return -1;
    }

    int rc = pclose(fp);
    if (rc != 0) {
        log_write("Command 'uuidgen -r' failed (rc=%d)\n", rc);
        return -1;
    }

    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
        buf[len - 1] = '\0';
        len--;
    }

    return len > 0 ? 0 : -1;
}

static int ensure_overlay_export(const char *mountpath) {
    FILE *f = fopen(EXPORTS_FILE, "r");
    if (!f) {
        log_write("Failed to open %s for read: %s\n", EXPORTS_FILE, strerror(errno));
        return -1;
    }

    char line[8192];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line_matches_mountpath(line, mountpath)) {
            found = 1;
            break;
        }
    }
    fclose(f);

    if (!found) {
        char fsid[128] = {0};
        if (generate_fsid_uuid(fsid, sizeof(fsid)) < 0) {
            log_write("Failed to generate fsid UUID for export %s\n", mountpath);
            return -1;
        }

        /* Re-read existing content, then write new entry above all others */
        f = fopen(EXPORTS_FILE, "r");
        char *existing = NULL;
        size_t existing_len = 0;
        if (f) {
            fseek(f, 0, SEEK_END);
            long fsize = ftell(f);
            if (fsize > 0) {
                rewind(f);
                existing = malloc((size_t)fsize + 1);
                if (existing) {
                    existing_len = fread(existing, 1, (size_t)fsize, f);
                    existing[existing_len] = '\0';
                }
            }
            fclose(f);
        }

        f = fopen(EXPORTS_FILE, "w");
        if (!f) {
            log_write("Failed to open %s for write: %s\n", EXPORTS_FILE, strerror(errno));
            free(existing);
            return -1;
        }

        if (fprintf(f, "%s *(rw,sync,no_subtree_check,fsid=%s,crossmnt)\n", mountpath, fsid) < 0) {
            log_write("Failed to write export entry for %s: %s\n", mountpath, strerror(errno));
            fclose(f);
            free(existing);
            return -1;
        }
        if (existing && existing_len > 0)
            fwrite(existing, 1, existing_len, f);
        fclose(f);
        free(existing);
        log_write("Added export entry for %s with fsid=%s (prepended)\n", mountpath, fsid);
    } else {
        log_write("Export entry already exists for %s\n", mountpath);
    }

    return run_exportfs_refresh();
}

int setup_overlay_dirs(struct mount_ctx *ctx, const char *sandboxname) {
    // Extract basename from sandboxname
    char sandboxname_copy[2048];
    strncpy(sandboxname_copy, sandboxname, sizeof(sandboxname_copy) - 1);
    sandboxname_copy[sizeof(sandboxname_copy) - 1] = '\0';
    char *sb_basename = basename(sandboxname_copy);
    
    // Construct upperdir and workdir from overlayroot and sandbox basename
    int ret1 = snprintf(ctx->upperdir, sizeof(ctx->upperdir), 
                        "%s/%s/upper", ctx->overlayroot, sb_basename);
    int ret2 = snprintf(ctx->workdir, sizeof(ctx->workdir), 
                        "%s/%s/work", ctx->overlayroot, sb_basename);
    
    if (ret1 >= (int)sizeof(ctx->upperdir) 
        || ret2 >= (int)sizeof(ctx->workdir)) {
        fprintf(stderr, "Error: overlay directory path too long\n");
        return -1;
    }
    
    return 0;
}


/*
 * Parse a colon-separated custom lowerdir string into components.
 * Tokens starting with '/' are local paths; otherwise the token is an
 * NFS hostname and the next token (after ':') is the NFS export path.
 *
 * For each NFS component, mount it under
 *   <overlayroot>/<sandbox_basename>-clower-<N>
 *
 * Returns the resolved overlay-ready lowerdir string in resolved_lowerdir.
 * On error, any NFS mounts already made are cleaned up.
 */
static int handle_custom_lowerdir(const char *custom_lowerdir,
                                   struct mount_ctx *ctx,
                                   const char *sandboxname,
                                   char *resolved_lowerdir,
                                   size_t resolved_size,
                                   int *nfs_mounted_count)
{
    char tmp[2048];
    strncpy(tmp, custom_lowerdir, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    char sb_copy[2048];
    strncpy(sb_copy, sandboxname, sizeof(sb_copy) - 1);
    sb_copy[sizeof(sb_copy) - 1] = '\0';
    char *sb_base = basename(sb_copy);

    int roff = 0;
    resolved_lowerdir[0] = '\0';
    int nfs_idx = 0;
    int first = 1;

    char *saveptr = NULL;
    char *token = strtok_r(tmp, ":", &saveptr);

    while (token) {
        if (token[0] == '/') {
            /* Local absolute path – validate accessibility */
            struct stat lst;
            if (stat(token, &lst) < 0) {
                log_write("Error: custom lowerdir '%s' is not accessible: %s\n",
                          token, strerror(errno));
                goto err_cleanup;
            }
            if (!first && roff < (int)resolved_size)
                resolved_lowerdir[roff++] = ':';
            int n = snprintf(resolved_lowerdir + roff,
                             resolved_size - (size_t)roff, "%s", token);
            if (n < 0 || roff + n >= (int)resolved_size) {
                log_write("Error: combined custom lowerdir path too long\n");
                errno = ENAMETOOLONG;
                goto err_cleanup;
            }
            roff += n;
            first = 0;
        } else {
            /* NFS hostname – consume next token as NFS path */
            char *nfs_path = strtok_r(NULL, ":", &saveptr);
            if (!nfs_path || nfs_path[0] != '/') {
                log_write("Error: invalid NFS source '%s' in custom lowerdir "
                          "(expected host:/path)\n", token);
                errno = EINVAL;
                goto err_cleanup;
            }

            char nfs_source[2048];
            snprintf(nfs_source, sizeof(nfs_source), "%s:%s", token, nfs_path);

            /* Build a unique mount target for this NFS source */
            char mount_target[2048];
            int tn = snprintf(mount_target, sizeof(mount_target),
                              "%s/%s-clower-%d", ctx->overlayroot, sb_base, nfs_idx);
            if (tn >= (int)sizeof(mount_target)) {
                log_write("Error: custom NFS mount target path too long\n");
                errno = ENAMETOOLONG;
                goto err_cleanup;
            }

            if (mkdir_p(mount_target, 0755) < 0) {
                log_write("Failed to create custom NFS target %s: %s\n",
                          mount_target, strerror(errno));
                goto err_cleanup;
            }

            if (!mountinfo_is_mounted(mount_target)) {
                char hostname[256] = {0};
                size_t hlen = strlen(token);
                if (hlen >= sizeof(hostname)) {
                    log_write("Error: hostname too long in custom lowerdir '%s'\n",
                              nfs_source);
                    errno = EINVAL;
                    goto err_cleanup;
                }
                strncpy(hostname, token, sizeof(hostname) - 1);

                struct hostent *he = gethostbyname(hostname);
                if (!he) {
                    log_write("Failed to resolve hostname '%s': %s\n",
                              hostname, hstrerror(h_errno));
                    errno = EIO;
                    goto err_cleanup;
                }
                char *ip = inet_ntoa(*(struct in_addr *)he->h_addr);
                char options[8192];
                snprintf(options, sizeof(options),
                         "addr=%s,vers=4.2,actimeo=720000,nocto,"
                         "lookupcache=all,nconnect=4", ip);

                log_write("Mounting custom NFS '%s' at '%s'\n",
                          nfs_source, mount_target);
                if (mount(nfs_source, mount_target, "nfs",
                          MS_RDONLY, options) < 0) {
                    log_write("Failed to mount custom NFS '%s' to '%s': %s\n",
                              nfs_source, mount_target, strerror(errno));
                    goto err_cleanup;
                }
                log_write("Custom NFS lowerdir mounted at %s\n", mount_target);
            }

            nfs_idx++;

            /* Add mount target (not NFS source) to the lowerdir list */
            if (!first && roff < (int)resolved_size)
                resolved_lowerdir[roff++] = ':';
            int n = snprintf(resolved_lowerdir + roff,
                             resolved_size - (size_t)roff, "%s", mount_target);
            if (n < 0 || roff + n >= (int)resolved_size) {
                log_write("Error: combined custom lowerdir path too long\n");
                errno = ENAMETOOLONG;
                goto err_cleanup;
            }
            roff += n;
            first = 0;
        }
        token = strtok_r(NULL, ":", &saveptr);
    }

    if (roff == 0) {
        log_write("Error: custom lowerdir resolves to empty\n");
        errno = EINVAL;
        goto err_cleanup;
    }

    *nfs_mounted_count = nfs_idx;
    return 0;

err_cleanup:
    /* Undo any NFS mounts we made so far */
    for (int i = 0; i < nfs_idx; i++) {
        char target[4096];
        snprintf(target, sizeof(target), "%s/%s-clower-%d",
                 ctx->overlayroot, sb_base, i);
        if (mountinfo_is_mounted(target)) {
            umount2(target, MNT_DETACH);
            log_write("Cleaned up custom NFS mount at %s\n", target);
        }
    }
    *nfs_mounted_count = 0;
    return -1;
}

/*
 * Unmount any custom NFS lowerdirs created during mount for the given
 * sandbox.  Uses the naming convention <overlayroot>/<sb_basename>-clower-N.
 */

/*
 * Write a JSON record of the successful mount under
 *   <overlayroot>/<sandbox_basename>/mountinfo.json
 *
 * This file contains all details needed to recreate the mount and is
 * automatically cleaned up when the sandbox is destroyed (the parent
 * directory is removed during umount_sandbox cleanup).
 */
static void json_escape(FILE *f, const char *s)
{
    for (; *s; s++) {
        switch (*s) {
        case '"':  fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f);  break;
        case '\r': fputs("\\r", f);  break;
        case '\t': fputs("\\t", f);  break;
        default:   fputc(*s, f);     break;
        }
    }
}

static void json_kv_str(FILE *f, const char *key, const char *val, int comma)
{
    fprintf(f, "  \"%s\": \"", key);
    json_escape(f, val);
    fprintf(f, "\"%s\n", comma ? "," : "");
}

static void save_mount_record(const struct mount_ctx *ctx,
                               const char *sandboxname,
                               const char *effective_lowerdir,
                               const char *custom_lowerdir,
                               const char *custom_overlayroot,
                               uid_t uid, gid_t gid, int flags,
                               const char *baas_path,
                               const char *prebuild_sb)
{
    char sb_copy[2048];
    snprintf(sb_copy, sizeof(sb_copy), "%s", sandboxname);
    char *sb_base = basename(sb_copy);

    char record_path[4096];
    int rn = snprintf(record_path, sizeof(record_path),
                      "%s/%s/mountinfo.json", ctx->overlayroot, sb_base);
    if (rn <= 0 || rn >= (int)sizeof(record_path)) {
        log_write("Warning: mount record path too long, skipping save\n");
        return;
    }

    FILE *f = fopen(record_path, "w");
    if (!f) {
        log_write("Warning: failed to create mount record '%s': %s\n",
                  record_path, strerror(errno));
        return;
    }

    time_t now = time(NULL);
    struct tm tm_buf;
    struct tm *tm = localtime_r(&now, &tm_buf);
    char timebuf[64];
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%dT%H:%M:%S", tm);

    fprintf(f, "{\n");
    json_kv_str(f, "sandboxname", sandboxname, 1);
    json_kv_str(f, "overlayroot", ctx->overlayroot, 1);
    json_kv_str(f, "lowerdir", effective_lowerdir, 1);
    json_kv_str(f, "upperdir", ctx->upperdir, 1);
    json_kv_str(f, "workdir", ctx->workdir, 1);
    if (custom_lowerdir && custom_lowerdir[0])
        json_kv_str(f, "custom_lowerdir", custom_lowerdir, 1);
    if (custom_overlayroot && custom_overlayroot[0])
        json_kv_str(f, "custom_overlayroot", custom_overlayroot, 1);
    if (baas_path && baas_path[0])
        json_kv_str(f, "baas_path", baas_path, 1);
    if (prebuild_sb && prebuild_sb[0])
        json_kv_str(f, "prebuild_sb", prebuild_sb, 1);
    fprintf(f, "  \"uid\": %d,\n", (int)uid);
    fprintf(f, "  \"gid\": %d,\n", (int)gid);
    fprintf(f, "  \"flags\": %d,\n", flags);
    json_kv_str(f, "created", timebuf, 0);
    fprintf(f, "}\n");

    fclose(f);
    log_write("Saved mount record to '%s'\n", record_path);
}

int mount_sandbox(const char *sandboxname, uid_t uid, gid_t gid, int flags, const char *custom_lowerdir, const char *custom_overlayroot, const char *baas_path, const char *prebuild_sb) {
    log_write("Mounting sandbox at '%s' for UID %d\n", sandboxname, uid);

    struct mount_ctx ctx = {0};
    const struct daemon_config *dcfg = config_get();

    /* Determine overlayroot: prefer client-supplied, fall back to daemon config */
    if (custom_overlayroot && custom_overlayroot[0]) {
        log_write("Using client-supplied overlayroot '%s'\n", custom_overlayroot);
        strncpy(ctx.overlayroot, custom_overlayroot, sizeof(ctx.overlayroot) - 1);
        ctx.overlayroot[sizeof(ctx.overlayroot) - 1] = '\0';
    } else if (dcfg->overlayroot[0]) {
        strncpy(ctx.overlayroot, dcfg->overlayroot, sizeof(ctx.overlayroot) - 1);
        ctx.overlayroot[sizeof(ctx.overlayroot) - 1] = '\0';
    }

    if (!ctx.overlayroot[0]) {
        log_write("Error: overlayroot not set (configure in %s or use --overlayroot)\n", MREPOD_CONF_PATH);
        return -EINVAL;
    }

    if (!custom_lowerdir || !custom_lowerdir[0]) {
        log_write("Error: --lowerdir is required\n");
        return -EINVAL;
    }

    if (mountinfo_is_mounted(sandboxname)) return 0;   /* already mounted */
    // create mount point as user might not have created it yet
    mkdir_p(sandboxname, 0775);
    // sandboxname creation fails return
    if (access(sandboxname, F_OK) < 0) {
        log_write("Failed to create mount point %s: %s\n", sandboxname, strerror(errno));
        return -errno;
    }
    // chown to uid and gid so that user can access it after mount (ignore error - might not have permission or dir already exists with correct ownership)
    if (chown(sandboxname, uid, gid) < 0) {
        log_write("Warning: Failed to chown %s to %d:%d: %s\n", sandboxname, uid, gid, strerror(errno));
    }
    
    lock_sandbox(sandboxname);
    enter_namespace();
    
    // Setup overlay directory paths based on overlayroot and sandbox name
    if (setup_overlay_dirs(&ctx, sandboxname) < 0) {
        exit_namespace();
        unlock_sandbox(sandboxname);
        return -errno;
    }

    int custom_nfs_count = 0;
    char effective_lowerdir[8192];

    if (prebuild_sb && prebuild_sb[0]) {
        /* For pbsb-backed sandboxes, lowerdir is already a mounted local path. */
        struct stat lst;
        if (custom_lowerdir[0] != '/') {
            log_write("Error: invalid pbsb lowerdir '%s' (expected absolute path)\n",
                      custom_lowerdir);
            errno = EINVAL;
            goto err;
        }
        if (stat(custom_lowerdir, &lst) < 0) {
            log_write("Error: pbsb lowerdir '%s' is not accessible: %s\n",
                      custom_lowerdir, strerror(errno));
            goto err;
        }
        snprintf(effective_lowerdir, sizeof(effective_lowerdir), "%s", custom_lowerdir);
        log_write("Using pbsb lowerdir '%s' (skip custom lowerdir parsing)\n",
                  effective_lowerdir);
    } else {
        /* Parse lowerdir, mount NFS components, validate local paths */
        log_write("Processing lowerdir '%s'\n", custom_lowerdir);
        if (handle_custom_lowerdir(custom_lowerdir, &ctx, sandboxname,
                                    effective_lowerdir, sizeof(effective_lowerdir),
                                    &custom_nfs_count) < 0) {
            goto err;
        }
    }
    
    if (fault("FAIL_OVERLAY_MOUNT")) goto err;
    
    // create dirs if they don't exist
    if (mkdir_p(ctx.upperdir, 0775) < 0) {
        log_write("Failed to create upperdir %s: %s\n", ctx.upperdir, strerror(errno));
    }
    if (mkdir_p(ctx.workdir, 0775) < 0) {
        log_write("Failed to create workdir %s: %s\n", ctx.workdir, strerror(errno));
    }
    
    // Check that upperdir and workdir are on the same filesystem
    struct stat st_upper, st_work;
    if (stat(ctx.upperdir, &st_upper) == 0 && stat(ctx.workdir, &st_work) == 0) {
        if (st_upper.st_dev != st_work.st_dev) {
            log_write("Error: upperdir and workdir must be on the same filesystem\n");
            log_write("  upperdir device: %lu, workdir device: %lu\n", 
                   (unsigned long)st_upper.st_dev, (unsigned long)st_work.st_dev);
            goto err;
        }
    }

    // Reject upperdir/workdir residing on an overlayfs – nested overlays are unsupported
    struct statfs sfs_upper, sfs_work;
    if (statfs(ctx.upperdir, &sfs_upper) == 0 && sfs_upper.f_type == OVERLAYFS_SUPER_MAGIC) {
        log_write("Error: upperdir %s is on an overlayfs filesystem; "
                  "check the overlayroot setting\n", ctx.upperdir);
        errno = ENOTSUP;
        goto err;
    }
    if (statfs(ctx.workdir, &sfs_work) == 0 && sfs_work.f_type == OVERLAYFS_SUPER_MAGIC) {
        log_write("Error: workdir %s is on an overlayfs filesystem; "
                  "check the overlayroot setting\n", ctx.workdir);
        errno = ENOTSUP;
        goto err;
    }

    if (dcfg->post_mount_whiteout_dir_count > 0) {
        int wrc = start_prepare_whiteout_async(effective_lowerdir, ctx.upperdir, dcfg);
        if (wrc < 0) {
            log_write("Warning: failed to start async whiteout preparation for '%s': %s\n",
                      sandboxname, strerror(-wrc));
        }
    }
    
    // Build overlay mount options
    char opts[8192];
    int opts_len = snprintf(opts, sizeof(opts),
        "lowerdir=%s,upperdir=%s,workdir=%s,redirect_dir=on,index=on,nfs_export=on",
        effective_lowerdir, ctx.upperdir, ctx.workdir);

    if ((flags & FLAG_OVERRIDE_UID) && opts_len > 0 && opts_len < (int)sizeof(opts)) {
        int n = snprintf(opts + opts_len, sizeof(opts) - (size_t)opts_len,
                         ",override_creds=%d:%d", (int)uid, (int)gid);
        if (n > 0)
            opts_len += n;
        log_write("Override UID enabled: overlay will use uid=%d gid=%d\n", (int)uid, (int)gid);
    }
    
    if (opts_len >= (int)sizeof(opts)) {
        log_write("Error: overlay mount options too long opts=%s\n", opts);
        goto err;
    }
    
    log_write("Attempting overlay mount at %s, opts=%s\n", sandboxname, opts);
    
    if (mount("overlay", sandboxname, "overlay", 0, opts) < 0) {
        log_write("Failed to mount overlay at '%s': %s (errno=%d)\n", sandboxname, strerror(errno), errno);
        log_write("  lowerdir: %s\n", effective_lowerdir);
        log_write("  upperdir: %s\n", ctx.upperdir);
        log_write("  workdir: %s\n", ctx.workdir);
        log_write("  mountpoint: %s\n", sandboxname);
        log_write("Hint: Verify overlayfs kernel support with: cat /proc/filesystems | grep overlay\n");
        goto err;
    }
    
    log_write("Successfully mounted sandbox at %s\n", sandboxname);
    mountinfo_track(sandboxname, flags, ctx.overlayroot, baas_path, prebuild_sb);

    if (ensure_overlay_export(sandboxname) < 0) {
        log_write("Failed to update %s for mountpoint %s\n", EXPORTS_FILE, sandboxname);
        mountinfo_untrack(sandboxname);
        umount2(sandboxname, MNT_DETACH);
        goto err;
    }
    
    if (chown(sandboxname, uid, gid) < 0) {
        /* Ignore error - might not have permission or dir already exists with correct ownership */
        log_write("Warning: Failed to set ownership of %s to %d:%d\n", sandboxname, uid, gid);
    }

    if (dcfg->post_mount_chown_dir_count > 0) {
        int crc = start_post_mount_chown_async(sandboxname, uid, gid, dcfg);
        if (crc < 0) {
            log_write("Warning: failed to start post-mount chown worker for '%s': %s\n",
                      sandboxname, strerror(-crc));
        }
    }

    save_mount_record(&ctx, sandboxname, effective_lowerdir,
                      custom_lowerdir, custom_overlayroot, uid, gid, flags,
                      baas_path, prebuild_sb);

    exit_namespace();
    unlock_sandbox(sandboxname);
    
    return 0;

err:
    if (custom_nfs_count > 0)
        cleanup_custom_nfs_mounts(ctx.overlayroot, sandboxname);
    exit_namespace();
    unlock_sandbox(sandboxname);
    return -errno;
}


int restore_mounts(char *details, size_t details_size)
{
    const struct daemon_config *dcfg = config_get();
    if (!dcfg->overlayroot[0]) {
        log_write("Recover: no overlayroot configured in %s\n", MREPOD_CONF_PATH);
        snprintf(details, details_size,
                 "No overlayroot configured in %s\n", MREPOD_CONF_PATH);
        return -EINVAL;
    }

    const char *overlayroot = dcfg->overlayroot;
    log_write("Recover: scanning '%s' for mountinfo.json records\n", overlayroot);

    DIR *dir = opendir(overlayroot);
    if (!dir) {
        log_write("Recover: failed to open overlayroot '%s': %s\n",
                  overlayroot, strerror(errno));
        snprintf(details, details_size,
                 "Failed to open overlayroot '%s': %s\n",
                 overlayroot, strerror(errno));
        return -errno;
    }

    int restored = 0;
    int failed = 0;
    int skipped = 0;
    size_t off = 0;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;

        char record_path[4096];
        int rn = snprintf(record_path, sizeof(record_path),
                          "%s/%s/mountinfo.json", overlayroot, ent->d_name);
        if (rn <= 0 || rn >= (int)sizeof(record_path))
            continue;

        if (access(record_path, R_OK) != 0)
            continue;

        json_error_t jerr;
        json_t *root = json_load_file(record_path, 0, &jerr);
        if (!root) {
            log_write("Recover: failed to parse '%s': %s\n",
                      record_path, jerr.text);
            failed++;
            continue;
        }

        const char *sandboxname = json_string_value(json_object_get(root, "sandboxname"));
        const char *custom_lowerdir = json_string_value(json_object_get(root, "lowerdir"));
        const char *custom_overlayroot = json_string_value(json_object_get(root, "custom_overlayroot"));
        const char *rec_baas_path = json_string_value(json_object_get(root, "baas_path"));
        const char *rec_prebuild_sb = json_string_value(json_object_get(root, "prebuild_sb"));
        int flags = (int)json_integer_value(json_object_get(root, "flags"));
        int uid = (int)json_integer_value(json_object_get(root, "uid"));
        int gid = (int)json_integer_value(json_object_get(root, "gid"));

        if (!sandboxname || !custom_lowerdir) {
            log_write("Recover: missing required fields in '%s'\n", record_path);
            json_decref(root);
            failed++;
            continue;
        }

        /* Skip if already mounted */
        if (mountinfo_is_mounted(sandboxname)) {
            const char *effective_overlayroot =
                (custom_overlayroot && custom_overlayroot[0]) ? custom_overlayroot : overlayroot;
            if (mountinfo_track(sandboxname, flags, effective_overlayroot,
                                rec_baas_path ? rec_baas_path : "",
                                rec_prebuild_sb ? rec_prebuild_sb : "") != 0) {
                log_write("Recover: '%s' already mounted; failed to re-track\n", sandboxname);
            } else {
                log_write("Recover: '%s' already mounted; re-tracked\n", sandboxname);
            }
            int n = snprintf(details + off, details_size - off,
                             "  skip: %s (already mounted)\n", sandboxname);
            if (n > 0) off += (size_t)n;
            json_decref(root);
            skipped++;
            continue;
        }

        log_write("Recover: restoring sandbox '%s'\n", sandboxname);

        int rc = mount_sandbox(sandboxname,
                              (uid_t)uid, (gid_t)gid, flags,
                              custom_lowerdir ? custom_lowerdir : "",
                              custom_overlayroot ? custom_overlayroot : "",
                              rec_baas_path ? rec_baas_path : "",
                              rec_prebuild_sb ? rec_prebuild_sb : "");

        if (rc == 0) {
            restored++;
            int n = snprintf(details + off, details_size - off,
                             "  ok: %s\n", sandboxname);
            if (n > 0) off += (size_t)n;
        } else {
            failed++;
            int n = snprintf(details + off, details_size - off,
                             "  FAIL: %s: %s\n",
                             sandboxname, strerror(-rc));
            if (n > 0) off += (size_t)n;
        }

        json_decref(root);
    }

    closedir(dir);

    int n = snprintf(details + off, details_size - off,
                     "Recover complete: %d restored, %d failed, %d skipped\n",
                     restored, failed, skipped);
    if (n > 0) off += (size_t)n;

    log_write("Recover complete: %d restored, %d failed, %d skipped\n",
              restored, failed, skipped);
    return (failed > 0) ? -EIO : 0;
}

