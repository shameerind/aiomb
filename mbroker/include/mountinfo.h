#pragma once

#define MOUNTINFO_MAX_TRACKED 256
int mountinfo_is_mounted(const char *path);

/* Active mount tracking for daemon cleanup */
int  mountinfo_track(const char *mountpoint, int flags, const char *overlayroot,
                     const char *baas_path, const char *prebuild_sb);
int  mountinfo_flags_for(const char *mountpoint);
const char *mountinfo_overlayroot_for(const char *mountpoint);
int  mountinfo_baas_info_for(const char *mountpoint, char *baas_path, size_t bp_len,
                             char *prebuild_sb, size_t ps_len);
int  mountinfo_snapshot_mountpoints(char mountpoints[][2048], int max_mountpoints);
void mountinfo_untrack(const char *mountpoint);
void mountinfo_cleanup_all(void);
int  mountinfo_tracked_count(void);
int  mountinfo_describe_all(char *buf, size_t buflen);
