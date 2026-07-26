#define _GNU_SOURCE
#include "common.h"
#include "broker.h"
#include "protocol.h"
#include "util.h"
#include "logger.h"
#include "mountinfo.h"
#include "daemon_config.h"
#include "monitor.h"
#include "model_client.h"
#include <sched.h>
#include <signal.h>
#include <fcntl.h>

static int daemonize(void) {
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "Failed to fork: %s\n", strerror(errno));
        return -1;
    }
    if (pid > 0) {
        // Parent process exits
        exit(0);
    }
    
    // Child continues
    // Create new session and become session leader
    if (setsid() < 0) {
        fprintf(stderr, "Failed to create new session: %s\n", strerror(errno));
        return -1;
    }
    
    // Fork again to ensure we're not session leader (prevents acquiring terminal)
    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "Failed to fork second time: %s\n", strerror(errno));
        return -1;
    }
    if (pid > 0) {
        exit(0);
    }
    
    // Change to root directory
    if (chdir("/") < 0) {
        fprintf(stderr, "Failed to change to root directory: %s\n", strerror(errno));
        return -1;
    }
    
    // Close stdin, stdout, stderr and redirect to /dev/null
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    
    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) close(fd);
    }
    
    return 0;
}

static void cleanup_all(void) {
    monitor_stop();
    // cleanup pid file
    unlink("/var/run/mrepod.pid");
    log_write("mrepod exiting\n");
    log_close();
}

static void signal_handler(int sig) {
    (void)sig;
    broker_stop();
}

int main(int argc, char **argv) {
    int daemonize_flag = 0;
    
    // Check for -d flag
    if (argc > 1 && strcmp(argv[1], "-d") == 0) {
        daemonize_flag = 1;
    }
    
    // Daemonize only if -d flag specified
    if (daemonize_flag) {
        if (daemonize() < 0) {
            return 1;
        }
    }
    
    // Initialize logging (log to stdout in foreground mode, file only in daemon mode)
    if (log_init("/var/log/mrepod.log", !daemonize_flag) < 0) {
        // If log file fails, continue without it
        log_init(NULL, !daemonize_flag);
    }
    
    log_write("Starting mrepod daemon...\n");
    
    // Register cleanup and signal handlers
    atexit(cleanup_all);
    signal(SIGTERM, signal_handler);
    signal(SIGINT,  signal_handler);
    
    // Write PID file
    FILE *pidfp = fopen("/var/run/mrepod.pid", "w");
    if (pidfp) {
        fprintf(pidfp, "%d\n", getpid());
        fclose(pidfp);
    } else {
        log_write("Warning: Failed to write PID file: %s\n", strerror(errno));
    }

    // Load daemon configuration (non-fatal if missing)
    config_load(MREPOD_CONF_PATH);

    // create socket path dir if it doesn't exist
    mkdir_p(OVERLAY_SOCKET_PATH, 0755);

    // Start background health-check monitor thread
    if (monitor_start() < 0) {
        log_write("Warning: failed to start monitor thread\n");
    }

    // Check ML inference sidecar connectivity
    {
        const struct daemon_config *dcfg = config_get();
        if (dcfg->ml_enabled) {
            if (model_health_check(dcfg->model_socket) == 0) {
                log_write("ML inference sidecar is reachable at '%s'\n",
                          dcfg->model_socket);
            } else {
                log_write("Warning: ML inference sidecar not reachable at '%s' — "
                          "ML features will fall back to defaults\n",
                          dcfg->model_socket);
            }
        } else {
            log_write("ML integration disabled (ml_enabled=0)\n");
        }
    }

    return broker_run(OVERLAY_SOCKET_FILE);
}
