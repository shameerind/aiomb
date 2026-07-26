/*
 * Unit tests for mrepod components.
 * Tests verify the changes to mountinfo tracking (with flags),
 * daemon config parsing, mkdir_p utility,
 * and protocol definitions.
 *
 * Build:  make test
 * Run:    ./test_unit
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdarg.h>
#include <limits.h>

#include "common.h"
#include "protocol.h"
#include "mountinfo.h"
#include "daemon_config.h"
#include "util.h"

/* ---- Stubs for daemon dependencies not under test ---- */

void log_write(const char *fmt, ...)
{
    (void)fmt;
    /* silent during tests */
}

/* ---- Test infrastructure ---- */

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define RUN_TEST(fn) do {                             \
    printf("  %-55s ", #fn);                          \
    fflush(stdout);                                   \
    tests_run++;                                      \
    fn();                                             \
    tests_passed++;                                   \
    printf("PASS\n");                                 \
} while (0)

/* ========================================================
 * Test 1: mountinfo track + overlayroot_for lookup
 * Verifies the basic track/lookup cycle.
 * ======================================================== */
static void test_mountinfo_track_and_overlayroot_for(void)
{
    int rc = mountinfo_track("/test/sb1", 0, "/overlay/root");
    assert(rc == 0);

    const char *oroot = mountinfo_overlayroot_for("/test/sb1");
    assert(oroot != NULL);
    assert(strcmp(oroot, "/overlay/root") == 0);

    /* Unknown mountpoint returns NULL */
    assert(mountinfo_overlayroot_for("/nonexistent") == NULL);

    mountinfo_untrack("/test/sb1");
}

/* ========================================================
 * Test 2: mountinfo_flags_for
 * Verifies that flags stored via mountinfo_track() are
 * retrievable via mountinfo_flags_for().
 * ======================================================== */
static void test_mountinfo_flags_for(void)
{
    int flags = FLAG_CUSTOM_LOWERDIR | FLAG_OVERRIDE_UID;
    int rc = mountinfo_track("/test/flags_sb", flags, "/overlay");
    assert(rc == 0);

    int got = mountinfo_flags_for("/test/flags_sb");
    assert(got == flags);
    assert(got & FLAG_CUSTOM_LOWERDIR);
    assert(got & FLAG_OVERRIDE_UID);
    assert(!(got & FLAG_LAZY));
    assert(!(got & FLAG_FORCE));

    /* Untracked path should return 0 flags */
    assert(mountinfo_flags_for("/no/such/path") == 0);

    mountinfo_untrack("/test/flags_sb");
}

/* ========================================================
 * Test 3: mountinfo untrack + tracked_count
 * Verifies track/untrack cycle decrements the active count.
 * ======================================================== */
static void test_mountinfo_untrack_and_count(void)
{
    int base = mountinfo_tracked_count();

    mountinfo_track("/test/cnt1", 0, "/overlay");
    mountinfo_track("/test/cnt2", FLAG_LAZY, "/overlay");
    mountinfo_track("/test/cnt3", FLAG_PROMOTE_THREADS, "/overlay");
    assert(mountinfo_tracked_count() == base + 3);

    mountinfo_untrack("/test/cnt2");
    assert(mountinfo_tracked_count() == base + 2);

    /* Untrack already-removed entry is a no-op */
    mountinfo_untrack("/test/cnt2");
    assert(mountinfo_tracked_count() == base + 2);

    mountinfo_untrack("/test/cnt1");
    mountinfo_untrack("/test/cnt3");
    assert(mountinfo_tracked_count() == base);
}

/* ========================================================
 * Test 4: mountinfo_describe_all
 * Verifies the describe output contains tracked mountpoints.
 * ======================================================== */
static void test_mountinfo_describe_all(void)
{
    mountinfo_track("/mnt/alpha_sb", 0, "/overlay/a");
    mountinfo_track("/mnt/beta_sb",  FLAG_CUSTOM_LOWERDIR, "/overlay/b");

    char buf[4096];
    int rc = mountinfo_describe_all(buf, sizeof(buf));
    assert(rc == 0);
    assert(strstr(buf, "/mnt/alpha_sb") != NULL);
    assert(strstr(buf, "/mnt/beta_sb") != NULL);

    mountinfo_untrack("/mnt/alpha_sb");
    mountinfo_untrack("/mnt/beta_sb");

    /* With nothing tracked, describe should say "No active sandboxes" */
    if (mountinfo_tracked_count() == 0) {
        rc = mountinfo_describe_all(buf, sizeof(buf));
        assert(rc == 0);
        assert(strstr(buf, "No active") != NULL);
    }
}

/* ========================================================
 * Test 5: mountinfo_snapshot_mountpoints
 * Verifies snapshot captures all active mountpoints.
 * ======================================================== */
static void test_mountinfo_snapshot(void)
{
    mountinfo_track("/snap/a", 0, "/overlay");
    mountinfo_track("/snap/b", FLAG_OVERRIDE_UID, "/overlay");
    mountinfo_track("/snap/c", FLAG_CUSTOM_LOWERDIR, "/overlay");

    char mps[8][2048];
    int n = mountinfo_snapshot_mountpoints(mps, 8);
    assert(n >= 3);

    /* Verify our three mountpoints are in the snapshot */
    int found_a = 0, found_b = 0, found_c = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(mps[i], "/snap/a") == 0) found_a = 1;
        if (strcmp(mps[i], "/snap/b") == 0) found_b = 1;
        if (strcmp(mps[i], "/snap/c") == 0) found_c = 1;
    }
    assert(found_a && found_b && found_c);

    mountinfo_untrack("/snap/a");
    mountinfo_untrack("/snap/b");
    mountinfo_untrack("/snap/c");
}

/* ========================================================
 * Test 6: mountinfo slot reuse
 * After untracking, a new track should reuse the freed slot
 * rather than growing the table.
 * ======================================================== */
static void test_mountinfo_slot_reuse(void)
{
    mountinfo_track("/reuse/x", 0, "/overlay");
    mountinfo_track("/reuse/y", 0, "/overlay");
    int count_before = mountinfo_tracked_count();

    mountinfo_untrack("/reuse/x");
    assert(mountinfo_tracked_count() == count_before - 1);

    /* Track new entry - should reuse the freed slot */
    mountinfo_track("/reuse/z", FLAG_LAZY, "/overlay");
    assert(mountinfo_tracked_count() == count_before);

    /* Verify the new entry is correctly stored */
    const char *oroot = mountinfo_overlayroot_for("/reuse/z");
    assert(oroot != NULL);
    assert(strcmp(oroot, "/overlay") == 0);
    assert(mountinfo_flags_for("/reuse/z") == FLAG_LAZY);

    mountinfo_untrack("/reuse/y");
    mountinfo_untrack("/reuse/z");
}

/* ========================================================
 * Test 7: daemon config load + get
 * Writes a temporary config file and verifies config_load()
 * parses the overlayroot key correctly.
 * ======================================================== */
static void test_config_load_and_get(void)
{
    char tmppath[] = "/tmp/mrepod_test_conf_XXXXXX";
    int fd = mkstemp(tmppath);
    assert(fd >= 0);

    const char *content =
        "# test config\n"
        "overlayroot = /test/overlay/root\n"
        "\n"
        "# comment line\n";
    ssize_t nw = write(fd, content, strlen(content));
    assert(nw == (ssize_t)strlen(content));
    close(fd);

    int rc = config_load(tmppath);
    assert(rc == 0);

    const struct daemon_config *cfg = config_get();
    assert(cfg != NULL);
    assert(strcmp(cfg->overlayroot, "/test/overlay/root") == 0);

    unlink(tmppath);
}

/* ========================================================
 * Test 8: mkdir_p
 * Creates nested directories under /tmp and verifies they
 * exist.
 * ======================================================== */
static void test_mkdir_p(void)
{
    char base[] = "/tmp/mrepod_test_mkdir_XXXXXX";
    char *dir = mkdtemp(base);
    assert(dir != NULL);

    char nested[512];
    snprintf(nested, sizeof(nested), "%s/a/b/c/d", dir);

    int rc = mkdir_p(nested, 0755);
    assert(rc == 0);

    struct stat st;
    assert(stat(nested, &st) == 0);
    assert(S_ISDIR(st.st_mode));

    /* Calling again on existing path should succeed */
    rc = mkdir_p(nested, 0755);
    assert(rc == 0);

    /* Cleanup */
    rmdir(nested);
    snprintf(nested, sizeof(nested), "%s/a/b/c", dir);
    rmdir(nested);
    snprintf(nested, sizeof(nested), "%s/a/b", dir);
    rmdir(nested);
    snprintf(nested, sizeof(nested), "%s/a", dir);
    rmdir(nested);
    rmdir(dir);
}

/* ========================================================
 * Test 9: protocol flag definitions
 * Verifies that flag bits are unique and struct fields have
 * expected sizes.  Catches accidental redefinition errors.
 * ======================================================== */
static void test_protocol_flags_and_struct(void)
{
    /* Each flag must occupy a unique bit */
    int all_flags[] = {
        FLAG_LAZY, FLAG_FORCE, FLAG_PROMOTE_THREADS,
        FLAG_CHOWN_OWNER_THREAD, FLAG_OVERRIDE_UID,
        FLAG_CUSTOM_LOWERDIR
    };
    int n = sizeof(all_flags) / sizeof(all_flags[0]);
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            assert((all_flags[i] & all_flags[j]) == 0);
        }
        /* Each flag should be a single bit */
        assert(all_flags[i] != 0);
        assert((all_flags[i] & (all_flags[i] - 1)) == 0);
    }

    /* Verify command constants are distinct */
    assert(CMD_CREATE != CMD_DESTROY);
    assert(CMD_DESTROY != CMD_REFRESH);
    assert(CMD_REFRESH != CMD_SANDBOX_LIST);
    assert(CMD_SANDBOX_LIST != CMD_RECOVER);

    /* Struct field sizes match expectations */
    struct mount_request req;
    assert(sizeof(req.sandboxname) == 2048);
    assert(sizeof(req.lowerdir) == 2048);
    assert(sizeof(req.overlayroot) == 2048);

    struct mount_reply rep;
    assert(sizeof(rep.details) == REPLY_DETAILS_MAX);
}

/* ---- Main ---- */

int main(void)
{
    printf("\n=== mrepod unit tests ===\n\n");

    RUN_TEST(test_mountinfo_track_and_overlayroot_for);
    RUN_TEST(test_mountinfo_flags_for);
    RUN_TEST(test_mountinfo_untrack_and_count);
    RUN_TEST(test_mountinfo_describe_all);
    RUN_TEST(test_mountinfo_snapshot);
    RUN_TEST(test_mountinfo_slot_reuse);
    RUN_TEST(test_config_load_and_get);
    RUN_TEST(test_mkdir_p);
    RUN_TEST(test_protocol_flags_and_struct);

    printf("\n--- Results: %d/%d passed, %d failed ---\n\n",
           tests_passed, tests_run, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
