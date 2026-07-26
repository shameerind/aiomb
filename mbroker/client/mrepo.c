#include "../include/common.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <grp.h>
#include <pwd.h>
#include <ctype.h>
#include <time.h>
#include "protocol.h"

#define VERSION "1.0"

static void print_usage(const char *progname) {
    printf("Usage: %s <command> [sandboxname] [options]\n\n", progname);
    printf("Commands:\n");
    printf("  create <name> -pbsb <vol>   Create overlay sandbox using prebuild volume\n");
    printf("  destroy <name>              Unmount overlay sandbox\n");
    printf("  list                        List active sandboxes\n");
    printf("  recover                     Restore mounts from saved records\n");
    printf("  version                     Show version\n");
    printf("  help                        Show this help\n\n");
    printf("Options:\n");
    printf("  -pbsb <name>                Prebuild sandbox volume name (required for create)\n");
    printf("  -p, --overlayroot <path>    Override overlayroot for upper/work dirs\n");

    printf("Examples:\n");
    printf("  cd /b/workspace\n");
    printf("  %s create mysb1 -pbsb myvol\n", progname);
    printf("  %s create mysb1 -pbsb myvol -p /b/workspace/.cache\n", progname);
    printf("  %s destroy mysb1\n", progname);
    printf("  %s list\n", progname);
}

static void print_version(void) {
    printf("mrepo version %s\n", VERSION);
    printf("Monorepo sandbox helper client\n");
}

static int is_path_mounted(const char *path) {
    FILE *f = fopen("/proc/mounts", "r");
    if (!f)
        return 0;

    char line[8192];
    while (fgets(line, sizeof(line), f)) {
        char mountpoint[4096] = {0};
        if (sscanf(line, "%*s %4095s %*s %*s %*d %*d", mountpoint) == 1) {
            if (strcmp(mountpoint, path) == 0) {
                fclose(f);
                return 1;
            }
        }
    }

    fclose(f);
    return 0;
}

static void ensure_tmp_log_mode(const char *path)
{
    if (!path || strncmp(path, "/tmp/", 5) != 0)
        return;
    if (chmod(path, 0777) < 0 && errno != ENOENT) {
        fprintf(stderr, "Warning: failed to chmod 0777 on %s: %s\n",
                path, strerror(errno));
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }
    
    // Handle version and help commands
    if (strcmp(argv[1], "version") == 0 || strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0) {
        print_version();
        return 0;
    }
    
    if (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_usage(argv[0]);
        return 0;
    }
    
    int enable_promote = 0;
    int enable_chown_owner = 0;
    int override_uid_explicit = -1;
    int enable_override_uid = 0;
    char *custom_lowerdir = NULL;
    char *custom_overlayroot = NULL;
    char *prebuild_sb = NULL;
    const char *baas_path = "/volume/baas_devops/bin/baas";
    const char *baas_ns   = "_cd-builder";
    int effective_argc = argc;
    while (effective_argc >= 3) {
        const char *tail = argv[effective_argc - 1];
        if (strcmp(tail, "--promote") == 0) {
            enable_promote = 1;
            effective_argc--;
            continue;
        }
        if (strcmp(tail, "--chown-owner") == 0) {
            enable_chown_owner = 1;
            effective_argc--;
            continue;
        }
        if (strcmp(tail, "--override-uid") == 0) {
            override_uid_explicit = 1;
            effective_argc--;
            continue;
        }
        if (strcmp(tail, "--no-override-uid") == 0) {
            override_uid_explicit = 0;
            effective_argc--;
            continue;
        }
        if (strcmp(tail, "--lowerdir") == 0 || strcmp(tail, "-l") == 0) {
            fprintf(stderr, "Error: --lowerdir requires a path argument\n\n");
            print_usage(argv[0]);
            return 1;
        }
        if (strcmp(tail, "--overlayroot") == 0 || strcmp(tail, "-p") == 0) {
            fprintf(stderr, "Error: --overlayroot requires a path argument\n\n");
            print_usage(argv[0]);
            return 1;
        }
        /* Check for --lowerdir / -l <value> pair */
        if (effective_argc >= 4 && (strcmp(argv[effective_argc - 2], "--lowerdir") == 0 || strcmp(argv[effective_argc - 2], "-l") == 0)) {
            custom_lowerdir = argv[effective_argc - 1];
            effective_argc -= 2;
            continue;
        }
        /* Check for --overlayroot / -p <value> pair */
        if (effective_argc >= 4 && (strcmp(argv[effective_argc - 2], "--overlayroot") == 0 || strcmp(argv[effective_argc - 2], "-p") == 0)) {
            custom_overlayroot = argv[effective_argc - 1];
            effective_argc -= 2;
            continue;
        }
        if (strcmp(tail, "-pbsb") == 0) {
            fprintf(stderr, "Error: -pbsb requires a sandbox name argument\n\n");
            print_usage(argv[0]);
            return 1;
        }
        /* Check for -pbsb <value> pair */
        if (effective_argc >= 4 && strcmp(argv[effective_argc - 2], "-pbsb") == 0) {
            prebuild_sb = argv[effective_argc - 1];
            effective_argc -= 2;
            continue;
        }
        if (strcmp(tail, "--baas-path") == 0) {
            fprintf(stderr, "Error: --baas-path requires a path argument\n\n");
            print_usage(argv[0]);
            return 1;
        }
        /* Check for --baas-path <value> pair */
        if (effective_argc >= 4 && strcmp(argv[effective_argc - 2], "--baas-path") == 0) {
            baas_path = argv[effective_argc - 1];
            effective_argc -= 2;
            continue;
        }
        if (strcmp(tail, "--baas-ns") == 0) {
            fprintf(stderr, "Error: --baas-ns requires a namespace argument\n\n");
            print_usage(argv[0]);
            return 1;
        }
        /* Check for --baas-ns <value> pair */
        if (effective_argc >= 4 && strcmp(argv[effective_argc - 2], "--baas-ns") == 0) {
            baas_ns = argv[effective_argc - 1];
            effective_argc -= 2;
            continue;
        }
        break;
    }

    int is_create = strcmp(argv[1], "create") == 0;
    int is_destroy = strcmp(argv[1], "destroy") == 0;
    int is_list = strcmp(argv[1], "list") == 0;
    int is_refresh = strcmp(argv[1], "refresh") == 0;
    int is_recover = strcmp(argv[1], "recover") == 0;

    /* Default override-uid to enabled for create operations. */
    enable_override_uid = is_create ? 1 : 0;
    if (override_uid_explicit != -1)
        enable_override_uid = override_uid_explicit;

    if (!is_create && !is_destroy && !is_list && !is_refresh && !is_recover) {
        fprintf(stderr, "Error: Invalid command '%s'\n\n", argv[1]);
        print_usage(argv[0]);
        return 1;
    }

    if ((is_create || is_destroy) && effective_argc < 3) {
        fprintf(stderr, "Error: Missing required arguments\n\n");
        print_usage(argv[0]);
        return 1;
    }

    if (is_list && effective_argc != 2) {
        fprintf(stderr, "Error: list does not take arguments\n\n");
        print_usage(argv[0]);
        return 1;
    }

    if (is_refresh && effective_argc != 2) {
        fprintf(stderr, "Error: refresh does not take arguments\n\n");
        print_usage(argv[0]);
        return 1;
    }

    if (is_recover && effective_argc != 2) {
        fprintf(stderr, "Error: recover does not take arguments\n\n");
        print_usage(argv[0]);
        return 1;
    }

    if (enable_chown_owner && !is_create) {
        fprintf(stderr, "Error: --chown-owner is supported only with create\n\n");
        print_usage(argv[0]);
        return 1;
    }

    if (override_uid_explicit != -1 && !is_create) {
        fprintf(stderr, "Error: --override-uid/--no-override-uid is supported only with create\n\n");
        print_usage(argv[0]);
        return 1;
    }

    if (custom_lowerdir && !is_create) {
        fprintf(stderr, "Error: --lowerdir is supported only with create\n\n");
        print_usage(argv[0]);
        return 1;
    }

    if (prebuild_sb && custom_lowerdir) {
        fprintf(stderr, "Error: -pbsb and --lowerdir are mutually exclusive\n\n");
        print_usage(argv[0]);
        return 1;
    }

    if (is_create && !custom_lowerdir && !prebuild_sb) {
        fprintf(stderr, "Error: -pbsb is required for create\n\n");
        print_usage(argv[0]);
        return 1;
    }

    if (prebuild_sb && !is_create) {
        fprintf(stderr, "Error: -pbsb is supported only with create\n\n");
        print_usage(argv[0]);
        return 1;
    }

    if (custom_overlayroot && !is_create) {
        fprintf(stderr, "Error: --overlayroot is supported only with create\n\n");
        print_usage(argv[0]);
        return 1;
    }
    
    struct mount_request r = {0};
    struct mount_reply rep = {0};

    if (is_create) {
        r.cmd = CMD_CREATE;
    } else if (is_destroy) {
        r.cmd = CMD_DESTROY;
    } else if (is_list) {
        r.cmd = CMD_SANDBOX_LIST;
    } else if (is_recover) {
        r.cmd = CMD_RECOVER;
    } else {
        r.cmd = CMD_REFRESH;
    }
    if (enable_promote)
        r.flags |= FLAG_PROMOTE_THREADS;
    if (enable_chown_owner)
        r.flags |= FLAG_CHOWN_OWNER_THREAD;
    if (enable_override_uid)
        r.flags |= FLAG_OVERRIDE_UID;

    /* Populate custom overlayroot if provided */
    if (custom_overlayroot) {
        if (custom_overlayroot[0] != '/') {
            fprintf(stderr, "Error: --overlayroot must be an absolute path\n\n");
            print_usage(argv[0]);
            return 1;
        }
        if (strlen(custom_overlayroot) >= sizeof(r.overlayroot)) {
            fprintf(stderr, "Error: --overlayroot path too long (max %zu chars)\n",
                    sizeof(r.overlayroot) - 1);
            return 1;
        }
        snprintf(r.overlayroot, sizeof(r.overlayroot), "%s", custom_overlayroot);
    }
    r.uid = getuid();
    r.gid = getgid();

    /* If --override-uid, prefer the 'build-access' group GID when the user
     * is a member of that group. */
    if (enable_override_uid) {
        struct group *grp = getgrnam("build-access");
        if (grp) {
            /* Check supplementary group list */
            int ngroups = 64;
            gid_t groups[64];
            if (getgrouplist(getenv("USER") ? getenv("USER") : "",
                            r.gid, groups, &ngroups) >= 0) {
                for (int i = 0; i < ngroups; i++) {
                    if (groups[i] == grp->gr_gid) {
                        r.gid = grp->gr_gid;
                        break;
                    }
                }
            }
        }
    }

    // Construct full path from current directory + sandboxname
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        fprintf(stderr, "Failed to get current directory: %s\n", strerror(errno));
        return 1;
    }
    
    if (r.cmd == CMD_CREATE || r.cmd == CMD_DESTROY) {
        // if absolute path is provided for sandboxname, use it directly instead of prepending cwd
        if (argv[2][0] == '/') {
            if (strlen(argv[2]) >= sizeof(r.sandboxname)) {
                fprintf(stderr, "Error: Sandbox path too long (max %zu chars): %s\n",
                        sizeof(r.sandboxname) - 1, argv[2]);
                return 1;
            }
            snprintf(r.sandboxname, sizeof(r.sandboxname), "%s", argv[2]);
        } else {
            // Build full sandbox path and ensure it fits in the buffer
            int len = snprintf(r.sandboxname, sizeof(r.sandboxname), "%s/%s", cwd, argv[2]);
            if (len >= (int)sizeof(r.sandboxname)) {
                fprintf(stderr, "Error: Sandbox path too long (max %zu chars): %s/%s\n",
                        sizeof(r.sandboxname) - 1, cwd, argv[2]);
                return 1;
            }
        }
    }

    /* Mount prebuild sandbox via baas and derive lowerdir from its mount path */
    char pbsb_lowerdir[2048];
    if (prebuild_sb && is_create) {
        /* Validate name: only alphanumeric, hyphen, underscore, dot allowed */
        for (const char *p = prebuild_sb; *p; p++) {
            if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_' && *p != '.') {
                fprintf(stderr, "Error: -pbsb name contains invalid characters\n");
                return 1;
            }
        }
        char pbsb_log[512];
        char log_name_ts[32];
        time_t log_name_now = time(NULL);
        struct tm log_name_tm;
        if (log_name_now == (time_t)-1 || localtime_r(&log_name_now, &log_name_tm) == NULL ||
            strftime(log_name_ts, sizeof(log_name_ts), "%Y%m%d_%H%M%S", &log_name_tm) == 0) {
            snprintf(log_name_ts, sizeof(log_name_ts), "unknown");
        }
        int ln = snprintf(pbsb_log, sizeof(pbsb_log),
                          "/tmp/mrepo_%s_baas_%s.log", prebuild_sb, log_name_ts);
        if (ln >= (int)sizeof(pbsb_log)) {
            fprintf(stderr, "Error: -pbsb name too long for log path\n");
            return 1;
        }
        ensure_tmp_log_mode(pbsb_log);
        int n = snprintf(pbsb_lowerdir, sizeof(pbsb_lowerdir), BAAS_MOUNT_DIR "/%s", prebuild_sb);
        if (n >= (int)sizeof(pbsb_lowerdir)) {
            fprintf(stderr, "Error: pbsb mount path too long\n");
            return 1;
        }

        if (is_path_mounted(pbsb_lowerdir)) {
            printf("Using existing mounted prebuild sandbox at %s\n", pbsb_lowerdir);
        } else {
            FILE *logf;
            time_t now;
            struct tm tm_now;
            char ts[64];
            char cmd[4096];
            char exec_cmd[4608];

            n = snprintf(cmd, sizeof(cmd),
                         "%s createvol -s %s -c %s/%s --skipco --cmdline --skipauth",
                         baas_path, prebuild_sb, baas_ns, prebuild_sb);
            if (n >= (int)sizeof(cmd)) {
                fprintf(stderr, "Error: -pbsb name too long\n");
                return 1;
            }

            now = time(NULL);
            if (now == (time_t)-1 || localtime_r(&now, &tm_now) == NULL ||
                strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_now) == 0) {
                snprintf(ts, sizeof(ts), "unknown-time");
            }
            logf = fopen(pbsb_log, "a");
            if (!logf) {
                fprintf(stderr, "Error: failed to open log file %s: %s\n", pbsb_log, strerror(errno));
                return 1;
            }
            ensure_tmp_log_mode(pbsb_log);
            fprintf(logf, "[%s] CMD: %s\n", ts, cmd);
            fclose(logf);

            n = snprintf(exec_cmd, sizeof(exec_cmd), "%s >> %s 2>&1", cmd, pbsb_log);
            if (n >= (int)sizeof(exec_cmd)) {
                fprintf(stderr, "Error: command too long\n");
                return 1;
            }
            printf("Mounting prebuild sandbox '%s' via baas...\n", prebuild_sb);
            if (system(exec_cmd) != 0) {
                fprintf(stderr,
                        "Error: pbsb clone failed: %s/%s (log: %s)\n",
                        baas_ns, prebuild_sb, pbsb_log);
                return 1;
            }

            n = snprintf(cmd, sizeof(cmd),
                         "%s edit -s %s -bmp --login False",
                         baas_path, prebuild_sb);
            if (n >= (int)sizeof(cmd)) {
                fprintf(stderr, "Error: -pbsb name too long\n");
                return 1;
            }

            now = time(NULL);
            if (now == (time_t)-1 || localtime_r(&now, &tm_now) == NULL ||
                strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_now) == 0) {
                snprintf(ts, sizeof(ts), "unknown-time");
            }
            logf = fopen(pbsb_log, "a");
            if (!logf) {
                fprintf(stderr, "Error: failed to open log file %s: %s\n", pbsb_log, strerror(errno));
                return 1;
            }
            ensure_tmp_log_mode(pbsb_log);
            fprintf(logf, "[%s] CMD: %s\n", ts, cmd);
            fclose(logf);

            n = snprintf(exec_cmd, sizeof(exec_cmd), "%s >> %s 2>&1", cmd, pbsb_log);
            if (n >= (int)sizeof(exec_cmd)) {
                fprintf(stderr, "Error: command too long\n");
                return 1;
            }
            if (system(exec_cmd) != 0) {
                fprintf(stderr,
                        "Error: pbsb mount failed: %s (log: %s)\n", prebuild_sb, pbsb_log);
                return 1;
            }
        }

        if (access(pbsb_lowerdir, F_OK) != 0) {
            fprintf(stderr, "Error: pbsb mount path not accessible: %s (%s)\n",
                    pbsb_lowerdir, strerror(errno));
            return 1;
        }
        custom_lowerdir = pbsb_lowerdir;
    }

    /* Populate custom lowerdir if provided.
     * Supports colon-separated multiple paths:
     *   /local1:/local2:nfshost:/export
     * A token starting with '/' is a local path; otherwise it is an
     * NFS hostname and the following token after ':' is the NFS path.
     * A single relative path (no ':') is resolved against cwd. */
    if (custom_lowerdir) {
        if (strchr(custom_lowerdir, ':') == NULL && custom_lowerdir[0] != '/') {
            /* Single relative local path – resolve against cwd */
            int len = snprintf(r.lowerdir, sizeof(r.lowerdir), "%s/%s", cwd, custom_lowerdir);
            if (len >= (int)sizeof(r.lowerdir)) {
                fprintf(stderr, "Error: --lowerdir path too long (max %zu chars)\n",
                        sizeof(r.lowerdir) - 1);
                return 1;
            }
        } else {
            if (strlen(custom_lowerdir) >= sizeof(r.lowerdir)) {
                fprintf(stderr, "Error: --lowerdir value too long (max %zu chars)\n",
                        sizeof(r.lowerdir) - 1);
                return 1;
            }
            snprintf(r.lowerdir, sizeof(r.lowerdir), "%s", custom_lowerdir);
        }
    }

    /* Populate baas fields for daemon-side operations */
    if (prebuild_sb) {
        snprintf(r.baas_path, sizeof(r.baas_path), "%s", baas_path);
        snprintf(r.prebuild_sb, sizeof(r.prebuild_sb), "%s", prebuild_sb);
    }

    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) {
        fprintf(stderr, "Failed to create socket: %s\n", strerror(errno));
        return 1;
    }
    
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", OVERLAY_SOCKET_FILE);
    
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        fprintf(stderr, "Failed to connect to daemon: %s\n", strerror(errno));
        fprintf(stderr, "Ensure mrepod service is running\n");
        close(s);
        return 1;
    }
    if (write(s, &r, sizeof(r)) < 0) {
        printf("Failed to send request: %s\n", strerror(errno));
        close(s);
        return 1;
    }
    if (read(s, &rep, sizeof(rep)) < 0) {
        printf("Failed to receive reply: %s\n", strerror(errno));
        close(s);
        return 1;
    }
    rep.details[sizeof(rep.details) - 1] = '\0';
    rep.baas_path[sizeof(rep.baas_path) - 1] = '\0';
    rep.prebuild_sb[sizeof(rep.prebuild_sb) - 1] = '\0';

    if (rep.status < 0) {
        if (rep.details[0]) {
            fprintf(stderr, "Error: %s\n", rep.details);
        } else {
            fprintf(stderr, "Error: %s\n", strerror(-rep.status));
        }
        close(s);
        return 1;
    }

    if (r.cmd == CMD_SANDBOX_LIST || r.cmd == CMD_RECOVER) {
        printf("%s", rep.details);
        close(s);
        return 0;
    }

    if (r.cmd == CMD_REFRESH) {
        printf("Success: exportfs refreshed successfully\n");
        close(s);
        return 0;
    }

    if (r.cmd == CMD_DESTROY && rep.prebuild_sb[0]) {
        const char *destroy_baas_path = rep.baas_path[0] ? rep.baas_path : baas_path;
        char destroy_log[512];
        char destroy_log_ts[32];
        time_t destroy_log_now = time(NULL);
        struct tm destroy_log_tm;
        if (destroy_log_now == (time_t)-1 || localtime_r(&destroy_log_now, &destroy_log_tm) == NULL ||
            strftime(destroy_log_ts, sizeof(destroy_log_ts), "%Y%m%d_%H%M%S", &destroy_log_tm) == 0) {
            snprintf(destroy_log_ts, sizeof(destroy_log_ts), "unknown");
        }
        int ln = snprintf(destroy_log, sizeof(destroy_log),
                          "/tmp/mrepo_destroypod_%s_%s.log", rep.prebuild_sb, destroy_log_ts);
        if (ln > 0 && ln < (int)sizeof(destroy_log)) {
            ensure_tmp_log_mode(destroy_log);
            char destroy_cmd[4096];
            int dn = snprintf(destroy_cmd, sizeof(destroy_cmd),
                              "%s destroypod -s %s --skipauth",
                              destroy_baas_path, rep.prebuild_sb);
            if (dn > 0 && dn < (int)sizeof(destroy_cmd)) {
                time_t now = time(NULL);
                struct tm tm_now;
                char ts[64];
                if (now == (time_t)-1 || localtime_r(&now, &tm_now) == NULL ||
                    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_now) == 0) {
                    snprintf(ts, sizeof(ts), "unknown-time");
                }

                FILE *logf = fopen(destroy_log, "a");
                if (logf) {
                    ensure_tmp_log_mode(destroy_log);
                    fprintf(logf, "[%s] CMD: %s\n", ts, destroy_cmd);
                    fclose(logf);
                }

                char exec_cmd[4608];
                int en = snprintf(exec_cmd, sizeof(exec_cmd), "%s >> %s 2>&1",
                                  destroy_cmd, destroy_log);
                if (en > 0 && en < (int)sizeof(exec_cmd)) {
                    int rc = system(exec_cmd);
                    if (rc != 0) {
                        fprintf(stderr,
                                "Warning: baas destroypod failed for %s (log: %s)\n",
                                rep.prebuild_sb, destroy_log);
                    }
                }
            }
        }
    }
    
    printf("Success: sandbox %s successfully at %s\n",
            r.cmd == CMD_CREATE ? "created" : "unmounted", r.sandboxname);

    close(s);
    return 0;
}
