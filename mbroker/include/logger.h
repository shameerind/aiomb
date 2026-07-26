#pragma once
#include <stdio.h>

// Initialize logging system
// logpath: path to log file, or NULL to only log to stdout
// also_stdout: if 1, also print to stdout in addition to log file
int log_init(const char *logpath, int also_stdout);

// Write a log message (printf-style format)
void log_write(const char *format, ...);

// Close log file
void log_close(void);
