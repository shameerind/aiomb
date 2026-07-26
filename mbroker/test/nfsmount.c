#include <stdio.h>
#include <sys/mount.h>
#include <errno.h>
#include <string.h>
#include <netdb.h>
#include <arpa/inet.h>

int main() {
    const char *hostname = "qnc-emake-34d";
    const char *remote_path = "/b/shameer/overlay/sb_root";
    const char *target = "/var/src";
    
    // 1. Resolve Hostname to IP
    struct hostent *he = gethostbyname(hostname);
    if (he == NULL) {
        herror("gethostbyname");
        return 1;
    }
    char *ip = inet_ntoa(*(struct in_addr *)he->h_addr);

    // 2. Format Source and Data strings
    char source[256];
    char data[256];
    snprintf(source, sizeof(source), "%s:%s", hostname, remote_path);
    // 'addr=' is MANDATORY; 'nolock' and 'vers=3' improve compatibility
    snprintf(data, sizeof(data), "addr=%s,nolock,vers=3", ip);

    printf("Attempting to mount %s (IP: %s) to %s...\n", source, ip, target);

    // 3. Execute mount (must run as sudo)
    if (mount(source, target, "nfs", 0, data) == 0) {
        printf("Mount successful!\n");
    } else {
        fprintf(stderr, "Mount failed: %s (errno: %d)\n", strerror(errno), errno);
        if (errno == EINVAL) {
            printf("Check: Does /var/src exist? Are NFS utilities installed?\n");
        }
    }

    return 0;
}
