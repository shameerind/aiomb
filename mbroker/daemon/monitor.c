#include "common.h"
#include <sys/statvfs.h>
#include <sys/vfs.h>
#include <pthread.h>
#include <signal.h>
#include "monitor.h"
#include "mountinfo.h"
#include "logger.h"
#include "daemon_config.h"
#include "model_client.h"

#define MONITOR_INTERVAL_SEC 600   /* 10 minutes */
#define OVERLAYFS_SUPER_MAGIC 0x794c7630

static pthread_t monitor_tid;
static volatile sig_atomic_t monitor_running;

/*
 * Check a single tracked sandbox mountpoint.
 * Returns 0 if healthy, negative count of issues detected.
 */
static int check_sandbox(const char *mountpoint)
{
    int issues = 0;

    /* 1. Is it still present in /proc/self/mountinfo? */
    if (!mountinfo_is_mounted(mountpoint)) {
        log_write("MONITOR: sandbox '%s' is NO LONGER MOUNTED\n",
                  mountpoint);
        issues++;
        /* No point checking further if not mounted */
        return -issues;
    }

    /* 2. Is the mountpoint accessible (stat)? */
    struct stat st;
    if (stat(mountpoint, &st) < 0) {
        log_write("MONITOR: sandbox '%s' stat FAILED: %s\n",
                  mountpoint, strerror(errno));
        issues++;
    }

    /* 3. Is it really an overlayfs? */
    struct statfs sfs;
    if (statfs(mountpoint, &sfs) == 0) {
        if (sfs.f_type != OVERLAYFS_SUPER_MAGIC) {
            log_write("MONITOR: sandbox '%s' is NOT overlayfs "
                      "(fstype=0x%lx)\n",
                      mountpoint, (unsigned long)sfs.f_type);
            issues++;
        }
    } else {
        log_write("MONITOR: sandbox '%s' statfs FAILED: %s\n",
                  mountpoint, strerror(errno));
        issues++;
    }

    /* 4. Filesystem space check – warn if overlay is >95% full */
    struct statvfs svfs;
    if (statvfs(mountpoint, &svfs) == 0 && svfs.f_blocks > 0) {
        unsigned long used = svfs.f_blocks - svfs.f_bfree;
        unsigned long pct = (used * 100) / svfs.f_blocks;
        if (pct >= 95) {
            log_write("MONITOR: sandbox '%s' filesystem "
                      "%lu%% full\n", mountpoint, pct);
            issues++;
        }
    }

    return issues ? -issues : 0;
}

static void run_health_check(void)
{
    char mountpoints[MOUNTINFO_MAX_TRACKED][2048];
    int count = mountinfo_snapshot_mountpoints(mountpoints,
                                               MOUNTINFO_MAX_TRACKED);
    if (count == 0) {
        log_write("MONITOR: no active sandboxes to check\n");
        return;
    }

    log_write("MONITOR: checking %d active sandbox(es)\n", count);

    int healthy = 0;
    int unhealthy = 0;

    for (int i = 0; i < count; i++) {
        const char *mp = mountpoints[i];

        int rc = check_sandbox(mp);
        if (rc < 0)
            unhealthy++;
        else
            healthy++;
    }

    log_write("MONITOR: check complete - %d healthy, %d unhealthy "
              "(of %d total)\n", healthy, unhealthy, count);

    /*
     * ML Integration: If the model sidecar is available and NFS servers
     * are configured, run anomaly detection on NFS telemetry.
     *
     * NOTE: Real telemetry collection (e.g. parsing /proc/self/mountstats)
     * should replace the placeholder zeros below.
     */
    const struct daemon_config *dcfg = config_get();
    if (dcfg->ml_enabled && dcfg->nfs_server_count > 0) {
        double nfs_metrics[MODEL_MAX_SERVERS * 11];
        memset(nfs_metrics, 0, sizeof(nfs_metrics));

        /* TODO: Populate nfs_metrics from /proc/self/mountstats or
         * a dedicated telemetry collector.  Each server gets 11 floats:
         *   [resp_time, throughput, error_rate, conn_count, read_ops,
         *    write_ops, read_bytes, write_bytes, retransmits,
         *    cache_hit_ratio, queue_depth]
         */

        struct model_anomaly_result anomaly = {0};
        int arc = model_check_anomaly(dcfg->model_socket, nfs_metrics,
                                      dcfg->nfs_server_count, &anomaly);
        if (arc == 0) {
            if (anomaly.any_anomalous) {
                for (int i = 0; i < anomaly.count; i++) {
                    if (anomaly.servers[i].anomalous) {
                        log_write("MONITOR: ML anomaly detected on NFS server %d "
                                  "(error=%.4f threshold=%.4f)\n",
                                  anomaly.servers[i].server_index,
                                  anomaly.servers[i].error,
                                  anomaly.servers[i].threshold);
                    }
                }
            } else {
                log_write("MONITOR: ML anomaly check passed — "
                          "all %d NFS server(s) healthy\n",
                          dcfg->nfs_server_count);
            }
        } else {
            log_write("MONITOR: ML anomaly check unavailable "
                      "(model server not reachable)\n");
        }
    }
}

static void *monitor_thread(void *arg)
{
    (void)arg;
    log_write("MONITOR: health-check thread started "
              "(interval: %ds)\n", MONITOR_INTERVAL_SEC);

    while (monitor_running) {
        /* Sleep in 1-second increments so we can exit promptly */
        for (int i = 0; i < MONITOR_INTERVAL_SEC && monitor_running; i++)
            sleep(1);

        if (!monitor_running)
            break;

        run_health_check();
    }

    log_write("MONITOR: health-check thread exiting\n");
    return NULL;
}

int monitor_start(void)
{
    monitor_running = 1;
    if (pthread_create(&monitor_tid, NULL, monitor_thread, NULL) != 0) {
        log_write("Failed to start monitor thread: %s\n", strerror(errno));
        return -1;
    }
    pthread_detach(monitor_tid);
    return 0;
}

void monitor_stop(void)
{
    monitor_running = 0;
}
