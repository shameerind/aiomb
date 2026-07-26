#include "common.h"
#include <fcntl.h>
#include "locks.h"
#include "util.h"

static int fd = -1;

void lock_sandbox(const char *name) {
    
    // create locks dir if it doesn't exist
    char locks_dir[256];
    snprintf(locks_dir, sizeof(locks_dir), "%s", LOCKS_DIR);
    mkdir_p(locks_dir, 0755);

    char p[256];
    snprintf(p, sizeof(p), "%s/%s.lock", LOCKS_DIR, name);
    fd = open(p, O_CREAT|O_RDWR, 0644);
    if (fd == -1) {
        perror("open");
        return;
    }
    if (lockf(fd, F_LOCK, 0) == -1) {
        perror("lockf");
        close(fd);
        fd = -1;
    }
}

void unlock_sandbox(const char *name) {
    (void)name;  /* unused for now */
    if (fd == -1) {
        return;
    }
    if (lockf(fd, F_ULOCK, 0) == -1) {
        perror("lockf");
    }
    close(fd);
    fd = -1;
}
