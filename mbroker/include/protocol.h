#pragma once
#include <sys/types.h>
#include <stdio.h>
#include <string.h>

#define CMD_CREATE 1
#define CMD_DESTROY 2
#define CMD_REFRESH 4
#define CMD_SANDBOX_LIST 5
#define CMD_RECOVER 6
#define FLAG_LAZY  0x1
#define FLAG_FORCE 0x2
#define FLAG_PROMOTE_THREADS 0x4
#define FLAG_CHOWN_OWNER_THREAD 0x8
#define FLAG_OVERRIDE_UID     0x10
#define FLAG_CUSTOM_LOWERDIR  0x20

#define REPLY_DETAILS_MAX 16384

#define OVERLAY_SOCKET_PATH "/run/mrepod"
#define OVERLAY_SOCKET_FILE "/run/mrepod/socket"
#define BAAS_MOUNT_DIR "/mnt/side-vols"

struct mount_request {
    int cmd;
    int flags;
    uid_t uid;
    gid_t gid;
    char sandboxname[2048];
    char lowerdir[2048];
    char overlayroot[2048];
    char baas_path[512];
    char prebuild_sb[256];
};

struct mount_reply {
    int status;
    char details[REPLY_DETAILS_MAX];
    char baas_path[512];
    char prebuild_sb[256];
};
