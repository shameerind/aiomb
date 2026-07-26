# AI-OMB Model Package

Machine learning subsystems for predictive filesystem orchestration in scalable monorepo environments.

## Package Structure

```
model/
├── __init__.py              # Package exports
├── config.py                # Centralized constants and hyperparameters
├── replay_buffer.py         # Shared prioritized experience replay
├── predictor.py             # LSTM-Transformer forecasting model
├── vae.py                   # β-VAE for NFS anomaly detection
├── dqn.py                   # DQN for mount lifecycle policy optimization
├── server_selector.py       # DQN-based NFS server selector + VAE health scorer
├── inference_server.py      # Production inference sidecar (UNIX socket server)
├── train.py                 # Training script for LSTM-Transformer predictor
├── run_dqn.py               # Training script for DQN policy optimizer
├── run_vae.py               # Training script for VAE anomaly detector
├── run_server_selector.py   # Training script for NFS server selector
├── model.py                 # Backward-compatible shim (imports from predictor.py)
├── requirements.txt         # Python dependencies
├── .gitignore               # Excludes weights, bytecode, etc.
└── best_model.pt            # Pre-trained LSTM-Transformer weights
```

## Installation

```bash
cd model
pip install -r requirements.txt
```

**Requirements:**
- Python ≥ 3.9
- PyTorch ≥ 1.10 (LSTM-Transformer predictor)
- TensorFlow ≥ 2.10 (VAE, DQN, server selector)
- NumPy ≥ 1.21
- scikit-learn ≥ 1.0

## Quick Start

### Train all models

```bash
# From the repo root:
python -m model.train                # LSTM-Transformer predictor → best_model.pt
python -m model.run_vae              # VAE anomaly detector → vae_weights.h5
python -m model.run_dqn              # DQN policy optimizer → dqn_weights.h5
python -m model.run_server_selector  # Server selector → selector_weights.h5
```

### Start inference server (production)

```bash
python -m model.inference_server --socket /run/mrepod/model.sock --model-dir model/
```

## Components

### 1. LSTM-Transformer Predictor (`predictor.py`)

Predicts which mount policies will be needed in the next 15–30 minutes for pre-staging.

| Parameter | Value |
|-----------|-------|
| Input | (batch, 288, 16) — 24h window at 5-min intervals |
| Architecture | 2-layer LSTM → 4-head Transformer decoder |
| Output | (batch, 20) — binary logits for mount-policy pairs |
| Loss | BCE with logits |
| Optimizer | Adam (lr=1e-3, weight_decay=1e-5) |

### 2. VAE Anomaly Detector (`vae.py`)

Detects NFS server health anomalies using a β-VAE trained on normal telemetry.

| Parameter | Value |
|-----------|-------|
| Input | 11-dimensional NFS telemetry vector |
| Encoder | 256→128→64→16→latent(8) with BatchNorm |
| Loss | SSE reconstruction + 0.5 × KL divergence |
| Threshold | 99th percentile of training reconstruction errors |

**Telemetry features:** response time, throughput, error rate, connection count,
read/write ops, read/write bytes, retransmits, cache hit ratio, queue depth.

### 3. DQN Policy Optimizer (`dqn.py`)

Reinforcement learning agent that tunes mount lifecycle parameters.

| Parameter | Value |
|-----------|-------|
| State | 18 dimensions (mount counts, storage, queue, latency, health) |
| Actions | 12 discrete (GC threshold, quota, pool size, backoff, prefetch, noop) |
| Replay | Prioritized (α=0.6, β=0.4, capacity=50K) |
| Training | ε-greedy (1.0→0.05), γ=0.99, target sync every 500 steps |

### 4. NFS Server Selector (`server_selector.py`)

Selects the optimal NFS server using VAE health scores + LSTM predicted load.

| Parameter | Value |
|-----------|-------|
| State | 30 dims (8 health + 8 load + 8 connections + 6 policy one-hot) |
| Action | Select 1 of 8 servers |
| Reward | 0.4×latency + 0.35×success + 0.25×balance |

**Production usage:**

```python
from model.server_selector import select_server, VAEHealthScorer
from model.vae import NFSVAE

# Load trained models
vae = NFSVAE(input_dim=11)
vae.load_weights("vae_weights.h5")
scorer = VAEHealthScorer(vae, threshold=0.42)

# Select server
server_idx, q_values = select_server(
    policy_net, scorer,
    server_telemetry,   # (8, 11)
    predicted_load,     # (8,)
    connections,        # (8,)
    policy_onehot,      # (6,)
)
```

## Configuration

All hyperparameters and constants are centralized in `config.py`:

```python
from model.config import (
    PREDICTOR_INPUT_DIM,      # 16
    PREDICTOR_NUM_OUTPUTS,    # 20
    VAE_INPUT_DIM,            # 11
    DQN_STATE_DIM,            # 18
    DQN_N_ACTIONS,            # 12
    DQN_ACTION_NAMES,         # ["increase_gc_threshold", ...]
    SELECTOR_NUM_SERVERS,     # 8
    SELECTOR_NUM_POLICIES,    # 6
)
```

## Integration with mbroker Daemon

See [docs/ML_INTEGRATION.md](../docs/ML_INTEGRATION.md) for the complete guide on
connecting these models to the C daemon via the inference sidecar.

## Replacing Synthetic Data

All training scripts use random/synthetic data. To use real data:

1. **train.py**: Replace `X = np.random.randn(...)` with mount log sequences
2. **run_vae.py**: Replace `x_train = np.random.rand(5000, 11)` with NFS telemetry from `/proc/self/mountstats`
3. **run_dqn.py**: Implement `MountLifecycleEnv.step()` with real system metrics
4. **run_server_selector.py**: Replace `NFSServerEnv.step()` with real NFS latency/failure feedback
