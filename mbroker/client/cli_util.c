#include "cli_util.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

void print_usage(const char *progname) {
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

void print_version(void) {
    printf("mrepo version %s\n", VERSION);
    printf("Monorepo sandbox helper client\n");
}
