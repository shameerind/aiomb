
#define LOCKS_DIR "/run/overlay_helper/locks"

void lock_sandbox(const char *name);
void unlock_sandbox(const char *name);
