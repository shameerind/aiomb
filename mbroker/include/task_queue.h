#pragma once
#include <pthread.h>
#include <sys/types.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "logger.h"

#define TASK_QUEUE_INITIAL 4096
#define TASK_ENQUEUE_LEVELS 6

struct task_entry {
    char path[4096];
    uid_t uid;
};

struct task_queue {
    struct task_entry *tasks;
    int total;
    int capacity;
    int next;
    void *stats;       /* opaque; used by chown for cycle_stats */
    pthread_mutex_t lock;
};

static inline void task_queue_init(struct task_queue *q) {
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->lock, NULL);
}

static inline void task_queue_destroy(struct task_queue *q) {
    free(q->tasks);
    q->tasks = NULL;
    q->capacity = 0;
    pthread_mutex_destroy(&q->lock);
}

static inline int task_queue_push(struct task_queue *q, const char *path, uid_t uid) {
    if (q->total >= q->capacity) {
        int new_cap = q->capacity ? q->capacity * 2 : TASK_QUEUE_INITIAL;
        struct task_entry *tmp = realloc(q->tasks, (size_t)new_cap * sizeof(*tmp));
        if (!tmp) {
            log_write("Task queue realloc failed at %d entries: %s\n",
                      q->total, strerror(errno));
            return -1;
        }
        q->tasks = tmp;
        q->capacity = new_cap;
    }

    struct task_entry *t = &q->tasks[q->total++];
    snprintf(t->path, sizeof(t->path), "%s", path);
    t->uid = uid;
    return 0;
}

/* Grab next index atomically; returns -1 when queue is exhausted */
static inline int task_queue_next(struct task_queue *q) {
    int index;
    pthread_mutex_lock(&q->lock);
    index = q->next++;
    pthread_mutex_unlock(&q->lock);
    return (index < q->total) ? index : -1;
}
