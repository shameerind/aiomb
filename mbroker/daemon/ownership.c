#define _GNU_SOURCE
#include "common.h"
#include "logger.h"
#include "ownership.h"
#include "task_queue.h"
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/resource.h>
#include <stdbool.h>

struct chown_cycle_stats {
    unsigned long files_seen;
    unsigned long dirs_seen;
    unsigned long changed;
    unsigned long errors;
    unsigned long race_skips;
};

struct chown_request {
    char mountpoint[2048];
};

static void set_chown_low_priority(void) {
    if (setpriority(PRIO_PROCESS, 0, 15) < 0)
        log_write("Warning: failed to set chown worker low priority: %s\n", strerror(errno));
}

static int is_expected_chown_race_errno(int err)
{
    return err == ENOENT || err == ENOTDIR || err == ESTALE;
}

/*
 * Fast recursive chown using opendir + fchownat.
 * Avoids redundant stat() calls and full-path resolution overhead
 * that nftw incurs on every entry.  For 3M+ files this is ~2x faster.
 */
static void chown_tree_fchownat(const char *dirpath, uid_t uid,
                                struct chown_cycle_stats *stats) {
    int dfd = open(dirpath, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dfd < 0) {
        if (is_expected_chown_race_errno(errno)) {
            if (stats) __sync_fetch_and_add(&stats->race_skips, 1);
            return;
        }
        if (stats) __sync_fetch_and_add(&stats->errors, 1);
        return;
    }

    /* chown the directory itself */
    if (fchownat(dfd, "", uid, (gid_t)-1,
                 AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW) < 0) {
        if (is_expected_chown_race_errno(errno)) {
            if (stats) __sync_fetch_and_add(&stats->race_skips, 1);
        } else if (errno != EPERM && errno != EROFS && errno != ENOTSUP) {
            if (stats) __sync_fetch_and_add(&stats->errors, 1);
        }
    } else {
        if (stats) __sync_fetch_and_add(&stats->changed, 1);
    }
    if (stats) __sync_fetch_and_add(&stats->dirs_seen, 1);

    DIR *dir = fdopendir(dfd);
    if (!dir) {
        close(dfd);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.' &&
            (entry->d_name[1] == '\0' ||
             (entry->d_name[1] == '.' && entry->d_name[2] == '\0')))
            continue;

        /* chown the entry (file, symlink, or dir) without following symlinks */
        if (fchownat(dirfd(dir), entry->d_name, uid, (gid_t)-1,
                     AT_SYMLINK_NOFOLLOW) < 0) {
            if (is_expected_chown_race_errno(errno)) {
                if (stats) __sync_fetch_and_add(&stats->race_skips, 1);
                continue;
            }
            if (errno != EPERM && errno != EROFS && errno != ENOTSUP) {
                if (stats) __sync_fetch_and_add(&stats->errors, 1);
            }
            continue;
        } else {
            if (stats) __sync_fetch_and_add(&stats->changed, 1);
        }

        /* recurse into sub-directories */
        bool is_dir = false;
#ifdef _DIRENT_HAVE_D_TYPE
        if (entry->d_type == DT_DIR)
            is_dir = true;
        else if (entry->d_type == DT_UNKNOWN) {
            struct stat cst;
            if (fstatat(dirfd(dir), entry->d_name, &cst,
                        AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(cst.st_mode))
                is_dir = true;
        }
#else
        {
            struct stat cst;
            if (fstatat(dirfd(dir), entry->d_name, &cst,
                        AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(cst.st_mode))
                is_dir = true;
        }
#endif
        if (is_dir) {
            char child[4096];
            int n = snprintf(child, sizeof(child), "%s/%s", dirpath, entry->d_name);
            if (n > 0 && n < (int)sizeof(child))
                chown_tree_fchownat(child, uid, stats);
        } else {
            if (stats) __sync_fetch_and_add(&stats->files_seen, 1);
        }
    }
    closedir(dir); /* also closes dfd */
}

static int enqueue_chown_tasks_up_to_depth(struct task_queue *q,
                                           const char *path,
                                           uid_t uid,
                                           int depth,
                                           int max_depth,
                                           const char *mountpoint) {
    struct stat st;
    if (lstat(path, &st) < 0)
        return 0;

    /* chown every directory immediately as we encounter it (levels 0-6) */
    if (S_ISDIR(st.st_mode) && st.st_uid != uid) {
        if (lchown(path, uid, (gid_t)-1) < 0) {
            if (errno != EPERM && errno != EROFS && errno != ENOTSUP &&
                errno != ENOENT && errno != ENOTDIR && errno != ESTALE)
                log_write("Ownership fix failed on dir %s (depth %d): %s\n",
                          path, depth, strerror(errno));
        }
    }

    if (!S_ISDIR(st.st_mode) || depth >= max_depth) {
        if (task_queue_push(q, path, uid) < 0) {
            log_write("Ownership task queue full while processing %s\n", mountpoint);
            return -1;
        }
        return 1;
    }

    DIR *dir = opendir(path);
    if (!dir) {
        if (task_queue_push(q, path, uid) < 0) {
            log_write("Ownership task queue full while processing %s\n", mountpoint);
            return -1;
        }
        return 1;
    }

    int enqueued = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char child[4096];
        int n = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (n <= 0 || n >= (int)sizeof(child))
            continue;

        int rc = enqueue_chown_tasks_up_to_depth(q, child, uid, depth + 1, max_depth, mountpoint);
        if (rc < 0) {
            closedir(dir);
            return -1;
        }
        enqueued += rc;
    }
    closedir(dir);

    if (enqueued == 0) {
        if (task_queue_push(q, path, uid) < 0) {
            log_write("Ownership task queue full while processing %s\n", mountpoint);
            return -1;
        }
        return 1;
    }

    return enqueued;
}

static void *chown_worker_thread(void *arg) {
    struct task_queue *q = (struct task_queue *)arg;
    struct chown_cycle_stats *stats = (struct chown_cycle_stats *)q->stats;
    set_chown_low_priority();

    for (;;) {
        int index = task_queue_next(q);
        if (index < 0)
            break;
        chown_tree_fchownat(q->tasks[index].path, q->tasks[index].uid, stats);
    }
    return NULL;
}

static void *chown_owner_thread(void *arg) {
    struct chown_request *req = (struct chown_request *)arg;
    set_chown_low_priority();

    struct chown_cycle_stats cycle_stats = {0};
    const char *mountpoint = req->mountpoint;

    struct stat st;
    if (stat(mountpoint, &st) < 0) {
        log_write("Ownership-fix skipped for %s: stat failed: %s\n", mountpoint, strerror(errno));
        free(req);
        return NULL;
    }

    if (st.st_uid == 0) {
        log_write("Ownership-fix skipped for %s: mountpoint owner is root\n", mountpoint);
        free(req);
        return NULL;
    }

    /* chown the mountpoint itself */
    if (lchown(mountpoint, st.st_uid, (gid_t)-1) < 0) {
        if (errno != EPERM && errno != EROFS && errno != ENOTSUP)
            log_write("Ownership fix failed on mountpoint %s: %s\n", mountpoint, strerror(errno));
    }

    log_write("Ownership-fix starting for %s: owner uid=%d, online_cpus=%d\n",
              mountpoint, (int)st.st_uid, (int)sysconf(_SC_NPROCESSORS_ONLN));
    struct task_queue *queue = calloc(1, sizeof(*queue));
    if (!queue) {
        log_write("Ownership task queue alloc failed for %s: %s\n", mountpoint, strerror(errno));
        free(req);
        return NULL;
    }
    task_queue_init(queue);
    queue->stats = &cycle_stats;

    DIR *dir = opendir(mountpoint);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;

            char child[4096];
            int n = snprintf(child, sizeof(child), "%s/%s", mountpoint, entry->d_name);
            if (n <= 0 || n >= (int)sizeof(child))
                continue;

            if (enqueue_chown_tasks_up_to_depth(queue, child, st.st_uid,
                                                1, TASK_ENQUEUE_LEVELS, mountpoint) < 0)
                break;
        }
        closedir(dir);
    }

    if (queue->total > 0) {
        int cpu_count = (int)sysconf(_SC_NPROCESSORS_ONLN);
        if (cpu_count < 1)
            cpu_count = 1;
        if (cpu_count > queue->total)
            cpu_count = queue->total;

        log_write("Ownership-fix dispatching: tasks=%d workers=%d for %s\n",
                  queue->total, cpu_count, mountpoint);

        pthread_t *workers = calloc((size_t)cpu_count, sizeof(*workers));
        bool *worker_started = calloc((size_t)cpu_count, sizeof(*worker_started));
        if (!workers || !worker_started) {
            for (int t = 0; t < queue->total; t++)
                chown_tree_fchownat(queue->tasks[t].path, st.st_uid, &cycle_stats);
        } else {
            int started = 0;
            for (int w = 0; w < cpu_count; w++) {
                if (pthread_create(&workers[w], NULL, chown_worker_thread, queue) == 0) {
                    worker_started[w] = true;
                    started++;
                }
            }

            for (int w = 0; w < cpu_count; w++) {
                if (worker_started[w])
                    pthread_join(workers[w], NULL);
            }

            if (started == 0) {
                for (int t = 0; t < queue->total; t++)
                    chown_tree_fchownat(queue->tasks[t].path, st.st_uid, &cycle_stats);
            }
        }

        free(workers);
        free(worker_started);
    }

    task_queue_destroy(queue);
    free(queue);

    log_write("Ownership-fix one-shot: mountpoint=%s dirs_seen=%lu files_seen=%lu changed=%lu errors=%lu race_skips=%lu\n",
              mountpoint,
              cycle_stats.dirs_seen,
              cycle_stats.files_seen,
              cycle_stats.changed,
              cycle_stats.errors,
              cycle_stats.race_skips);

    free(req);
    return NULL;
}

int daemon_chown_owner_mountpoint_async(const char *mountpoint) {
    if (!mountpoint || !mountpoint[0])
        return -EINVAL;

    struct chown_request *req = calloc(1, sizeof(*req));
    if (!req)
        return -ENOMEM;
    snprintf(req->mountpoint, sizeof(req->mountpoint), "%s", mountpoint);

    pthread_t thread;
    if (pthread_create(&thread, NULL, chown_owner_thread, req) != 0) {
        int saved = errno;
        free(req);
        return -saved;
    }
    pthread_detach(thread);

    log_write("Ownership-fix task started for mountpoint %s\n", mountpoint);
    return 0;
}
