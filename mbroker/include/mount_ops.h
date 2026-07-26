//mount operations for the daemon

#define EXPORTS_FILE "/etc/exports"
#define MAX_CUSTOM_LOWERDIR_COMPONENTS 16

struct mount_ctx {
    char overlayroot[2048];
    char lowerdir[2048];
    char upperdir[2048];
    char workdir[2048];
    char nfs_target[2048];
};

int run_exportfs_refresh(void);
int setup_overlay_dirs(struct mount_ctx *ctx, const char *sandboxname);

int mount_sandbox(const char *sandboxname, uid_t uid, gid_t gid, int flags, const char *custom_lowerdir, const char *custom_overlayroot, const char *baas_path, const char *prebuild_sb);
int umount_sandbox(const char *sandboxname, int flags, uid_t requester_uid);
int restore_mounts(char *details, size_t details_size);
int refresh_exports(void);
