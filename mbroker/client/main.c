#include "cli_util.h"
#include "mount_util.h"
#include "../include/protocol.h"
#include "../include/common.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <grp.h>
#include <pwd.h>
#include <ctype.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>

static int launch_detached_command(const char *cmd)
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;

    if (pid == 0) {
        pid_t grandchild = fork();
        if (grandchild < 0)
            _exit(1);
        if (grandchild > 0)
            _exit(0);

        (void)setsid();
        int chdir_rc = chdir("/");
        if (chdir_rc < 0) {
            /* Best-effort only for detached cleanup command. */
        }
        int cmd_rc = system(cmd);
        if (cmd_rc < 0) {
            /* Command errors are recorded in the redirected log file. */
        }
        _exit(0);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return -1;

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return -1;

    return 0;
}

// Command context structure
struct cmd_context {
    int enable_promote;
    int enable_chown_owner;
    int enable_override_uid;
    char *custom_lowerdir;
    char *custom_overlayroot;
    char *prebuild_sb;
    const char *baas_path;
    const char *baas_ns;
    char sandboxname[2048];
    int is_create;
    int is_destroy;
    int is_list;
    int is_recover;
    int is_refresh;
};

// Parse command line arguments
static int parse_arguments(int argc, char **argv, struct cmd_context *ctx, char *cwd)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->baas_path = "/volume/baas_devops/bin/baas";
    ctx->baas_ns = "_cd-builder";

    if (argc < 2) {
        print_usage(argv[0]);
        return -1;
    }

    // Handle version and help commands
    if (strcmp(argv[1], "version") == 0 || strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0) {
        print_version();
        return -1;
    }

    if (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_usage(argv[0]);
        return -1;
    }

    int effective_argc = argc;
    while (effective_argc >= 3) {
        const char *tail = argv[effective_argc - 1];
        if (strcmp(tail, "--promote") == 0) {
            ctx->enable_promote = 1;
            effective_argc--;
            continue;
        }
        if (strcmp(tail, "--chown-owner") == 0) {
            ctx->enable_chown_owner = 1;
            effective_argc--;
            continue;
        }
        if (strcmp(tail, "--override-uid") == 0) {
            ctx->enable_override_uid = 1;
            effective_argc--;
            continue;
        }
        if (strcmp(tail, "--no-override-uid") == 0) {
            ctx->enable_override_uid = 0;
            effective_argc--;
            continue;
        }
        if (strcmp(tail, "--lowerdir") == 0 || strcmp(tail, "-l") == 0) {
            fprintf(stderr, "Error: --lowerdir requires a path argument\n\n");
            print_usage(argv[0]);
            return -1;
        }
        if (strcmp(tail, "--overlayroot") == 0 || strcmp(tail, "-p") == 0) {
            fprintf(stderr, "Error: --overlayroot requires a path argument\n\n");
            print_usage(argv[0]);
            return -1;
        }
        if (effective_argc >= 4 && (strcmp(argv[effective_argc - 2], "--lowerdir") == 0 || strcmp(argv[effective_argc - 2], "-l") == 0)) {
            ctx->custom_lowerdir = argv[effective_argc - 1];
            effective_argc -= 2;
            continue;
        }
        if (effective_argc >= 4 && (strcmp(argv[effective_argc - 2], "--overlayroot") == 0 || strcmp(argv[effective_argc - 2], "-p") == 0)) {
            ctx->custom_overlayroot = argv[effective_argc - 1];
            effective_argc -= 2;
            continue;
        }
        if (strcmp(tail, "-pbsb") == 0) {
            fprintf(stderr, "Error: -pbsb requires a sandbox name argument\n\n");
            print_usage(argv[0]);
            return -1;
        }
        if (effective_argc >= 4 && strcmp(argv[effective_argc - 2], "-pbsb") == 0) {
            ctx->prebuild_sb = argv[effective_argc - 1];
            effective_argc -= 2;
            continue;
        }
        if (strcmp(tail, "--baas-path") == 0) {
            fprintf(stderr, "Error: --baas-path requires a path argument\n\n");
            print_usage(argv[0]);
            return -1;
        }
        if (effective_argc >= 4 && strcmp(argv[effective_argc - 2], "--baas-path") == 0) {
            ctx->baas_path = argv[effective_argc - 1];
            effective_argc -= 2;
            continue;
        }
        if (strcmp(tail, "--baas-ns") == 0) {
            fprintf(stderr, "Error: --baas-ns requires a namespace argument\n\n");
            print_usage(argv[0]);
            return -1;
        }
        if (effective_argc >= 4 && strcmp(argv[effective_argc - 2], "--baas-ns") == 0) {
            ctx->baas_ns = argv[effective_argc - 1];
            effective_argc -= 2;
            continue;
        }
        break;
    }

    ctx->is_create = strcmp(argv[1], "create") == 0;
    ctx->is_destroy = strcmp(argv[1], "destroy") == 0;
    ctx->is_list = strcmp(argv[1], "list") == 0;
    ctx->is_refresh = strcmp(argv[1], "refresh") == 0;
    ctx->is_recover = strcmp(argv[1], "recover") == 0;

    // Default override-uid to enabled for create operations
    if (ctx->is_create && ctx->enable_override_uid == 0)
        ctx->enable_override_uid = 1;

    if (!ctx->is_create && !ctx->is_destroy && !ctx->is_list && !ctx->is_refresh && !ctx->is_recover) {
        fprintf(stderr, "Error: Invalid command '%s'\n\n", argv[1]);
        print_usage(argv[0]);
        return -1;
    }

    if ((ctx->is_create || ctx->is_destroy) && effective_argc < 3) {
        fprintf(stderr, "Error: Missing required arguments\n\n");
        print_usage(argv[0]);
        return -1;
    }

    if (ctx->is_list && effective_argc != 2) {
        fprintf(stderr, "Error: list does not take arguments\n\n");
        print_usage(argv[0]);
        return -1;
    }

    if (ctx->is_refresh && effective_argc != 2) {
        fprintf(stderr, "Error: refresh does not take arguments\n\n");
        print_usage(argv[0]);
        return -1;
    }

    if (ctx->is_recover && effective_argc != 2) {
        fprintf(stderr, "Error: recover does not take arguments\n\n");
        print_usage(argv[0]);
        return -1;
    }

    if (ctx->enable_chown_owner && !ctx->is_create) {
        fprintf(stderr, "Error: --chown-owner is supported only with create\n\n");
        print_usage(argv[0]);
        return -1;
    }

    if (ctx->custom_lowerdir && !ctx->is_create) {
        fprintf(stderr, "Error: --lowerdir is supported only with create\n\n");
        print_usage(argv[0]);
        return -1;
    }

    if (ctx->prebuild_sb && ctx->custom_lowerdir) {
        fprintf(stderr, "Error: -pbsb and --lowerdir are mutually exclusive\n\n");
        print_usage(argv[0]);
        return -1;
    }

    if (ctx->is_create && !ctx->custom_lowerdir && !ctx->prebuild_sb) {
        fprintf(stderr, "Error: -pbsb is required for create\n\n");
        print_usage(argv[0]);
        return -1;
    }

    if (ctx->prebuild_sb && !ctx->is_create) {
        fprintf(stderr, "Error: -pbsb is supported only with create\n\n");
        print_usage(argv[0]);
        return -1;
    }

    if (ctx->custom_overlayroot && !ctx->is_create) {
        fprintf(stderr, "Error: --overlayroot is supported only with create\n\n");
        print_usage(argv[0]);
        return -1;
    }

    // Construct full sandbox path
    if (ctx->is_create || ctx->is_destroy) {
        if (argv[2][0] == '/') {
            if (strlen(argv[2]) >= sizeof(ctx->sandboxname)) {
                fprintf(stderr, "Error: Sandbox path too long (max %zu chars): %s\n",
                        sizeof(ctx->sandboxname) - 1, argv[2]);
                return -1;
            }
            snprintf(ctx->sandboxname, sizeof(ctx->sandboxname), "%s", argv[2]);
        } else {
            int len = snprintf(ctx->sandboxname, sizeof(ctx->sandboxname), "%s/%s", cwd, argv[2]);
            if (len >= (int)sizeof(ctx->sandboxname)) {
                fprintf(stderr, "Error: Sandbox path too long (max %zu chars): %s/%s\n",
                        sizeof(ctx->sandboxname) - 1, cwd, argv[2]);
                return -1;
            }
        }
    }

    return 0;
}

// Prepare mount request structure
static int prepare_mount_request(struct cmd_context *ctx, struct mount_request *r, char *cwd)
{
    memset(r, 0, sizeof(*r));

    if (ctx->is_create)
        r->cmd = CMD_CREATE;
    else if (ctx->is_destroy)
        r->cmd = CMD_DESTROY;
    else if (ctx->is_list)
        r->cmd = CMD_SANDBOX_LIST;
    else if (ctx->is_recover)
        r->cmd = CMD_RECOVER;
    else
        r->cmd = CMD_REFRESH;

    if (ctx->enable_promote)
        r->flags |= FLAG_PROMOTE_THREADS;
    if (ctx->enable_chown_owner)
        r->flags |= FLAG_CHOWN_OWNER_THREAD;
    if (ctx->enable_override_uid)
        r->flags |= FLAG_OVERRIDE_UID;

    if (ctx->custom_overlayroot) {
        if (ctx->custom_overlayroot[0] != '/') {
            fprintf(stderr, "Error: --overlayroot must be an absolute path\n\n");
            return -1;
        }
        if (strlen(ctx->custom_overlayroot) >= sizeof(r->overlayroot)) {
            fprintf(stderr, "Error: --overlayroot path too long (max %zu chars)\n",
                    sizeof(r->overlayroot) - 1);
            return -1;
        }
        snprintf(r->overlayroot, sizeof(r->overlayroot), "%s", ctx->custom_overlayroot);
    }

    r->uid = getuid();
    r->gid = getgid();

    if (ctx->enable_override_uid) {
        struct group *grp = getgrnam("build-access");
        if (grp) {
            int ngroups = 64;
            gid_t groups[64];
            if (getgrouplist(getenv("USER") ? getenv("USER") : "", r->gid, groups, &ngroups) >= 0) {
                for (int i = 0; i < ngroups; i++) {
                    if (groups[i] == grp->gr_gid) {
                        r->gid = grp->gr_gid;
                        break;
                    }
                }
            }
        }
    }

    snprintf(r->sandboxname, sizeof(r->sandboxname), "%s", ctx->sandboxname);

    if (ctx->custom_lowerdir) {
        if (strchr(ctx->custom_lowerdir, ':') == NULL && ctx->custom_lowerdir[0] != '/') {
            int len = snprintf(r->lowerdir, sizeof(r->lowerdir), "%s/%s", cwd, ctx->custom_lowerdir);
            if (len >= (int)sizeof(r->lowerdir)) {
                fprintf(stderr, "Error: --lowerdir path too long (max %zu chars)\n",
                        sizeof(r->lowerdir) - 1);
                return -1;
            }
        } else {
            if (strlen(ctx->custom_lowerdir) >= sizeof(r->lowerdir)) {
                fprintf(stderr, "Error: --lowerdir value too long (max %zu chars)\n",
                        sizeof(r->lowerdir) - 1);
                return -1;
            }
            snprintf(r->lowerdir, sizeof(r->lowerdir), "%s", ctx->custom_lowerdir);
        }
    }

    if (ctx->prebuild_sb) {
        snprintf(r->baas_path, sizeof(r->baas_path), "%s", ctx->baas_path);
        snprintf(r->prebuild_sb, sizeof(r->prebuild_sb), "%s", ctx->prebuild_sb);
    }

    return 0;
}

// Send request to daemon and receive reply
static int send_request_to_daemon(struct mount_request *r, struct mount_reply *rep)
{
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) {
        fprintf(stderr, "Failed to create socket: %s\n", strerror(errno));
        return -1;
    }

    struct sockaddr_un a = {.sun_family = AF_UNIX};
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", OVERLAY_SOCKET_FILE);

    if (connect(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        fprintf(stderr, "Failed to connect to daemon: %s\n", strerror(errno));
        fprintf(stderr, "Ensure mrepod service is running\n");
        close(s);
        return -1;
    }

    if (write(s, r, sizeof(*r)) < 0) {
        fprintf(stderr, "Failed to send request: %s\n", strerror(errno));
        close(s);
        return -1;
    }

    if (read(s, rep, sizeof(*rep)) < 0) {
        fprintf(stderr, "Failed to receive reply: %s\n", strerror(errno));
        close(s);
        return -1;
    }

    close(s);
    return 0;
}

// Handle create command with baas integration
static int handle_create(struct cmd_context *ctx, char *custom_lowerdir_out)
{
    if (!ctx->prebuild_sb)
        return 0;

    // Validate prebuild sandbox name
    for (const char *p = ctx->prebuild_sb; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_' && *p != '.') {
            fprintf(stderr, "Error: -pbsb name contains invalid characters\n");
            return -1;
        }
    }

    char pbsb_log[512], log_name_ts[32], pbsb_lowerdir[2048];
    time_t log_name_now = time(NULL);
    struct tm log_name_tm;

    if (log_name_now == (time_t)-1 || localtime_r(&log_name_now, &log_name_tm) == NULL ||
        strftime(log_name_ts, sizeof(log_name_ts), "%Y%m%d_%H%M%S", &log_name_tm) == 0) {
        snprintf(log_name_ts, sizeof(log_name_ts), "unknown");
    }

    int ln = snprintf(pbsb_log, sizeof(pbsb_log), "/tmp/mrepo_%s_baas_%s.log", ctx->prebuild_sb, log_name_ts);
    if (ln >= (int)sizeof(pbsb_log)) {
        fprintf(stderr, "Error: -pbsb name too long for log path\n");
        return -1;
    }
    ensure_tmp_log_mode(pbsb_log);

    int n = snprintf(pbsb_lowerdir, sizeof(pbsb_lowerdir), BAAS_MOUNT_DIR "/%s", ctx->prebuild_sb);
    if (n >= (int)sizeof(pbsb_lowerdir)) {
        fprintf(stderr, "Error: pbsb mount path too long\n");
        return -1;
    }

    if (is_path_mounted(pbsb_lowerdir)) {
        printf("Using existing mounted prebuild sandbox at %s\n", pbsb_lowerdir);
    } else {
        FILE *logf;
        time_t now;
        struct tm tm_now;
        char ts[64], cmd[4096], exec_cmd[4608];

        n = snprintf(cmd, sizeof(cmd), "%s createvol -s %s -c %s/%s --skipco --cmdline --skipauth",
                     ctx->baas_path, ctx->prebuild_sb, ctx->baas_ns, ctx->prebuild_sb);
        if (n >= (int)sizeof(cmd)) {
            fprintf(stderr, "Error: -pbsb name too long\n");
            return -1;
        }

        now = time(NULL);
        if (now == (time_t)-1 || localtime_r(&now, &tm_now) == NULL ||
            strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_now) == 0) {
            snprintf(ts, sizeof(ts), "unknown-time");
        }

        logf = fopen(pbsb_log, "a");
        if (!logf) {
            fprintf(stderr, "Error: failed to open log file %s: %s\n", pbsb_log, strerror(errno));
            return -1;
        }
        ensure_tmp_log_mode(pbsb_log);
        fprintf(logf, "[%s] CMD: %s\n", ts, cmd);
        fclose(logf);

        n = snprintf(exec_cmd, sizeof(exec_cmd), "%s >> %s 2>&1", cmd, pbsb_log);
        if (n >= (int)sizeof(exec_cmd)) {
            fprintf(stderr, "Error: command too long\n");
            return -1;
        }

        printf("Mounting prebuild sandbox '%s' via baas...\n", ctx->prebuild_sb);
        if (system(exec_cmd) != 0) {
            fprintf(stderr, "Error: pbsb clone failed: %s/%s (log: %s)\n", ctx->baas_ns, ctx->prebuild_sb, pbsb_log);
            return -1;
        }

        n = snprintf(cmd, sizeof(cmd), "%s edit -s %s -bmp --login False", ctx->baas_path, ctx->prebuild_sb);
        if (n >= (int)sizeof(cmd)) {
            fprintf(stderr, "Error: -pbsb name too long\n");
            return -1;
        }

        now = time(NULL);
        if (now == (time_t)-1 || localtime_r(&now, &tm_now) == NULL ||
            strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_now) == 0) {
            snprintf(ts, sizeof(ts), "unknown-time");
        }

        logf = fopen(pbsb_log, "a");
        if (!logf) {
            fprintf(stderr, "Error: failed to open log file %s: %s\n", pbsb_log, strerror(errno));
            return -1;
        }
        ensure_tmp_log_mode(pbsb_log);
        fprintf(logf, "[%s] CMD: %s\n", ts, cmd);
        fclose(logf);

        n = snprintf(exec_cmd, sizeof(exec_cmd), "%s >> %s 2>&1", cmd, pbsb_log);
        if (n >= (int)sizeof(exec_cmd)) {
            fprintf(stderr, "Error: command too long\n");
            return -1;
        }

        if (system(exec_cmd) != 0) {
            fprintf(stderr, "Error: pbsb mount failed: %s (log: %s)\n", ctx->prebuild_sb, pbsb_log);
            return -1;
        }
    }

    if (access(pbsb_lowerdir, F_OK) != 0) {
        fprintf(stderr, "Error: pbsb mount path not accessible: %s (%s)\n", pbsb_lowerdir, strerror(errno));
        return -1;
    }

    snprintf(custom_lowerdir_out, 2048, "%s", pbsb_lowerdir);
    return 0;
}

// Handle destroy command with baas cleanup
static int handle_destroy(struct mount_reply *rep, const char *baas_path)
{
    if (!rep->prebuild_sb[0])
        return 0;

    const char *destroy_baas_path = rep->baas_path[0] ? rep->baas_path : baas_path;
    char destroy_log[512], destroy_log_ts[32];
    time_t destroy_log_now = time(NULL);
    struct tm destroy_log_tm;

    if (destroy_log_now == (time_t)-1 || localtime_r(&destroy_log_now, &destroy_log_tm) == NULL ||
        strftime(destroy_log_ts, sizeof(destroy_log_ts), "%Y%m%d_%H%M%S", &destroy_log_tm) == 0) {
        snprintf(destroy_log_ts, sizeof(destroy_log_ts), "unknown");
    }

    int ln = snprintf(destroy_log, sizeof(destroy_log), "/tmp/mrepo_destroypod_%s_%s.log", rep->prebuild_sb, destroy_log_ts);
    if (ln <= 0 || ln >= (int)sizeof(destroy_log))
        return 0;

    ensure_tmp_log_mode(destroy_log);

    char destroy_cmd[4096];
    int dn = snprintf(destroy_cmd, sizeof(destroy_cmd), "%s destroypod -s %s --skipauth", destroy_baas_path, rep->prebuild_sb);
    if (dn <= 0 || dn >= (int)sizeof(destroy_cmd))
        return 0;

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
    int en = snprintf(exec_cmd, sizeof(exec_cmd), "%s >> %s 2>&1", destroy_cmd, destroy_log);
    if (en > 0 && en < (int)sizeof(exec_cmd)) {
        if (launch_detached_command(exec_cmd) < 0) {
            fprintf(stderr, "Warning: failed to launch async baas destroypod for %s (log: %s)\n",
                    rep->prebuild_sb, destroy_log);
        }
    }

    return 0;
}

// Handle list/recover commands
static int handle_info_command(struct mount_reply *rep)
{
    printf("%s", rep->details);
    return 0;
}

int main(int argc, char **argv)
{
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        fprintf(stderr, "Failed to get current directory: %s\n", strerror(errno));
        return 1;
    }

    struct cmd_context ctx;
    if (parse_arguments(argc, argv, &ctx, cwd) < 0)
        return 1;

    struct mount_request r = {0};
    if (prepare_mount_request(&ctx, &r, cwd) < 0)
        return 1;

    // Handle create command with baas integration
    char pbsb_lowerdir[2048] = {0};
    if (ctx.is_create && ctx.prebuild_sb) {
        if (handle_create(&ctx, pbsb_lowerdir) < 0)
            return 1;
        if (pbsb_lowerdir[0]) {
            snprintf(r.lowerdir, sizeof(r.lowerdir), "%s", pbsb_lowerdir);
        }
    }

    struct mount_reply rep = {0};
    if (send_request_to_daemon(&r, &rep) < 0) {
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
        return 1;
    }

    // Handle list/recover commands
    if (r.cmd == CMD_SANDBOX_LIST || r.cmd == CMD_RECOVER) {
        handle_info_command(&rep);
        return 0;
    }

    // Handle refresh command
    if (r.cmd == CMD_REFRESH) {
        printf("Success: exportfs refreshed successfully\n");
        return 0;
    }

    // Handle destroy command with baas cleanup
    if (r.cmd == CMD_DESTROY) {
        handle_destroy(&rep, ctx.baas_path);
    }

    printf("Success: sandbox %s successfully at %s\n",
            r.cmd == CMD_CREATE ? "created" : "destroyed", r.sandboxname);

    return 0;
}
