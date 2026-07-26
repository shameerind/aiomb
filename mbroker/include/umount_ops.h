#ifndef UMOUNT_OPS_H
#define UMOUNT_OPS_H

#include <sys/types.h>

int umount_sandbox(const char *sandboxname, int flags, uid_t requester_uid);
void cleanup_custom_nfs_mounts(const char *overlayroot, const char *sandboxname);

#endif /* UMOUNT_OPS_H */
