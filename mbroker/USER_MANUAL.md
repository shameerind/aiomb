# mrepo User Manual

This manual describes how to use `mrepo` and what `mrepod` does for each command.

## 1) Command summary

```bash
mrepo create <sandboxname> -pbsb <prebuild-sb-name> [options]
mrepo destroy <sandboxname>
mrepo list
mrepo recover
mrepo version
mrepo help
```

## 2) Concepts

- **sandboxname**: target mount point directory for overlay mount.
- **overlayroot**: base directory under which upper/work dirs and mount records are created. Configured in `/etc/mrepod/mrepod.conf`, or per-command with `--overlayroot` / `-p`.

Daemon receives requests over Unix socket and executes privileged operations.

## 3) Configuration

### Daemon config: `/etc/mrepod/mrepod.conf`

Simple `key = value` format. Lines starting with `#` are comments.

```
# Default overlayroot for all sandboxes
overlayroot = /ecloud-tmp/cache
```

Overlayroot precedence (highest to lowest):
1. `--overlayroot` / `-p` on the command line
2. `overlayroot` in `mrepod.conf`

## 4) `create` command

### Syntax

```bash
mrepo create <sandboxname> -pbsb <prebuild-sb-name> [options]
```

`-pbsb` is **required**.

### Options

| Option | Description |
|--------|-------------|
| `-pbsb <name>` | Mount a prebuild sandbox via `baas` and use it as lower dir |
| `-p`, `--overlayroot <path>` | Override the default overlayroot directory |
| `--promote` | Enable daemon promotion worker threads on demand |
| `--chown-owner` | Start a one-shot ownership-fix task for the sandbox mountpoint |
| `--no-override-uid` | Mount overlay without `override_creds` |


### `-pbsb` / prebuild sandbox details

When `-pbsb <name>` is given, the client:

1. Runs `baas createvol -s <name> -c _cd-builder/<name> --skipco --cmdline`
2. Runs `baas edit -s <name> -bmp --login False`
3. Uses `/mnt/side-vols/<name>` as the lowerdir for the overlay

The sandbox name must consist only of alphanumeric characters, hyphens, underscores, and dots.

### Examples

```bash
# Basic create with lowerdir
cd /b/workspace && mrepo create mysb1 --lowerdir /path/to/lower

# Using short flags
cd /b/workspace && mrepo create mysb1 -l /path/to/lower -p /fast/ssd/cache

# Absolute sandbox path
mrepo create /b/workspace/mysb1 --lowerdir /path/to/lower

# Custom overlayroot
cd /b/workspace && mrepo create mysb1 --lowerdir /path/to/lower --overlayroot /fast/ssd/cache

# Custom lowerdir + overlayroot + promote
cd /b/workspace && mrepo create mysb1 -l /local:srv:/nfs --overlayroot /fast/ssd --promote

# Prebuild sandbox as lower dir
cd /b/workspace && mrepo create mysb1 -pbsb pbsb--dev-common-branch--1550324

# Prebuild sandbox with custom overlayroot
cd /b/workspace && mrepo create mysb1 -pbsb pbsb--evo--rel262-202604211028-1 -p /fast/nvme/scratch
```

### What happens internally

1. Client resolves sandbox path (relative becomes absolute using cwd).
2. If `-pbsb` is given, client runs `baas createvol` and `baas edit`, then sets lowerdir to `/mnt/side-vols/<name>`.
3. Client sends request to daemon over Unix socket.
4. Daemon determines `overlayroot`:
   - Uses `--overlayroot` / `-p` if provided
   - Falls back to `overlayroot` in `mrepod.conf`
5. Daemon prepares overlay directory paths:
   - `upperdir`: `<overlayroot>/<sandbox_basename>/upper`
   - `workdir`: `<overlayroot>/<sandbox_basename>/work`
6. Lowerdir processing:
   - Local paths (starting with `/`) are validated for accessibility
   - NFS sources (`host:/path`) are mounted read-only under `<overlayroot>/<sandbox_basename>-clower-N`
7. Daemon mounts overlay at sandbox mountpoint.
8. Daemon tracks sandbox (with flags and overlayroot) for future unmount.
9. Daemon ensures `/etc/exports` has mountpoint export and runs `exportfs -ra`.
10. Daemon saves a `mountinfo.json` record under `<overlayroot>/<sandbox_basename>/` with all details needed for remounting.

### Mount record (`mountinfo.json`)

After a successful create, a JSON file is saved at `<overlayroot>/<sandbox_basename>/mountinfo.json`:

```json
{
  "sandboxname": "/b/workspace/mysb1",
  "overlayroot": "/ecloud-tmp/cache",
  "lowerdir": "/path/to/lower",
  "upperdir": "/ecloud-tmp/cache/mysb1/upper",
  "workdir": "/ecloud-tmp/cache/mysb1/work",
  "baas_path": "/volume/baas_devops/bin/baas",
  "prebuild_sb": "myvol",
  "uid": 1000,
  "gid": 1000,
  "flags": 0,
  "created": "2026-04-15T16:35:00"
}
```

If `--lowerdir` or `--overlayroot` were used, `custom_lowerdir` and `custom_overlayroot` fields are also included. The `baas_path` and `prebuild_sb` fields are present only when `-pbsb` was used during create. This file is automatically cleaned up on `mrepo destroy`.

## 5) `destroy` command

### Syntax

```bash
mrepo destroy <sandboxname>
```

### Examples

```bash
mrepo destroy mysb1
mrepo destroy /b/workspace/mysb1
```

### What happens internally

1. Daemon verifies ownership (only root or the mount owner can destroy).
2. Daemon resolves overlayroot from tracked sandbox data or daemon config.
3. Daemon retrieves stored `baas_path` and `prebuild_sb` from tracking (if the sandbox was created with `-pbsb`).
4. Overlay is unmounted (`umount2` with requested flags).
5. If normal unmount fails and lazy flag not set, daemon retries with `MNT_DETACH`.
6. If sandbox was created with `-pbsb`, daemon asynchronously runs `baas destroypod -s <vol> --skipauth` to clean up the volume.
7. Daemon removes per-sandbox `upperdir` and `workdir` (async cleanup).
8. Daemon removes sandbox export line from `/etc/exports` and runs `exportfs -ra`.
9. Any NFS lowerdirs mounted for this sandbox are unmounted.
10. The `mountinfo.json` record is removed along with the sandbox directory under overlayroot.

## 6) `list` command

### Syntax

```bash
mrepo list
```

Lists all currently active (tracked) sandbox mountpoints.

## 7) `refresh` command

### Syntax

```bash
mrepo refresh
```

Forces daemon to run:

```bash
/sbin/exportfs -ra
```

Useful after manual edits to `/etc/exports` or recovery operations.

## 8) `recover` command

### Syntax

```bash
mrepo recover
```

Restores sandbox mounts after a machine restart by scanning `<overlayroot>` (from `mrepod.conf`) for saved `mountinfo.json` records and re-executing the mount for each.

### What happens internally

1. Daemon reads `overlayroot` from `/etc/mrepod/mrepod.conf`.
2. Scans `<overlayroot>/*/mountinfo.json` for saved mount records.
3. For each record:
   - Skips if sandbox is already mounted.
   - Re-mounts the sandbox with the original parameters (sandboxname, uid, gid, flags, lowerdir, overlayroot).
4. Reports per-sandbox status.

### Example output

```
  ok: /b/workspace/mysb1
  ok: /b/workspace/mysb2
  skip: /b/workspace/mysb3 (already mounted)
Recover complete: 2 restored, 0 failed, 1 skipped
```

Upper dirs with user modifications are preserved on local disk, so after `recover` the sandboxes come back with all prior changes intact.

## 9) Health monitoring

The daemon runs a background health-check thread that inspects all active sandboxes every **10 minutes**. For each tracked sandbox it checks:

1. **Mount presence** — still in `/proc/self/mountinfo`?
2. **Accessibility** — can the mountpoint be stat'd?
3. **Filesystem type** — is it actually overlayfs?
4. **Disk space** — warns if filesystem is ≥95% full

Results are logged to the daemon log:

```
MONITOR: checking 3 active sandbox(es)
MONITOR: sandbox '/b/workspace/mysb3' filesystem 97% full
MONITOR: check complete — 2 healthy, 1 unhealthy (of 3 total)
```

The monitor starts automatically with the daemon and stops on shutdown.

## 10) Common workflows

### A) Normal mount/unmount cycle

```bash
cd /b/workspace
mrepo create mysb1 --lowerdir /path/to/lower
# ... work inside sandbox ...
mrepo destroy mysb1
```

### B) NFS lower dir

```bash
cd /b/workspace
mrepo create mysb1 -l nfsserver:/export/path
mrepo destroy mysb1
```
### `--lowerdir` / `-l` details

Accepts colon-separated paths. Each path can be local or an NFS source:

- **Local path** (starts with `/`): used directly, validated for accessibility
- **NFS source** (`host:/path`): mounted read-only under `<overlayroot>/<sandbox>-clower-N`

Examples:
```bash
# Single local lower dir
mrepo create mysb1 --lowerdir /path/to/lower
mrepo create mysb1 -l /path/to/lower

# Single NFS source
mrepo create mysb1 --lowerdir nfsserver:/export/path

# Multiple local dirs
mrepo create mysb1 --lowerdir /path1:/path2

# Mixed local + NFS
mrepo create mysb1 --lowerdir /local:srv:/nfs/path
```

### C) Multiple lower dirs (mixed local + NFS)

```bash
cd /b/workspace
mrepo create mysb1 --lowerdir /local/base:nfshost:/shared/overlay
mrepo destroy mysb1
```

### D) Custom overlayroot

```bash
cd /b/workspace
mrepo create mysb1 -l /path/to/lower -p /fast/nvme/scratch
mrepo destroy mysb1
```

### E) Recover after machine restart

```bash
mrepo recover
mrepo list
```

### F) Use a prebuild sandbox as lower dir

```bash
cd /b/workspace
mrepo create mysb1 -pbsb pbsb--dev-common-branch--1550324
# ... work inside sandbox ...
mrepo destroy mysb1
```

### G) Refresh exports manually

```bash
mrepo refresh
```

## 11) Error handling tips

### `overlayroot not set`
- Ensure `overlayroot` is set in `/etc/mrepod/mrepod.conf` or pass `--overlayroot` / `-p` on the command line.

### `Failed to connect to daemon`
- Start/check daemon:
  ```bash
  sudo systemctl status mrepod
  ```

### Mount errors
- Check overlay and NFS prerequisites.
- Review daemon logs:
  ```bash
  sudo journalctl -u mrepod -n 200 --no-pager
  ```
  or
  ```bash
  cat /var/log/mrepod.log
  ```

### Export-related errors
- Validate export command directly:
  ```bash
  sudo /sbin/exportfs -ra
  ```

### `Destroy denied`
- Only root or the original mount owner (by UID) can destroy a sandbox.

## 12) Security and operational notes

- Daemon performs privileged FS/network operations; keep `mrepod.conf` controlled.
- All client-supplied string fields are null-terminated on the daemon side before use.
- Mount record JSON output is properly escaped to prevent injection.
- Avoid deleting sandbox directories while mounted.
- Prefer graceful `mrepo destroy` over manual `umount` to keep tracking, exports, and mount records consistent.
- `--chown-owner` is not a global monitor; it runs once per request and only for that mountpoint.
- The health monitor thread logs issues but does not auto-remediate; use `mrepo recover` or manual intervention.
