#include "common.h"
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <fnmatch.h>
#include <glob.h>
#include "logger.h"
#include "daemon_config.h"
#include "util.h"
#include "post_mount.h"

#define OVERLAY_OPAQUE_XATTR "trusted.overlay.opaque"
#define WHITEOUT_SCAN_SPLIT_DEPTH 2

/* -------------------------------------------------------------------------
 * Recursive chown helper
 * ---------------------------------------------------------------------- */

static int chown_dir_recursive(const char *path, uid_t uid, gid_t gid) {
    struct stat st;
    if (lstat(path, &st) < 0) {
        if (errno == ENOENT)
            return 0;
        log_write("Warning: failed to stat '%s' for recursive chown: %s\n",
                  path, strerror(errno));
        return -1;
    }

    if (lchown(path, uid, gid) < 0 && errno != EPERM && errno != EROFS && errno != ENOTSUP) {
        log_write("Warning: failed to chown '%s' to %d:%d: %s\n",
                  path, (int)uid, (int)gid, strerror(errno));
    }

    if (!S_ISDIR(st.st_mode))
        return 0;

    DIR *dir = opendir(path);
    if (!dir) {
        if (errno != ENOENT) {
            log_write("Warning: failed to open '%s' for recursive chown: %s\n",
                      path, strerror(errno));
        }
        return -1;
    }

    struct dirent *entry;
    int ret = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char child[4096];
        int n = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (n <= 0 || n >= (int)sizeof(child)) {
            ret = -1;
            continue;
        }

        if (chown_dir_recursive(child, uid, gid) < 0)
            ret = -1;
    }

    closedir(dir);
    return ret;
}

static int has_wildcard(const char *s)
{
    return s && strpbrk(s, "*?[") != NULL;
}

/* -------------------------------------------------------------------------
 * Whiteout scan data structures and helpers
 * ---------------------------------------------------------------------- */

struct whiteout_scan_task {
    char lower_root[4096];
    char start_path[4096];
    char rel_path[4096];
    char upperdir[4096];
    const struct daemon_config *dcfg;
    struct whiteout_results *results;
};

struct whiteout_scan_queue {
    struct whiteout_scan_task *tasks;
    int total;
    int capacity;
    int next;
    pthread_mutex_t lock;
};

static void whiteout_scan_queue_init(struct whiteout_scan_queue *q)
{
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->lock, NULL);
}

static void whiteout_scan_queue_destroy(struct whiteout_scan_queue *q)
{
    free(q->tasks);
    q->tasks = NULL;
    q->total = 0;
    q->capacity = 0;
    q->next = 0;
    pthread_mutex_destroy(&q->lock);
}

static int whiteout_scan_queue_push(struct whiteout_scan_queue *q,
                                    const char *lower_root,
                                    const char *start_path,
                                    const char *rel_path,
                                    const char *upperdir,
                                    const struct daemon_config *dcfg,
                                    struct whiteout_results *results)
{
    if (q->total >= q->capacity) {
        int new_cap = q->capacity ? q->capacity * 2 : 256;
        struct whiteout_scan_task *tmp = realloc(q->tasks, (size_t)new_cap * sizeof(*tmp));
        if (!tmp)
            return -ENOMEM;
        q->tasks = tmp;
        q->capacity = new_cap;
    }

    struct whiteout_scan_task *task = &q->tasks[q->total++];
    snprintf(task->lower_root, sizeof(task->lower_root), "%s", lower_root);
    snprintf(task->start_path, sizeof(task->start_path), "%s", start_path);
    snprintf(task->rel_path, sizeof(task->rel_path), "%s", rel_path ? rel_path : "");
    snprintf(task->upperdir, sizeof(task->upperdir), "%s", upperdir);
    task->dcfg = dcfg;
    task->results = results;
    return 0;
}

static int whiteout_scan_queue_next(struct whiteout_scan_queue *q)
{
    int index;
    pthread_mutex_lock(&q->lock);
    index = q->next++;
    pthread_mutex_unlock(&q->lock);
    return (index < q->total) ? index : -1;
}

static int segment_matches(const char *pattern_seg, const char *path_seg)
{
    return fnmatch(pattern_seg, path_seg, 0) == 0;
}

static int pattern_matches_segments(char **pat_segs, int pi, int pc,
                                    char **path_segs, int si, int sc)
{
    if (pi == pc)
        return si == sc;

    if (strcmp(pat_segs[pi], "**") == 0) {
        if (pi + 1 == pc)
            return 1;
        for (int skip = si; skip <= sc; skip++) {
            if (pattern_matches_segments(pat_segs, pi + 1, pc, path_segs, skip, sc))
                return 1;
        }
        return 0;
    }

    if (si >= sc)
        return 0;

    if (!segment_matches(pat_segs[pi], path_segs[si]))
        return 0;

    return pattern_matches_segments(pat_segs, pi + 1, pc, path_segs, si + 1, sc);
}

static int split_segments(const char *value, char segments[][256], int max_segments)
{
    if (!value || !value[0])
        return 0;

    char buf[4096];
    snprintf(buf, sizeof(buf), "%s", value);
    int count = 0;
    char *saveptr = NULL;
    char *tok = strtok_r(buf, "/", &saveptr);
    while (tok && count < max_segments) {
        snprintf(segments[count], sizeof(segments[count]), "%s", tok);
        count++;
        tok = strtok_r(NULL, "/", &saveptr);
    }
    return count;
}

static int whiteout_pattern_matches(const char *pattern, const char *rel_path)
{
    char pat_storage[128][256];
    char rel_storage[128][256];
    char *pat_ptrs[128];
    char *rel_ptrs[128];

    int pat_count = split_segments(pattern, pat_storage, 128);
    int rel_count = split_segments(rel_path, rel_storage, 128);
    for (int i = 0; i < pat_count; i++)
        pat_ptrs[i] = pat_storage[i];
    for (int i = 0; i < rel_count; i++)
        rel_ptrs[i] = rel_storage[i];

    return pattern_matches_segments(pat_ptrs, 0, pat_count, rel_ptrs, 0, rel_count);
}

static int whiteout_matches_any(const struct daemon_config *dcfg, const char *rel_path)
{
    for (int i = 0; i < dcfg->post_mount_whiteout_dir_count; i++) {
        if (whiteout_pattern_matches(dcfg->post_mount_whiteout_dirs[i], rel_path))
            return 1;
    }
    return 0;
}

static int ensure_whiteout_dir(const char *upperdir, const char *rel_path)
{
    char target[4096];
    int n = snprintf(target, sizeof(target), "%s/%s", upperdir, rel_path);
    if (n <= 0 || n >= (int)sizeof(target)) {
        log_write("Warning: whiteout target path too long for '%s'\n", rel_path);
        return -1;
    }
    if (mkdir_p(target, 0775) < 0 && errno != EEXIST) {
        log_write("Warning: failed to create whiteout dir '%s': %s\n", target, strerror(errno));
        return -1;
    }
    if (setxattr(target, OVERLAY_OPAQUE_XATTR, "y", 1, 0) < 0) {
        if (errno != EEXIST) {
            log_write("Warning: failed to set whiteout xattr on '%s': %s\n", target, strerror(errno));
            return -1;
        }
    }
    log_write("Prepared directory whiteout at %s\n", target);
    return 0;
}

struct whiteout_match {
    char rel_path[4096];
    char upperdir[4096];
};

struct whiteout_results {
    struct whiteout_match *items;
    int total;
    int capacity;
    pthread_mutex_t lock;
};

static int whiteout_results_init(struct whiteout_results *res)
{
    memset(res, 0, sizeof(*res));
    pthread_mutex_init(&res->lock, NULL);
    return 0;
}

static void whiteout_results_destroy(struct whiteout_results *res)
{
    free(res->items);
    pthread_mutex_destroy(&res->lock);
}

static int whiteout_results_add(struct whiteout_results *res,
                                const char *rel_path,
                                const char *upperdir)
{
    pthread_mutex_lock(&res->lock);
    if (res->total >= res->capacity) {
        int new_cap = res->capacity ? res->capacity * 2 : 1024;
        struct whiteout_match *tmp = realloc(res->items, (size_t)new_cap * sizeof(*tmp));
        if (!tmp) {
            pthread_mutex_unlock(&res->lock);
            return -ENOMEM;
        }
        res->items = tmp;
        res->capacity = new_cap;
    }
    snprintf(res->items[res->total].rel_path, sizeof(res->items[res->total].rel_path),
             "%s", rel_path);
    snprintf(res->items[res->total].upperdir, sizeof(res->items[res->total].upperdir),
             "%s", upperdir);
    res->total++;
    pthread_mutex_unlock(&res->lock);
    return 0;
}

static void scan_whiteout_dirs(const char *path,
                               const char *rel_path,
                               const char *upperdir,
                               const struct daemon_config *dcfg,
                               struct whiteout_results *results)
{
    struct stat st;
    if (lstat(path, &st) < 0 || !S_ISDIR(st.st_mode))
        return;

    if (rel_path[0] && whiteout_matches_any(dcfg, rel_path))
        (void)whiteout_results_add(results, rel_path, upperdir);

    DIR *dir = opendir(path);
    if (!dir)
        return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char child[4096];
        int cn = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (cn <= 0 || cn >= (int)sizeof(child))
            continue;

        struct stat cst;
        if (lstat(child, &cst) < 0 || !S_ISDIR(cst.st_mode))
            continue;

        char child_rel[4096];
        if (rel_path[0])
            snprintf(child_rel, sizeof(child_rel), "%s/%s", rel_path, entry->d_name);
        else
            snprintf(child_rel, sizeof(child_rel), "%s", entry->d_name);

        scan_whiteout_dirs(child, child_rel, upperdir, dcfg, results);
    }

    closedir(dir);
}

static int enqueue_whiteout_tasks(struct whiteout_scan_queue *q,
                                  const char *lower_root,
                                  const char *path,
                                  const char *rel_path,
                                  int depth,
                                  const char *upperdir,
                                  const struct daemon_config *dcfg,
                                  struct whiteout_results *results)
{
    struct stat st;
    if (lstat(path, &st) < 0 || !S_ISDIR(st.st_mode))
        return 0;

    if (depth >= WHITEOUT_SCAN_SPLIT_DEPTH)
        return whiteout_scan_queue_push(q, lower_root, path, rel_path, upperdir, dcfg, results);

    DIR *dir = opendir(path);
    if (!dir)
        return whiteout_scan_queue_push(q, lower_root, path, rel_path, upperdir, dcfg, results);

    int pushed = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char child[4096];
        int cn = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (cn <= 0 || cn >= (int)sizeof(child))
            continue;

        struct stat cst;
        if (lstat(child, &cst) < 0 || !S_ISDIR(cst.st_mode))
            continue;

        char child_rel[4096];
        if (rel_path && rel_path[0])
            snprintf(child_rel, sizeof(child_rel), "%s/%s", rel_path, entry->d_name);
        else
            snprintf(child_rel, sizeof(child_rel), "%s", entry->d_name);

        int rc = enqueue_whiteout_tasks(q, lower_root, child, child_rel,
                                        depth + 1, upperdir, dcfg, results);
        if (rc < 0) {
            closedir(dir);
            return rc;
        }
        pushed += rc;
    }
    closedir(dir);

    if (pushed == 0)
        return whiteout_scan_queue_push(q, lower_root, path, rel_path, upperdir, dcfg, results);

    return pushed;
}

static void *whiteout_scan_worker_thread(void *arg)
{
    struct whiteout_scan_queue *q = (struct whiteout_scan_queue *)arg;
    for (;;) {
        int index = whiteout_scan_queue_next(q);
        if (index < 0)
            break;
        struct whiteout_scan_task *task = &q->tasks[index];
        scan_whiteout_dirs(task->start_path, task->rel_path, task->upperdir, task->dcfg, task->results);
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Whiteout create worker
 * ---------------------------------------------------------------------- */

struct whiteout_create_task {
    char rel_path[4096];
    char upperdir[4096];
};

struct whiteout_create_queue {
    struct whiteout_create_task *tasks;
    int total;
    int capacity;
    int next;
    pthread_mutex_t lock;
};

static void whiteout_create_queue_init(struct whiteout_create_queue *q)
{
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->lock, NULL);
}

static void whiteout_create_queue_destroy(struct whiteout_create_queue *q)
{
    free(q->tasks);
    pthread_mutex_destroy(&q->lock);
}

static int whiteout_create_queue_push(struct whiteout_create_queue *q,
                                      const char *rel_path,
                                      const char *upperdir)
{
    if (q->total >= q->capacity) {
        int new_cap = q->capacity ? q->capacity * 2 : 1024;
        struct whiteout_create_task *tmp = realloc(q->tasks, (size_t)new_cap * sizeof(*tmp));
        if (!tmp)
            return -ENOMEM;
        q->tasks = tmp;
        q->capacity = new_cap;
    }
    snprintf(q->tasks[q->total].rel_path, sizeof(q->tasks[q->total].rel_path), "%s", rel_path);
    snprintf(q->tasks[q->total].upperdir, sizeof(q->tasks[q->total].upperdir), "%s", upperdir);
    q->total++;
    return 0;
}

static int whiteout_create_queue_next(struct whiteout_create_queue *q)
{
    int index;
    pthread_mutex_lock(&q->lock);
    index = q->next++;
    pthread_mutex_unlock(&q->lock);
    return (index < q->total) ? index : -1;
}

static void *whiteout_create_worker_thread(void *arg)
{
    struct whiteout_create_queue *q = (struct whiteout_create_queue *)arg;
    for (;;) {
        int index = whiteout_create_queue_next(q);
        if (index < 0)
            break;
        struct whiteout_create_task *task = &q->tasks[index];
        (void)ensure_whiteout_dir(task->upperdir, task->rel_path);
    }
    return NULL;
}

static int prepare_whiteout_dirs(const char *effective_lowerdir,
                                 const char *upperdir,
                                 const struct daemon_config *dcfg)
{
    if (!dcfg || dcfg->post_mount_whiteout_dir_count <= 0)
        return 0;

    char lower_copy[8192];
    snprintf(lower_copy, sizeof(lower_copy), "%s", effective_lowerdir);

    struct whiteout_results results;
    whiteout_results_init(&results);

    struct whiteout_scan_queue scan_q;
    whiteout_scan_queue_init(&scan_q);

    char *saveptr = NULL;
    char *token = strtok_r(lower_copy, ":", &saveptr);
    while (token) {
        struct stat st;
        if (lstat(token, &st) == 0 && S_ISDIR(st.st_mode)) {
            int rc = enqueue_whiteout_tasks(&scan_q, token, token, "", 0, upperdir, dcfg, &results);
            if (rc < 0) {
                whiteout_scan_queue_destroy(&scan_q);
                whiteout_results_destroy(&results);
                return rc;
            }
        }
        token = strtok_r(NULL, ":", &saveptr);
    }

    if (scan_q.total == 0) {
        whiteout_scan_queue_destroy(&scan_q);
        whiteout_results_destroy(&results);
        return 0;
    }

    int workers = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (workers < 1)
        workers = 1;
    if (workers > scan_q.total)
        workers = scan_q.total;

    pthread_t *threads = calloc((size_t)workers, sizeof(*threads));
    if (!threads) {
        whiteout_scan_queue_destroy(&scan_q);
        whiteout_results_destroy(&results);
        return -ENOMEM;
    }

    int started = 0;
    for (int i = 0; i < workers; i++) {
        if (pthread_create(&threads[i], NULL, whiteout_scan_worker_thread, &scan_q) == 0)
            started++;
    }

    if (started == 0) {
        free(threads);
        whiteout_scan_queue_destroy(&scan_q);
        whiteout_results_destroy(&results);
        return -EIO;
    }

    for (int i = 0; i < workers; i++) {
        if (threads[i])
            pthread_join(threads[i], NULL);
    }

    free(threads);
    whiteout_scan_queue_destroy(&scan_q);

    if (results.total <= 0) {
        whiteout_results_destroy(&results);
        return 0;
    }

    struct whiteout_create_queue create_q;
    whiteout_create_queue_init(&create_q);

    for (int i = 0; i < results.total; i++) {
        int rc = whiteout_create_queue_push(&create_q, results.items[i].rel_path, results.items[i].upperdir);
        if (rc < 0) {
            whiteout_create_queue_destroy(&create_q);
            whiteout_results_destroy(&results);
            return rc;
        }
    }

    workers = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (workers < 1)
        workers = 1;
    if (workers > create_q.total)
        workers = create_q.total;

    threads = calloc((size_t)workers, sizeof(*threads));
    if (!threads) {
        whiteout_create_queue_destroy(&create_q);
        whiteout_results_destroy(&results);
        return -ENOMEM;
    }

    started = 0;
    for (int i = 0; i < workers; i++) {
        if (pthread_create(&threads[i], NULL, whiteout_create_worker_thread, &create_q) == 0)
            started++;
    }

    if (started == 0) {
        free(threads);
        whiteout_create_queue_destroy(&create_q);
        whiteout_results_destroy(&results);
        return -EIO;
    }

    for (int i = 0; i < workers; i++) {
        if (threads[i])
            pthread_join(threads[i], NULL);
    }

    free(threads);
    whiteout_create_queue_destroy(&create_q);
    whiteout_results_destroy(&results);

    return 0;
}

struct prepare_whiteout_req {
    char effective_lowerdir[8192];
    char upperdir[4096];
    struct daemon_config dcfg_copy;
};

static void *prepare_whiteout_thread(void *arg)
{
    struct prepare_whiteout_req *req = (struct prepare_whiteout_req *)arg;
    int rc = prepare_whiteout_dirs(req->effective_lowerdir, req->upperdir, &req->dcfg_copy);
    if (rc < 0) {
        log_write("Warning: async whiteout preparation had errors: %s\n", strerror(-rc));
    } else if (rc == 0) {
        log_write("Async whiteout preparation completed\n");
    }
    free(req);
    return NULL;
}

int start_prepare_whiteout_async(const char *effective_lowerdir,
                                 const char *upperdir,
                                 const struct daemon_config *dcfg)
{
    if (!dcfg || dcfg->post_mount_whiteout_dir_count <= 0)
        return 0;

    struct prepare_whiteout_req *req = calloc(1, sizeof(*req));
    if (!req)
        return -ENOMEM;

    snprintf(req->effective_lowerdir, sizeof(req->effective_lowerdir), "%s", effective_lowerdir);
    snprintf(req->upperdir, sizeof(req->upperdir), "%s", upperdir);

    // Copy dcfg into req for thread to use
    memcpy(&req->dcfg_copy, dcfg, sizeof(*dcfg));

    pthread_t tid;
    if (pthread_create(&tid, NULL, prepare_whiteout_thread, req) != 0) {
        int saved = errno;
        free(req);
        return -saved;
    }
    pthread_detach(tid);
    return 0;
}

/* -------------------------------------------------------------------------
 * Post-mount chown
 * ---------------------------------------------------------------------- */

struct post_mount_chown_req {
    char sandboxname[2048];
    uid_t uid;
    gid_t gid;
    int dir_count;
    char dirs[MAX_POST_MOUNT_CHOWN_DIRS][MAX_POST_MOUNT_CHOWN_DIR_LEN];
};

static void post_mount_chown_one(const char *sandboxname,
                                 const char *rel,
                                 uid_t uid,
                                 gid_t gid)
{
    char target[4096];
    int tn = snprintf(target, sizeof(target), "%s/%s", sandboxname, rel);
    if (tn <= 0 || tn >= (int)sizeof(target)) {
        log_write("Warning: post-mount chown target path too long for '%s'\n", rel);
        return;
    }

    if (has_wildcard(rel)) {
        glob_t g;
        memset(&g, 0, sizeof(g));
        int grc = glob(target, 0, NULL, &g);
        if (grc == GLOB_NOMATCH) {
            log_write("Post-mount chown skip: configured wildcard path '%s' matched nothing\n",
                      target);
            globfree(&g);
            return;
        }
        if (grc != 0) {
            log_write("Warning: post-mount wildcard expansion failed for '%s'\n", target);
            globfree(&g);
            return;
        }

        for (size_t gi = 0; gi < g.gl_pathc; gi++) {
            const char *matched = g.gl_pathv[gi];
            if (chown_dir_recursive(matched, uid, gid) < 0) {
                log_write("Warning: post-mount recursive chown had errors under %s\n", matched);
            } else {
                log_write("Post-mount recursive chown completed for %s\n", matched);
            }
        }
        globfree(&g);
        return;
    }

    struct stat st;
    if (lstat(target, &st) < 0) {
        if (errno == ENOENT) {
            log_write("Post-mount chown skip: configured path '%s' does not exist\n",
                      target);
        } else {
            log_write("Warning: post-mount chown target '%s' inaccessible: %s\n",
                      target, strerror(errno));
        }
        return;
    }

    if (chown_dir_recursive(target, uid, gid) < 0) {
        log_write("Warning: post-mount recursive chown had errors under %s\n", target);
    } else {
        log_write("Post-mount recursive chown completed for %s\n", target);
    }
}

static void *post_mount_chown_thread(void *arg)
{
    struct post_mount_chown_req *req = (struct post_mount_chown_req *)arg;
    for (int i = 0; i < req->dir_count; i++) {
        post_mount_chown_one(req->sandboxname, req->dirs[i], req->uid, req->gid);
    }
    free(req);
    return NULL;
}

int start_post_mount_chown_async(const char *sandboxname,
                                 uid_t uid,
                                 gid_t gid,
                                 const struct daemon_config *dcfg)
{
    if (!sandboxname || !sandboxname[0] || !dcfg || dcfg->post_mount_chown_dir_count <= 0)
        return 0;

    struct post_mount_chown_req *req = calloc(1, sizeof(*req));
    if (!req)
        return -ENOMEM;

    snprintf(req->sandboxname, sizeof(req->sandboxname), "%s", sandboxname);
    req->uid = uid;
    req->gid = gid;
    req->dir_count = dcfg->post_mount_chown_dir_count;
    if (req->dir_count > MAX_POST_MOUNT_CHOWN_DIRS)
        req->dir_count = MAX_POST_MOUNT_CHOWN_DIRS;

    for (int i = 0; i < req->dir_count; i++) {
        snprintf(req->dirs[i], sizeof(req->dirs[i]), "%s", dcfg->post_mount_chown_dirs[i]);
    }

    pthread_t tid;
    if (pthread_create(&tid, NULL, post_mount_chown_thread, req) != 0) {
        int saved = errno;
        free(req);
        return -saved;
    }
    pthread_detach(tid);
    return 0;
}
