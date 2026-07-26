# mrepo / mrepod Installation Guide

This document explains how to build, package, install, and start **mrepod** (daemon) and **mrepo** (client).

## 1) Components

- `mrepod`: daemon that receives mount requests over a Unix domain socket.
- `mrepo`: CLI client used by users/scripts.
- Config file: `/etc/mrepod/mrepod.conf` (simple key=value format).

## 2) Prerequisites

Install required packages (example for Debian/Ubuntu):

```bash
sudo apt-get update
sudo apt-get install -y build-essential libjansson4 libjansson-dev uuid-runtime nfs-kernel-server
```

### Required runtime permissions

`mrepod` performs privileged operations:
- mount/destroy overlay and NFS
- writes `/etc/exports`
- runs `exportfs -ra`

So daemon must run as root (systemd/init scripts already configure this).

## 3) Build from source

From project root:

```bash
cd /b/workspace/overlay/mrepo
make
```

Build output:
- `./mrepod`
- `./mrepo`

Clean build artifacts:

```bash
make clean
```

## 4) Build Debian package

```bash
cd /b/workspace/overlay/mrepo
make install
```

This creates a package like:

- `mrepod_1.0.<YYYYMMDD>_amd64.deb`

## 5) Install package

```bash
cd /b/workspace/overlay/mrepo
sudo dpkg -i mrepod_*.deb
sudo apt-get -f install -y
```

Installed paths:
- Daemon binary: `/usr/sbin/mrepod`
- Client binary: `/usr/bin/mrepo`
- Config file: `/etc/mrepod/mrepod.conf`
- systemd unit: `/etc/systemd/system/mrepod.service`
- init script: `/etc/init.d/mrepod`

## 6) Configure daemon

Edit:

```bash
sudo vi /etc/mrepod/mrepod.conf
```

Config format (key = value, one per line):

```ini
# mrepod daemon configuration
#
# Default overlayroot directory. Upper and work directories for each
# sandbox are created under this path.
overlayroot = /ecloud-tmp/cache
```

Notes:
- Lines starting with `#` are comments.
- The `overlayroot` setting defines where upper/work dirs are stored.
- Clients can override this per-sandbox with `--overlayroot`.

## 7) Start daemon (systemd)

```bash
sudo systemctl daemon-reload
sudo systemctl enable mrepod
sudo systemctl start mrepod
sudo systemctl status mrepod
```

## 8) Start daemon (init.d fallback)

```bash
sudo service mrepod start
sudo service mrepod status
```

## 9) Verify installation

```bash
mrepo version
mrepo help
mrepo list
```

Expected:
- Client connects to daemon socket and returns sandbox list.

## 10) Post-install behavior to know

When mounting a sandbox, daemon also:
1. ensures an `/etc/exports` entry exists for mount path
2. adds line in format:
   `<mountpath> *(rw,sync,no_subtree_check,fsid=<uuid>,crossmnt)`
3. runs `exportfs -ra`

When unmounting a sandbox, daemon:
1. removes corresponding mount-path line from `/etc/exports`
2. runs `exportfs -ra`

When client uses `--chown-owner` with `mrepo create`, daemon:
1. starts a detached one-shot ownership-fix task
2. scopes processing to only the requested sandbox mountpoint
3. does not enable any global ownership monitor loop

## 11) Troubleshooting

### A) Client cannot connect

Error like: `Failed to connect to daemon`

Check:

```bash
sudo systemctl status mrepod
ls -l /run/mrepod/socket
```

### B) Mount fails

Check daemon logs:

```bash
sudo journalctl -u mrepod -n 200 --no-pager
```

Also verify:
- overlayfs support: `cat /proc/filesystems | grep overlay`
- NFS server/path accessibility (if using `host:/path`)

### C) Export refresh fails

Validate command manually:

```bash
sudo /sbin/exportfs -ra
```

Ensure `nfs-kernel-server` tools are installed.

### D) UUID command unavailable

Daemon checks `/bin/uuidgen`.
If not present, it auto-generates a random fsid-like string.

---

For usage and day-to-day operations, see `USER_MANUAL.md`.
