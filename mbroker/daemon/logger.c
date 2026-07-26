#include "common.h"
#include "logger.h"
#include <stdarg.h>
#include <time.h>

static FILE *log_file = NULL;
static int log_to_stdout = 1;

int log_init(const char *logpath, int also_stdout) {
    log_to_stdout = also_stdout;
    
    if (logpath) {
        log_file = fopen(logpath, "a");
        if (!log_file) {
            fprintf(stderr, "Failed to open log file %s: %s\n", logpath, strerror(errno));
            return -1;
        }
        // Make log file line-buffered
        setlinebuf(log_file);
    }
    return 0;
}

void log_write(const char *format, ...) {
    char timestamp[64];
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
    
    va_list args;
    
    // Write to log file with timestamp
    if (log_file) {
        fprintf(log_file, "[%s] ", timestamp);
        va_start(args, format);
        vfprintf(log_file, format, args);
        va_end(args);
        fflush(log_file);
    }
    
    // Also write to stdout if enabled
    if (log_to_stdout) {
        va_start(args, format);
        vprintf(format, args);
        va_end(args);
        fflush(stdout);
    }
}

void log_close(void) {
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
}
