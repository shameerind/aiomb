
#include "common.h"
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "broker.h"
#include "protocol.h"
#include "mount_ops.h"
#include "mountinfo.h"
#include "logger.h"
#include "promote.h"
#include "ownership.h"

static volatile sig_atomic_t broker_stopping;
static int broker_listen_fd = -1;

static const char *cmd_name(int cmd) {
    switch (cmd) {
    case CMD_CREATE:  return "create";
    case CMD_DESTROY: return "destroy";
    case CMD_REFRESH: return "refresh";
    case CMD_SANDBOX_LIST: return "list";
    case CMD_RECOVER: return "recover";
    default:          return "unknown";
    }
}

static void handle_client(int fd) {
    struct mount_request req;
    struct mount_reply rep = {0};

    if (read(fd, &req, sizeof(req)) != sizeof(req)) {
        log_write("Failed to read request: %s\n", strerror(errno));
        return;
    }

    /* Guarantee null-termination of all string fields from untrusted client */
    req.sandboxname[sizeof(req.sandboxname) - 1] = '\0';
    req.lowerdir[sizeof(req.lowerdir) - 1] = '\0';
    req.overlayroot[sizeof(req.overlayroot) - 1] = '\0';
    req.baas_path[sizeof(req.baas_path) - 1] = '\0';
    req.prebuild_sb[sizeof(req.prebuild_sb) - 1] = '\0';

    if (req.flags & FLAG_PROMOTE_THREADS) {
        if (req.cmd != CMD_CREATE || !req.sandboxname[0]) {
            rep.status = -EINVAL;
            snprintf(rep.details, sizeof(rep.details),
                     "--promote is supported only with create and a sandbox path");
            if (write(fd, &rep, sizeof(rep)) < 0)
                log_write("Failed to write reply: %s\n", strerror(errno));
            return;
        }
    }

    if ((req.flags & FLAG_CHOWN_OWNER_THREAD) &&
        ((req.cmd != CMD_CREATE) || !req.sandboxname[0])) {
        rep.status = -EINVAL;
        snprintf(rep.details, sizeof(rep.details),
                 "--chown-owner is supported only with create and a sandbox path");
        if (write(fd, &rep, sizeof(rep)) < 0)
            log_write("Failed to write reply: %s\n", strerror(errno));
        return;
    }

    /* User information available in req.uid and req.gid for permission checks */
    if (req.cmd == CMD_CREATE) {
        log_write("Received create request at '%s' from UID %d\n",
               req.sandboxname, req.uid);
        rep.status = mount_sandbox(req.sandboxname, req.uid, req.gid, req.flags, req.lowerdir, req.overlayroot, req.baas_path, req.prebuild_sb);
        if (rep.status == 0 && (req.flags & FLAG_PROMOTE_THREADS)) {
            int prc = daemon_promote_mountpoint_async(req.sandboxname);
            if (prc < 0) {
                log_write("Warning: failed to start promotion for '%s': %s\n",
                          req.sandboxname, strerror(-prc));
            }
        }
        if (rep.status == 0 && (req.flags & FLAG_CHOWN_OWNER_THREAD)) {
            int crc = daemon_chown_owner_mountpoint_async(req.sandboxname);
            if (crc < 0) {
                rep.status = crc;
                snprintf(rep.details, sizeof(rep.details),
                         "Failed to start ownership-fix task for '%s': %s", req.sandboxname, strerror(-crc));
            }
        }
    } else if (req.cmd == CMD_DESTROY) {
        log_write("Received destroy request at '%s' from UID %d\n", req.sandboxname, req.uid);
        /* Retrieve baas info from tracking before unmount (untrack happens inside) */
        char tracked_baas_path[512] = {0};
        char tracked_prebuild_sb[256] = {0};
        mountinfo_baas_info_for(req.sandboxname, tracked_baas_path, sizeof(tracked_baas_path),
                                tracked_prebuild_sb, sizeof(tracked_prebuild_sb));
        rep.status = umount_sandbox(req.sandboxname, req.flags, req.uid);
        if (rep.status == 0 && tracked_prebuild_sb[0]) {
            snprintf(rep.prebuild_sb, sizeof(rep.prebuild_sb), "%s", tracked_prebuild_sb);
            snprintf(rep.baas_path, sizeof(rep.baas_path), "%s", tracked_baas_path);
        }
    } else if (req.cmd == CMD_REFRESH) {
        log_write("Received refresh request from UID %d\n", req.uid);
        rep.status = refresh_exports();
    } else if (req.cmd == CMD_SANDBOX_LIST) {
        log_write("Received sandbox list request from UID %d\n", req.uid);
        rep.status = mountinfo_describe_all(rep.details, sizeof(rep.details));
    } else if (req.cmd == CMD_RECOVER) {
        log_write("Received recover request from UID %d\n", req.uid);
        rep.status = restore_mounts(rep.details, sizeof(rep.details));
    } else {
        rep.status = -EINVAL;
    }

    if (rep.status < 0 && rep.details[0] == '\0') {
        if (req.cmd == CMD_DESTROY && rep.status == -EPERM) {
            snprintf(rep.details, sizeof(rep.details),
                     "Destroy denied: only mountpoint owner can destroy '%s'",
                     req.sandboxname);
        } else if (req.cmd == CMD_CREATE && rep.status == -ENOTSUP) {
            snprintf(rep.details, sizeof(rep.details),
                     "Upper/work dirs are on an overlayfs; check overlayroot setting");
        } else {
            snprintf(rep.details, sizeof(rep.details),
                     "%s failed at '%s': %s",
                     cmd_name(req.cmd), req.sandboxname,
                     strerror(-rep.status));
        }
    }

    if (write(fd, &rep, sizeof(rep)) < 0) {
        log_write("Failed to write reply: %s\n", strerror(errno));
        return;
    }
}

int broker_run(const char *sockpath) {
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    broker_stopping = 0;
    broker_listen_fd = s;

    strncpy(addr.sun_path, sockpath, sizeof(addr.sun_path)-1);
    unlink(sockpath);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_write("Failed to bind socket: %s\n", strerror(errno));
        broker_listen_fd = -1;
        close(s);
        return 1;
    }
    if (chmod(sockpath, 0666) < 0) {
        log_write("Failed to set socket permissions: %s\n", strerror(errno));
        broker_listen_fd = -1;
        close(s);
        return 1;
    }
    if (listen(s, 16) < 0) {
        log_write("Failed to listen on socket: %s\n", strerror(errno));
        broker_listen_fd = -1;
        close(s);
        return 1;
    }

    for (; !broker_stopping;) {
        int c = accept(s, NULL, NULL);
        if (c >= 0) {
            handle_client(c);
            close(c);
            continue;
        }

        if (broker_stopping)
            break;

        if (errno == EINTR)
            continue;

        log_write("Accept failed: %s\n", strerror(errno));
    }

    if (broker_listen_fd >= 0) {
        close(broker_listen_fd);
        broker_listen_fd = -1;
    }
    unlink(sockpath);
    return 0;
}

void broker_stop(void)
{
    broker_stopping = 1;
    if (broker_listen_fd >= 0) {
        close(broker_listen_fd);
        broker_listen_fd = -1;
    }
}
