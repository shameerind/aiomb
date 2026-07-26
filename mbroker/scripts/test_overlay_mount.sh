#!/bin/bash
# test_overlay_mount.sh — test overlay mount capability in a given directory
# Usage: test_overlay_mount.sh [testdir]
#   testdir: directory to run the test in (default: /tmp/overlay_test_$$)
#
# Tests (in order):
#   1. mount with nfs_export=on,index=on,override_creds=<uid>:<gid>
#   2. mount with nfs_export=on,index=on (no override_creds)
#   3. basic mount (lowerdir/upperdir/workdir only)
#
# Reports which options are supported and cleans up after itself.

set -euo pipefail

TESTDIR="${1:-/b/workspace/tmp/overlay_test_$$}"
LOWER="$TESTDIR/ot/lower"
UPPER="$TESTDIR/ot/upper"
WORK="$TESTDIR/ot/work"
MOUNT="$TESTDIR/mysb"

UID_VAL=$(id -u)
GID_VAL=$(id -g)

cleanup() {
    if mountpoint -q "$MOUNT" 2>/dev/null; then
        sudo umount "$MOUNT" 2>/dev/null || sudo umount -l "$MOUNT" 2>/dev/null || true
    fi
    rm -rf "$TESTDIR"
}
trap cleanup EXIT

echo "=== Overlay mount capability test ==="
echo "Test directory : $TESTDIR"
echo "Running as     : $(id)"
echo ""

# --- Setup ---
echo "[setup] Creating directories and test files..."
mkdir -p "$LOWER" "$UPPER" "$WORK" "$MOUNT"
touch "$LOWER/shm" "$LOWER/shm1" "$LOWER/shm2" "$LOWER/shm3"
echo "testing file" >> "$LOWER/shmcontents"
echo "[setup] Done."
echo ""

do_mount() {
    local desc="$1"
    local opts="$2"
    # reset upper/work dirs between attempts
    rm -rf "$UPPER" "$WORK"
    mkdir -p "$UPPER" "$WORK"
    if mountpoint -q "$MOUNT" 2>/dev/null; then
        sudo umount "$MOUNT" 2>/dev/null || true
    fi
    echo "[test] $desc"
    echo "       mount -t overlay overlay -o $opts $MOUNT"
    if sudo mount -t overlay overlay -o "$opts" "$MOUNT" 2>/tmp/overlay_test_err; then
        echo "       PASS"
        # verify contents visible through mount
        if [ -f "$MOUNT/shmcontents" ]; then
            echo "       contents check: OK (shmcontents visible)"
        else
            echo "       contents check: WARN (shmcontents not visible)"
        fi
        sudo umount "$MOUNT" 2>/dev/null || true
        return 0
    else
        local err
        err=$(cat /tmp/overlay_test_err)
        echo "       FAIL: $err"
        return 1
    fi
}

PASS=0
FAIL=0

# Test 1: with override_creds + nfs_export + index
if do_mount \
    "with nfs_export=on,index=on,override_creds=${UID_VAL}:${GID_VAL}" \
    "lowerdir=$LOWER,upperdir=$UPPER,workdir=$WORK,nfs_export=on,index=on,override_creds=${UID_VAL}:${GID_VAL}"; then
    PASS=$((PASS+1))
else
    FAIL=$((FAIL+1))
fi
echo ""

# Test 2: with nfs_export + index, no override_creds
if do_mount \
    "with nfs_export=on,index=on (no override_creds)" \
    "lowerdir=$LOWER,upperdir=$UPPER,workdir=$WORK,nfs_export=on,index=on"; then
    PASS=$((PASS+1))
else
    FAIL=$((FAIL+1))
fi
echo ""

# Test 3: basic overlay (minimal options)
if do_mount \
    "basic (lowerdir/upperdir/workdir only)" \
    "lowerdir=$LOWER,upperdir=$UPPER,workdir=$WORK"; then
    PASS=$((PASS+1))
else
    FAIL=$((FAIL+1))
fi
echo ""

echo "=== Results: $PASS passed, $FAIL failed ==="
if [ "$PASS" -eq 0 ]; then
    echo "ERROR: overlay mounts are not functional in this environment."
    exit 1
fi
