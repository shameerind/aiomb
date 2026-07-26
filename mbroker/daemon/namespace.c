#define _GNU_SOURCE
#include "common.h"
#include <sched.h>
#include <fcntl.h>
#include "namespace.h"

static int orig_ns = -1;

int enter_namespace(void) {
    orig_ns = open("/proc/self/ns/mnt", O_RDONLY);
    return 0;
}

void exit_namespace(void) {
    if (orig_ns >= 0) {
        setns(orig_ns, 0);
        close(orig_ns);
        orig_ns = -1;
    }
}
