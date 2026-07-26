#include "common.h"
#include "fault.h"

int fault(const char *name) { return getenv(name) != NULL; }
