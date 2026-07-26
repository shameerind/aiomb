#define _GNU_SOURCE
#include "common.h"
#include "logger.h"
#include "promote.h"
#include "task_queue.h"
#include <dirent.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/xattr.h>
#include <stdbool.h>

#define PROMOTE_MAX_FILE_SIZE (off_t)(100L * 1024 * 1024)  /* 100 MB */

struct promote_request {
    char mountpoint[2048];
};

static void set_promote_low_priority(void) {
    if (setpriority(PRIO_PROCESS, 0, 15) < 0)
        log_write("Warning: failed to set promote worker low priority: %s\n", strerror(errno));
}

/*
 * Fast recursive promote using opendir.
 * Sets xattrs on both directories and files in a single pass.
 */
static void promote_tree_opendir(const char *dirpath) {
    struct stat st;
    if (lstat(dirpath, &st) < 0)
        return;

    if (!S_ISDIR(st.st_mode)) {
        /* skip files larger than 100 MB */
        if (st.st_size > PROMOTE_MAX_FILE_SIZE)
            return;
        /* promote single file or symlink */
        if (lsetxattr(dirpath, "user.mrepod.promote_file", "1", 1, 0) < 0) {
            if (errno != EOPNOTSUPP && errno != ENOTSUP && errno != EPERM)
                log_write("File promote xattr failed on %s: %s\n", dirpath, strerror(errno));
        }
        return;
    }

    DIR *dir = opendir(dirpath);
    if (!dir)
        return;

    /* promote the directory itself */
    if (lsetxattr(dirpath, "user.mrepod.promote_dir", "1", 1, 0) < 0) {
        if (errno != EOPNOTSUPP && errno != ENOTSUP && errno != EPERM)
            log_write("Directory promote xattr failed on %s: %s\n", dirpath, strerror(errno));
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.' &&
            (entry->d_name[1] == '\0' ||
             (entry->d_name[1] == '.' && entry->d_name[2] == '\0')))
            continue;

        /* skip .git directory */
        if (strcmp(entry->d_name, ".git") == 0)
            continue;

        char child[4096];
        int n = snprintf(child, sizeof(child), "%s/%s", dirpath, entry->d_name);
        if (n <= 0 || n >= (int)sizeof(child))
            continue;

        bool is_dir = false;
#ifdef _DIRENT_HAVE_D_TYPE
        if (entry->d_type == DT_DIR)
            is_dir = true;
        else if (entry->d_type == DT_UNKNOWN) {
            struct stat cst;
            if (lstat(child, &cst) == 0 && S_ISDIR(cst.st_mode))
                is_dir = true;
        }
#else
        {
            struct stat cst;
            if (lstat(child, &cst) == 0 && S_ISDIR(cst.st_mode))
                is_dir = true;
        }
#endif
        if (is_dir) {
            promote_tree_opendir(child);
        } else {
            /* skip files larger than 100 MB */
            struct stat fst;
            if (lstat(child, &fst) == 0 && fst.st_size > PROMOTE_MAX_FILE_SIZE)
                continue;
            if (lsetxattr(child, "user.mrepod.promote_file", "1", 1, 0) < 0) {
                if (errno != EOPNOTSUPP && errno != ENOTSUP && errno != EPERM)
                    log_write("File promote xattr failed on %s: %s\n", child, strerror(errno));
            }
        }
    }
    closedir(dir);
}

static int enqueue_promote_tasks(struct task_queue *q,
                                 const char *path,
                                 int depth,
                                 int max_depth) {
    struct stat st;
    if (lstat(path, &st) < 0)
        return 0;

    if (!S_ISDIR(st.st_mode) || depth >= max_depth) {
        if (task_queue_push(q, path, 0) < 0)
            return -1;
        return 1;
    }

    DIR *dir = opendir(path);
    if (!dir) {
        if (task_queue_push(q, path, 0) < 0)
            return -1;
        return 1;
    }

    int enqueued = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        /* skip .git directory */
        if (strcmp(entry->d_name, ".git") == 0)
            continue;

        char child[4096];
        int n = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (n <= 0 || n >= (int)sizeof(child))
            continue;

        int rc = enqueue_promote_tasks(q, child, depth + 1, max_depth);
        if (rc < 0) {
            closedir(dir);
            return -1;
        }
        enqueued += rc;
    }
    closedir(dir);

    if (enqueued == 0) {
        if (task_queue_push(q, path, 0) < 0)
            return -1;
        return 1;
    }

    return enqueued;
}

static void *promote_worker_thread(void *arg) {
    struct task_queue *q = (struct task_queue *)arg;
    set_promote_low_priority();

    for (;;) {
        int index = task_queue_next(q);
        if (index < 0)
            break;
        promote_tree_opendir(q->tasks[index].path);
    }
    return NULL;
}

static void *promote_mountpoint_thread(void *arg) {
    struct promote_request *req = (struct promote_request *)arg;
    set_promote_low_priority();

    const char *mountpoint = req->mountpoint;
    log_write("Promote scan starting for %s: online_cpus=%d\n",
              mountpoint, (int)sysconf(_SC_NPROCESSORS_ONLN));

    struct task_queue *queue = calloc(1, sizeof(*queue));
    if (!queue) {
        log_write("Promote task queue alloc failed for %s: %s\n", mountpoint, strerror(errno));
        free(req);
        return NULL;
    }
    task_queue_init(queue);

    DIR *dir = opendir(mountpoint);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;

            /* skip .git directory */
            if (strcmp(entry->d_name, ".git") == 0)
                continue;

            char child[4096];
            int n = snprintf(child, sizeof(child), "%s/%s", mountpoint, entry->d_name);
            if (n <= 0 || n >= (int)sizeof(child))
                continue;

            if (enqueue_promote_tasks(queue, child, 1, TASK_ENQUEUE_LEVELS) < 0)
                break;
        }
        closedir(dir);
    }

    if (queue->total > 0) {
        int cpu_count = (int)sysconf(_SC_NPROCESSORS_ONLN);
        if (cpu_count < 1) cpu_count = 1;
        if (cpu_count > queue->total) cpu_count = queue->total;

        log_write("Promote dispatching: tasks=%d workers=%d for %s\n",
                  queue->total, cpu_count, mountpoint);

        pthread_t *workers = calloc((size_t)cpu_count, sizeof(*workers));
        bool *worker_started = calloc((size_t)cpu_count, sizeof(*worker_started));
        if (!workers || !worker_started) {
            for (int t = 0; t < queue->total; t++)
                promote_tree_opendir(queue->tasks[t].path);
        } else {
            int started = 0;
            for (int w = 0; w < cpu_count; w++) {
                if (pthread_create(&workers[w], NULL, promote_worker_thread, queue) == 0) {
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
                    promote_tree_opendir(queue->tasks[t].path);
            }
        }
        free(workers);
        free(worker_started);
    }

    task_queue_destroy(queue);
    free(queue);

    log_write("Promote scan completed for %s\n", mountpoint);
    free(req);
    return NULL;
}

int daemon_promote_mountpoint_async(const char *mountpoint) {
    if (!mountpoint || !mountpoint[0])
        return -EINVAL;

    struct promote_request *req = calloc(1, sizeof(*req));
    if (!req)
        return -ENOMEM;
    snprintf(req->mountpoint, sizeof(req->mountpoint), "%s", mountpoint);

    pthread_t thread;
    if (pthread_create(&thread, NULL, promote_mountpoint_thread, req) != 0) {
        int saved = errno;
        free(req);
        return -saved;
    }
    pthread_detach(thread);

    log_write("Promotion task started for mountpoint %s\n", mountpoint);
    return 0;
}
