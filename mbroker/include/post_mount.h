#ifndef POST_MOUNT_H
#define POST_MOUNT_H

#include <sys/types.h>
#include "daemon_config.h"

int start_prepare_whiteout_async(const char *effective_lowerdir,
                                 const char *upperdir,
                                 const struct daemon_config *dcfg);

int start_post_mount_chown_async(const char *sandboxname,
                                 uid_t uid,
                                 gid_t gid,
                                 const struct daemon_config *dcfg);

#endif /* POST_MOUNT_H */
