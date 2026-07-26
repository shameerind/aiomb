# ML Integration Guide — AI-OMB Inference Sidecar

This document explains how the trained ML models from the `model/` directory
are integrated into the `mbroker` C daemon using a Python inference sidecar.

---

## Architecture Overview

```
┌─────────────────────┐         UNIX Socket          ┌──────────────────────────┐
│   mrepod daemon     │  ◄──── JSON over ────────►   │  inference_server.py     │
│   (C, privileged)   │   /run/mrepod/model.sock     │  (Python, runs as root)  │
│                     │                               │                          │
│  model_client.c ────┼── select_server ──────────►   │  ServerSelectorDQN       │
│                     │◄── {server_index: 3} ─────┤   │  + VAEHealthScorer       │
│                     │                               │                          │
│  monitor.c ─────────┼── check_anomaly ──────────►   │  NFSVAE                  │
│                     │◄── {anomalous: true} ─────┤   │                          │
│                     │                               │                          │
│  (future) ──────────┼── predict_load ───────────►   │  LSTMTransformerForecast │
│                     │◄── {predictions: [...]} ──┤   │                          │
│                     │                               │                          │
│  (future) ──────────┼── optimize_policy ────────►   │  DQN                     │
│                     │◄── {action: 5} ───────────┤   │                          │
└─────────────────────┘                               └──────────────────────────┘
```

**Key design principle:** Every ML call has a graceful fallback.  If the
inference server is down or returns an error, the daemon uses its existing
hardcoded defaults.  ML is never in the critical path for mount operations.

---

## Components

### 1. Python Inference Server (`model/inference_server.py`)

A standalone Python process that:
- Loads all four ML models at startup from saved weight files
- Listens on a UNIX socket (`/run/mrepod/model.sock`)
- Serves inference requests using a length-prefixed JSON protocol
- Handles concurrent requests via a thread pool (default: 4 threads)

**Protocol:**
```
[4 bytes big-endian length][JSON payload]
```

**Supported commands:**

| Command | Input | Output | Model Used |
|---------|-------|--------|------------|
| `select_server` | Server telemetry, predicted load, connections, policy | Best server index + Q-values | ServerSelectorDQN + NFSVAE |
| `check_anomaly` | NFS metrics (N×11) | Per-server anomaly flags | NFSVAE |
| `predict_load` | 24h history (288×16) | Binary mount-policy predictions | LSTMTransformerForecast |
| `optimize_policy` | State vector (18-dim) | Policy action + name | DQN |
| `health` | (none) | Status + loaded model list | (none) |

### 2. C Model Client (`mbroker/daemon/model_client.c`)

A C library linked into the daemon that:
- Connects to the inference server's UNIX socket
- Serializes requests as JSON (using jansson)
- Deserializes responses into C structs
- Returns `-1` on any failure so the caller falls back

**Header:** `mbroker/include/model_client.h`

**Key functions:**
```c
int model_select_server(sock_path, telemetry, predicted_load,
                        connections, policy_index, num_servers,
                        num_policies, &result);

int model_check_anomaly(sock_path, nfs_metrics, num_servers, &result);

int model_predict_load(sock_path, history, timesteps, features, &result);

int model_optimize_policy(sock_path, state, state_dim, &result);

int model_health_check(sock_path);
```

### 3. Daemon Integration Points

| Integration Point | File | What Happens |
|-------------------|------|-------------|
| **NFS server selection** | `mount_ops.c` → `handle_custom_lowerdir()` | Before NFS mount, if `ml_enabled=1` and `nfs_servers` is configured, queries `select_server` to override the hostname |
| **Anomaly detection** | `monitor.c` → `run_health_check()` | After sandbox health checks, queries `check_anomaly` with NFS telemetry and logs warnings |
| **Startup health check** | `main.c` | On daemon start, pings the inference server and logs availability |
| **Configuration** | `daemon_config.c` | Parses `ml_enabled`, `model_socket`, and `nfs_servers` from `mrepod.conf` |

---

## Setup Procedure

### Step 1: Train and Save Model Weights

```bash
cd model/

# 1. Train LSTM-Transformer and save weights
python train.py
# Output: best_model.pt

# 2. Train VAE and save weights + threshold
python -c "
import numpy as np
from vae import NFSVAE, train_vae_model, reconstruction_errors, fit_threshold

x_train = np.random.rand(5000, 11).astype('float32')  # Replace with real data
vae = train_vae_model(x_train, epochs=20)
vae.save_weights('vae_weights.h5')

errors = reconstruction_errors(vae, x_train)
threshold = fit_threshold(errors, 99.0)
with open('vae_threshold.txt', 'w') as f:
    f.write(str(threshold))
print(f'VAE saved. Threshold: {threshold}')
"

# 3. Train DQN and save weights
python -c "
from dqn import MountLifecycleEnv, train_dqn
env = MountLifecycleEnv()
policy_net, target_net = train_dqn(env, episodes=1000, batch_size=64)
policy_net.save_weights('dqn_weights.h5')
print('DQN saved.')
"

# 4. Train Server Selector and save weights
python -c "
from server_selector import NFSServerEnv, train_server_selector
env = NFSServerEnv(num_servers=8, num_policies=6)
policy_net, _ = train_server_selector(env, episodes=500, steps_per_episode=100)
policy_net.save_weights('selector_weights.h5')
print('Server selector saved.')
"
```

After training, you should have these files in `model/`:
```
best_model.pt           # LSTM-Transformer (PyTorch)
vae_weights.h5          # VAE (TensorFlow/Keras)
vae_threshold.txt       # Anomaly threshold (plain text float)
dqn_weights.h5          # DQN policy optimizer (TensorFlow/Keras)
selector_weights.h5     # NFS server selector (TensorFlow/Keras)
```

### Step 2: Install Python Dependencies

```bash
pip install torch tensorflow numpy scikit-learn
```

### Step 3: Deploy Model Files

```bash
sudo mkdir -p /opt/mrepod/model
sudo cp model/*.py model/*.pt model/*.h5 model/*.txt /opt/mrepod/model/
```

### Step 4: Configure the Daemon

Edit `/etc/mrepod/mrepod.conf`:
```ini
# Enable ML integration
ml_enabled = 1

# Path to inference server socket
model_socket = /run/mrepod/model.sock

# NFS server pool (comma-separated hostnames)
nfs_servers = nfs1.example.com,nfs2.example.com,nfs3.example.com,nfs4.example.com
```

### Step 5: Build the Daemon

```bash
cd mbroker
make clean
make
```

The daemon now compiles with `model_client.c` linked in.

### Step 6: Install and Start Services

```bash
# Install daemon package
sudo dpkg -i mrepod_*.deb

# Install inference server systemd unit
sudo cp mbroker/etc/systemd/system/mrepod-inference.service \
        /etc/systemd/system/

# Reload and start
sudo systemctl daemon-reload
sudo systemctl enable mrepod-inference
sudo systemctl start mrepod-inference    # Start inference server FIRST
sudo systemctl restart mrepod            # Then restart daemon
```

### Step 7: Verify

```bash
# Check inference server is running
sudo systemctl status mrepod-inference

# Check daemon logs for ML connectivity
sudo journalctl -u mrepod -n 20
# Should see: "ML inference sidecar is reachable at '/run/mrepod/model.sock'"

# Or check the log file directly
tail -20 /var/log/mrepod.log
```

---

## Testing the Integration

### Test the inference server standalone

```bash
# Start server in foreground for debugging
python model/inference_server.py --socket /tmp/test_model.sock --model-dir model/

# In another terminal, send a health check
python -c "
import socket, struct, json
sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.connect('/tmp/test_model.sock')
msg = json.dumps({'cmd': 'health'}).encode()
sock.sendall(struct.pack('>I', len(msg)) + msg)
length = struct.unpack('>I', sock.recv(4))[0]
print(json.loads(sock.recv(length)))
sock.close()
"
# Expected: {'status': 'ok', 'models_loaded': ['predictor', 'vae', ...]}
```

### Test with missing models

The server gracefully handles missing weight files:
```
2026-07-26T10:00:00 [inference] WARNING VAE weights not found at model/vae_weights.h5 — skipping
2026-07-26T10:00:01 [inference] INFO Model loading complete — 1/4 available: ['predictor']
```

Any request to a missing model returns `{"error": "...", "fallback": true}`,
which the C client interprets as "use default logic."

---

## Data Flow Examples

### NFS Server Selection During Mount

```
1. User:    mrepo create /b/workspace/mysb1 -l nfs1:/exports/base
2. Daemon:  Receives CMD_CREATE
3. mount_ops.c: Enters handle_custom_lowerdir()
4.            → Detects ml_enabled=1 and nfs_server_count > 1
5.            → Calls model_select_server(model.sock, telemetry, ...)
6. model_client.c: Connects to /run/mrepod/model.sock
7.            → Sends: {"cmd":"select_server","params":{...}}
8. inference_server.py:
9.            → VAE scores server health from telemetry
10.           → ServerSelectorDQN picks server 2 (lowest Q-cost)
11.           → Returns: {"server_index": 2, "q_values": [...]}
12. model_client.c: Parses response, returns server_index=2
13. mount_ops.c: Overrides hostname → nfs3.example.com
14.           → mount("nfs3.example.com:/exports/base", target, "nfs", ...)
15.           → Logs: "ML server selection: chose server 2 ('nfs3.example.com')"
```

### Anomaly Detection During Health Check

```
1. monitor.c: run_health_check() runs every 10 minutes
2.          → Completes standard sandbox health checks
3.          → Detects ml_enabled=1 and nfs_server_count > 0
4.          → Calls model_check_anomaly(model.sock, nfs_metrics, ...)
5. inference_server.py:
6.          → NFSVAE reconstructs telemetry
7.          → Server 3 has reconstruction error 1.87 > threshold 0.42
8.          → Returns: {"any_anomalous": true, "servers": [...]}
9. monitor.c: Logs: "ML anomaly detected on NFS server 3 (error=1.87 threshold=0.42)"
```

---

## File Summary

### New Files Created

| File | Purpose |
|------|---------|
| `model/inference_server.py` | Python inference sidecar server |
| `mbroker/daemon/model_client.c` | C client library for daemon ↔ sidecar IPC |
| `mbroker/include/model_client.h` | C header for model client API |
| `mbroker/etc/systemd/system/mrepod-inference.service` | systemd unit for sidecar |

### Modified Files

| File | Change |
|------|--------|
| `model/vae.py` | Added missing helper functions: `train_vae_model()`, `reconstruction_errors()`, `fit_threshold()`, `is_anomalous()` |
| `mbroker/include/daemon_config.h` | Added `ml_enabled`, `model_socket`, `nfs_servers[]` fields |
| `mbroker/daemon/daemon_config.c` | Added parsing for `ml_enabled`, `model_socket`, `nfs_servers` config keys |
| `mbroker/daemon/mount_ops.c` | Injected ML server selection before NFS `mount()` call |
| `mbroker/daemon/monitor.c` | Added ML anomaly detection after health checks |
| `mbroker/daemon/main.c` | Added ML sidecar health check at startup |
| `mbroker/Makefile` | Added `daemon/model_client.c` to `DAEMON_SRCS` |
| `mbroker/etc/mrepod/mrepod.conf` | Added `ml_enabled`, `model_socket`, `nfs_servers` config keys |

---

## Production Considerations

### Telemetry Collection (TODO)

The current integration passes placeholder zeros for NFS telemetry. For
production, you need to populate the 11-dimension telemetry vector per server
by parsing `/proc/self/mountstats` or using a dedicated collector:

```
Index  Metric
  0    Response time (ms)
  1    Throughput (MB/s)
  2    Error rate (errors/sec)
  3    Connection count
  4    Read operations/sec
  5    Write operations/sec
  6    Read bytes/sec
  7    Write bytes/sec
  8    Retransmit count
  9    Cache hit ratio
 10    Queue depth
```

A future `telemetry_collector.c` module should:
1. Parse `/proc/self/mountstats` periodically (every 10s)
2. Store metrics in a ring buffer per NFS server
3. Expose the latest snapshot to `monitor.c` and `mount_ops.c`

### Model Retraining

Models should be retrained periodically with real production data:
1. Collect mount logs and NFS telemetry over 1-2 weeks
2. Retrain models using the scripts in `model/`
3. Deploy new weight files to `/opt/mrepod/model/`
4. Restart the inference server: `sudo systemctl restart mrepod-inference`

The daemon does **not** need to be restarted — it reconnects per-request.

### Resource Limits

The inference server systemd unit limits resources:
- `CPUQuota=50%` — won't starve mount operations
- `MemoryMax=2G` — caps ML model memory usage

### Disabling ML

Set `ml_enabled = 0` in `mrepod.conf` and restart the daemon.  No code
changes needed.  The inference server can remain running (it just won't
receive queries).
