#include "common.h"
#include <sys/mount.h>
#include "mountinfo.h"
#include "protocol.h"
#include "logger.h"
#include <pthread.h>

/*
 * Parse /proc/self/mountinfo properly.
 * Format: id parent_id major:minor root MOUNTPOINT opts ...
 * The 5th field (0-indexed: field[4]) is the mount point.
 */
int mountinfo_is_mounted(const char *path)
{
    FILE *f = fopen("/proc/self/mountinfo", "r");
    if (!f) return 0;

    char line[4096];
    int found = 0;

    while (fgets(line, sizeof(line), f)) {
        /* Field 5 is the mountpoint (0-indexed field 4) */
        char *tok = line;
        char *field = NULL;
        for (int i = 0; i < 5; i++) {
            field = strsep(&tok, " ");
            if (!field) break;
        }
        if (field && strcmp(field, path) == 0) {
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

/* ---- Active mount tracking ---- */

struct tracked_mount {
    char mountpoint[2048];
    char overlayroot[2048];
    char baas_path[512];
    char prebuild_sb[256];
    int  flags;
    int  active;
};

static struct tracked_mount tracked[MOUNTINFO_MAX_TRACKED];
static int tracked_used = 0;
static pthread_mutex_t tracked_lock = PTHREAD_MUTEX_INITIALIZER;

int mountinfo_track(const char *mountpoint, int flags, const char *overlayroot,
                    const char *baas_path, const char *prebuild_sb)
{
    pthread_mutex_lock(&tracked_lock);
    for (int i = 0; i < tracked_used; i++) {
        if (tracked[i].active && strcmp(tracked[i].mountpoint, mountpoint) == 0) {
            /* Refresh metadata for already tracked mountpoint. */
            snprintf(tracked[i].overlayroot, sizeof(tracked[i].overlayroot), "%s", overlayroot ? overlayroot : "");
            snprintf(tracked[i].baas_path, sizeof(tracked[i].baas_path), "%s", baas_path ? baas_path : "");
            snprintf(tracked[i].prebuild_sb, sizeof(tracked[i].prebuild_sb), "%s", prebuild_sb ? prebuild_sb : "");
            tracked[i].flags = flags;
            pthread_mutex_unlock(&tracked_lock);
            return 0;
        }
    }
    for (int i = 0; i < tracked_used; i++) {
        if (!tracked[i].active) {
            /* reuse slot */
            snprintf(tracked[i].mountpoint, sizeof(tracked[i].mountpoint), "%s", mountpoint);
            snprintf(tracked[i].overlayroot, sizeof(tracked[i].overlayroot), "%s", overlayroot ? overlayroot : "");
            snprintf(tracked[i].baas_path, sizeof(tracked[i].baas_path), "%s", baas_path ? baas_path : "");
            snprintf(tracked[i].prebuild_sb, sizeof(tracked[i].prebuild_sb), "%s", prebuild_sb ? prebuild_sb : "");
            tracked[i].flags = flags;
            tracked[i].active = 1;
            pthread_mutex_unlock(&tracked_lock);
            return 0;
        }
    }
    if (tracked_used >= MOUNTINFO_MAX_TRACKED) {
        log_write("Warning: mount tracking table full, cannot track %s\n", mountpoint);
        pthread_mutex_unlock(&tracked_lock);
        return -1;
    }
    struct tracked_mount *t = &tracked[tracked_used++];
    strncpy(t->mountpoint, mountpoint, sizeof(t->mountpoint) - 1);
    t->mountpoint[sizeof(t->mountpoint) - 1] = '\0';
    strncpy(t->overlayroot, overlayroot ? overlayroot : "", sizeof(t->overlayroot) - 1);
    t->overlayroot[sizeof(t->overlayroot) - 1] = '\0';
    strncpy(t->baas_path, baas_path ? baas_path : "", sizeof(t->baas_path) - 1);
    t->baas_path[sizeof(t->baas_path) - 1] = '\0';
    strncpy(t->prebuild_sb, prebuild_sb ? prebuild_sb : "", sizeof(t->prebuild_sb) - 1);
    t->prebuild_sb[sizeof(t->prebuild_sb) - 1] = '\0';
    t->active = 1;
    t->flags = flags;
    pthread_mutex_unlock(&tracked_lock);
    return 0;
}

const char *mountinfo_overlayroot_for(const char *mountpoint)
{
    static __thread char overlayroot_copy[2048];
    pthread_mutex_lock(&tracked_lock);
    for (int i = 0; i < tracked_used; i++) {
        if (tracked[i].active && strcmp(tracked[i].mountpoint, mountpoint) == 0) {
            strncpy(overlayroot_copy, tracked[i].overlayroot, sizeof(overlayroot_copy) - 1);
            overlayroot_copy[sizeof(overlayroot_copy) - 1] = '\0';
            pthread_mutex_unlock(&tracked_lock);
            return overlayroot_copy;
        }
    }
    pthread_mutex_unlock(&tracked_lock);
    return NULL;
}

int mountinfo_baas_info_for(const char *mountpoint, char *baas_path, size_t bp_len,
                            char *prebuild_sb, size_t ps_len)
{
    pthread_mutex_lock(&tracked_lock);
    for (int i = 0; i < tracked_used; i++) {
        if (tracked[i].active && strcmp(tracked[i].mountpoint, mountpoint) == 0) {
            if (tracked[i].prebuild_sb[0]) {
                snprintf(baas_path, bp_len, "%s", tracked[i].baas_path);
                snprintf(prebuild_sb, ps_len, "%s", tracked[i].prebuild_sb);
                pthread_mutex_unlock(&tracked_lock);
                return 1;
            }
            pthread_mutex_unlock(&tracked_lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&tracked_lock);
    return 0;
}

int mountinfo_flags_for(const char *mountpoint)
{
    int flags = 0;
    pthread_mutex_lock(&tracked_lock);
    for (int i = 0; i < tracked_used; i++) {
        if (tracked[i].active && strcmp(tracked[i].mountpoint, mountpoint) == 0) {
            flags = tracked[i].flags;
            break;
        }
    }
    pthread_mutex_unlock(&tracked_lock);
    return flags;
}

int mountinfo_snapshot_mountpoints(char mountpoints[][2048], int max_mountpoints)
{
    if (!mountpoints || max_mountpoints <= 0)
        return 0;

    int count = 0;
    pthread_mutex_lock(&tracked_lock);
    for (int i = 0; i < tracked_used && count < max_mountpoints; i++) {
        if (!tracked[i].active)
            continue;
        memcpy(mountpoints[count], tracked[i].mountpoint, 2048);
        mountpoints[count][2048 - 1] = '\0';
        count++;
    }
    pthread_mutex_unlock(&tracked_lock);
    return count;
}

void mountinfo_untrack(const char *mountpoint)
{
    pthread_mutex_lock(&tracked_lock);
    for (int i = 0; i < tracked_used; i++) {
        if (tracked[i].active && strcmp(tracked[i].mountpoint, mountpoint) == 0) {
            tracked[i].active = 0;
            pthread_mutex_unlock(&tracked_lock);
            return;
        }
    }
    pthread_mutex_unlock(&tracked_lock);
}

int mountinfo_tracked_count(void)
{
    int n = 0;
    pthread_mutex_lock(&tracked_lock);
    for (int i = 0; i < tracked_used; i++)
        if (tracked[i].active) n++;
    pthread_mutex_unlock(&tracked_lock);
    return n;
}

int mountinfo_describe_all(char *buf, size_t buflen)
{
    if (!buf || buflen == 0)
        return -1;

    buf[0] = '\0';
    int off = 0;
    int count = 0;

    pthread_mutex_lock(&tracked_lock);
    for (int i = 0; i < tracked_used; i++) {
        if (!tracked[i].active)
            continue;
        /* Prune entries whose mount is no longer present in /proc */
        if (!mountinfo_is_mounted(tracked[i].mountpoint)) {
            log_write("List: pruning stale tracked entry '%s'\n", tracked[i].mountpoint);
            tracked[i].active = 0;
            continue;
        }
        count++;
        const char *pbsb = tracked[i].prebuild_sb[0] ? tracked[i].prebuild_sb : "-";
        int n = snprintf(buf + off, buflen - (size_t)off,
                 "%s (%s)\n", tracked[i].mountpoint, pbsb);
        if (n > 0 && (size_t)(off + n) < buflen)
            off += n;
    }
    pthread_mutex_unlock(&tracked_lock);

    if (count == 0)
        snprintf(buf, buflen, "No active sandboxes\n");

    return 0;
}

void mountinfo_cleanup_all(void)
{
    struct tracked_mount snapshot[MOUNTINFO_MAX_TRACKED];
    int snapshot_count = 0;

    pthread_mutex_lock(&tracked_lock);
    for (int i = 0; i < tracked_used && snapshot_count < MOUNTINFO_MAX_TRACKED; i++) {
        if (!tracked[i].active)
            continue;
        snapshot[snapshot_count++] = tracked[i];
        tracked[i].active = 0;
    }
    pthread_mutex_unlock(&tracked_lock);

    log_write("Cleaning up %d tracked overlay mount(s)...\n", snapshot_count);
    for (int i = 0; i < snapshot_count; i++) {
        log_write("  cleanup: unmounting %s\n",
                  snapshot[i].mountpoint);
        /* Lazy detach to avoid blocking on busy mounts */
        if (umount2(snapshot[i].mountpoint, MNT_DETACH) < 0) {
            log_write("  cleanup: umount2(%s) failed: %s\n",
                      snapshot[i].mountpoint, strerror(errno));
        }
    }
    log_write("Mount cleanup complete\n");
}
