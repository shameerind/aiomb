#!/usr/bin/env bash
set -euo pipefail

# Reproduce and diagnose EPERM on removing lower-only overlay entries.
#
# Example:
#   ./repro_overlayfs_eperm.sh /b/workspace/mysb3/obj-arm/install/junos/images/flask_session
#
# Notes:
# - By default this script attempts to remove the target directory with rmdir.
# - Use --no-delete for diagnostics only.

usage() {
  cat <<'EOF'
Usage:
  repro_overlayfs_eperm.sh [--no-delete] <absolute-target-dir-on-overlay>

Options:
  --no-delete   Do not attempt rmdir on target; run diagnostics only
  -h, --help    Show this help
EOF
}

log() { printf '%s\n' "$*"; }
warn() { printf 'WARN: %s\n' "$*" >&2; }
err() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

DELETE_TARGET=1
TARGET=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-delete)
      DELETE_TARGET=0
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    -* )
      err "unknown option: $1"
      ;;
    * )
      if [[ -n "$TARGET" ]]; then
        err "only one target path is supported"
      fi
      TARGET="$1"
      shift
      ;;
  esac
done

[[ -n "$TARGET" ]] || { usage; exit 2; }
[[ "$TARGET" = /* ]] || err "target must be an absolute path"
[[ -e "$TARGET" ]] || err "target does not exist: $TARGET"
[[ -d "$TARGET" ]] || err "target is not a directory: $TARGET"

if ! command -v findmnt >/dev/null 2>&1; then
  err "findmnt is required"
fi

# Resolve mount metadata for target.
FSTYPE=$(findmnt -T "$TARGET" -no FSTYPE)
MNTPT=$(findmnt -T "$TARGET" -no TARGET)
OPTS=$(findmnt -T "$TARGET" -no OPTIONS)

[[ "$FSTYPE" = "overlay" ]] || err "target is not on overlayfs (fstype=$FSTYPE)"

# Extract lowerdir/upperdir from mount options.
# This is sufficient for common option strings used in workspace overlays.
LOWER_RAW=$(printf '%s\n' "$OPTS" | sed -n 's/.*lowerdir=\([^,]*\).*/\1/p')
UPPER_RAW=$(printf '%s\n' "$OPTS" | sed -n 's/.*upperdir=\([^,]*\).*/\1/p')

[[ -n "$LOWER_RAW" ]] || err "could not parse lowerdir from mount options"
[[ -n "$UPPER_RAW" ]] || err "could not parse upperdir from mount options"

# Split lowerdir list on unescaped colon.
# Convert escaped colon to sentinel, split, then restore.
SENT=$'\001'
LOWER_TMP="${LOWER_RAW//\\:/$SENT}"
IFS=':' read -r -a LOWER_ARR <<< "$LOWER_TMP"
for i in "${!LOWER_ARR[@]}"; do
  LOWER_ARR[$i]="${LOWER_ARR[$i]//$SENT/:}"
done
unset IFS

if [[ "$TARGET" = "$MNTPT" ]]; then
  REL="."
else
  REL="${TARGET#$MNTPT/}"
fi

PARENT=$(dirname "$TARGET")
BASE=$(basename "$TARGET")

log "== Overlay mount info =="
log "target          : $TARGET"
log "mountpoint      : $MNTPT"
log "fstype          : $FSTYPE"
log "upperdir        : $UPPER_RAW"
log "lowerdir(raw)   : $LOWER_RAW"
log "relative path   : $REL"

log ""
log "== Visible merged path metadata =="
stat -c 'target  mode=%A perm=%a uid=%u gid=%g path=%n' "$TARGET" || true
stat -c 'parent  mode=%A perm=%a uid=%u gid=%g path=%n' "$PARENT" || true

if command -v getfacl >/dev/null 2>&1; then
  log ""
  log "== ACL snapshot (parent then target) =="
  getfacl -cp "$PARENT" 2>/dev/null || true
  getfacl -cp "$TARGET" 2>/dev/null || true
fi

if command -v lsattr >/dev/null 2>&1; then
  log ""
  log "== Extended attrs flags (if supported) =="
  lsattr -d "$PARENT" "$TARGET" 2>/dev/null || true
fi

LOWER_HITS=0
log ""
log "== Lower layer presence =="
for L in "${LOWER_ARR[@]}"; do
  CAND="$L/$REL"
  if [[ -e "$CAND" ]]; then
    LOWER_HITS=$((LOWER_HITS + 1))
    log "present: $CAND"
  else
    log "absent : $CAND"
  fi
done

UPPER_CAND="$UPPER_RAW/$REL"
UPPER_PRESENT=0
if [[ -e "$UPPER_CAND" ]]; then
  UPPER_PRESENT=1
fi

log ""
log "== Upper layer presence =="
if [[ $UPPER_PRESENT -eq 1 ]]; then
  log "present: $UPPER_CAND"
else
  log "absent : $UPPER_CAND"
fi

log ""
log "== Control delete in same parent =="
CTRL="$PARENT/.ovl_repro_ctrl_$$"
mkdir -p "$CTRL"
CTRL_DELETE_OK=0
if rmdir "$CTRL" 2>/tmp/ovl_repro_ctrl_err.$$; then
  CTRL_DELETE_OK=1
  log "control rmdir: success"
else
  log "control rmdir: failed"
  sed 's/^/  /' /tmp/ovl_repro_ctrl_err.$$ || true
fi
rm -f /tmp/ovl_repro_ctrl_err.$$ || true

TARGET_DELETE_RC=0
TARGET_DELETE_MSG="not attempted"
if [[ $DELETE_TARGET -eq 1 ]]; then
  log ""
  log "== Target delete attempt =="
  if rmdir "$TARGET" 2>/tmp/ovl_repro_target_err.$$; then
    TARGET_DELETE_MSG="success"
    log "target rmdir: success"
  else
    TARGET_DELETE_RC=$?
    TARGET_DELETE_MSG=$(tr '\n' ' ' < /tmp/ovl_repro_target_err.$$ | sed 's/[[:space:]]\+/ /g; s/^ //; s/ $//')
    log "target rmdir: failed (rc=$TARGET_DELETE_RC)"
    sed 's/^/  /' /tmp/ovl_repro_target_err.$$ || true
  fi
  rm -f /tmp/ovl_repro_target_err.$$ || true
else
  log ""
  log "== Target delete attempt =="
  log "skipped due to --no-delete"
fi

LOWER_ONLY=0
if [[ $LOWER_HITS -gt 0 && $UPPER_PRESENT -eq 0 ]]; then
  LOWER_ONLY=1
fi

log ""
log "== Summary =="
log "lower_hits              : $LOWER_HITS"
log "upper_present           : $UPPER_PRESENT"
log "lower_only_candidate    : $LOWER_ONLY"
log "control_delete_success  : $CTRL_DELETE_OK"
log "target_delete_rc        : $TARGET_DELETE_RC"
log "target_delete_message   : $TARGET_DELETE_MSG"

if [[ $LOWER_ONLY -eq 1 && $CTRL_DELETE_OK -eq 1 && $TARGET_DELETE_RC -ne 0 ]]; then
  log ""
  log "DIAGNOSIS: lower-only overlay delete path likely failed (whiteout/rename path), consistent with EPERM in current mount cred context."
fi
