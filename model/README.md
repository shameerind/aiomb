# AI-OMB: AI-Enhanced Overlay Mount Broker

Machine learning modules for predictive filesystem orchestration in scalable monorepo environments.

## Project Structure

| File | Description |
|------|-------------|
| `model.py` | LSTM-Transformer forecasting model (current version) — predicts which mount policies will be needed |
| `model_old.py` | Previous version of the LSTM-Transformer model |
| `train.py` | Training script for the LSTM-Transformer predictor |
| `vae.py` | Variational Autoencoder (VAE) for NFS anomaly detection |
| `run_vae.py` | Runner script to train the VAE and test anomaly detection |
| `dqn.py` | Deep Q-Network (DQN) agent for RL-based policy optimization |
| `run_dqn.py` | Runner script to train the DQN agent |
| `server_selector.py` | DQN-based NFS server selector using VAE health scores + predicted load |
| `run_server_selector.py` | Runner script to train and test the server selector |
| `best_model.pt` | Saved weights for the best LSTM-Transformer model |

## Requirements

```bash
pip install tensorflow numpy torch scikit-learn
```

- **Python** >= 3.8
- **TensorFlow** >= 2.x (used by VAE and DQN modules)
- **PyTorch** >= 1.10 (used by the LSTM-Transformer predictor)
- **NumPy**
- **scikit-learn** (for train/test splitting)

## Running the Scripts

### 1. LSTM-Transformer Predictor (Mount Pre-staging)

Trains a model that predicts which mount policies will be needed in the next 15 minutes.

```bash
python train.py
```

- Input: 288-step sequences (24h of 5-min intervals), 16 features per timestep
- Output: Binary predictions for 20 mount-policy pairs
- Saves best model to `best_model.pt`
- Uses Adam optimizer (lr=1e-3, weight_decay=1e-5), BCE loss, 15 epochs

To run the model standalone (sanity check):

```bash
python model.py
```

### 2. VAE Anomaly Detector (NFS Health Monitoring)

Detects NFS anomalies by learning normal telemetry patterns and flagging high reconstruction errors.

```bash
python run_vae.py
```

- Input: 11-dimensional NFS telemetry vectors (sampled every 10 seconds)
- Architecture: 4-layer encoder (256→128→64→16→latent dim 8) with batch normalization
- Loss: SSE reconstruction + β·KL divergence (β=0.5)
- Threshold: 99th percentile of training reconstruction errors

**Note:** `run_vae.py` imports helper functions (`train_vae_model`, `reconstruction_errors`, `fit_threshold`, `is_anomalous`) that need to be added to `vae.py` before running. Currently only the `NFSVAE` model class is defined.

### 3. DQN Policy Optimizer (Reinforcement Learning)

Optimizes garbage collection, quotas, connection pool, and retry parameters.

```bash
python run_dqn.py
```

- State: 18-dimensional vector (mount counts, storage occupancy, queue length, etc.)
- Actions: 12 discrete options (adjust GC threshold, quotas, pool size, backoff, or no-op)
- Uses prioritized experience replay and ε-greedy exploration (ε: 1.0 → 0.05)
- Reward combines latency, failure rate, storage efficiency, and stale mount ratio

## Replacing Dummy Data

All scripts currently use random/synthetic data. To use real data:

1. **train.py**: Replace `X = np.random.randn(N, seq_len, input_dim)` with actual mount log sequences
2. **run_vae.py**: Replace `x_train = np.random.rand(5000, 11)` with real NFS telemetry
3. **dqn.py**: Implement `MountLifecycleEnv.step()` with actual system metrics and mount lifecycle logic
4. **server_selector.py**: Replace `NFSServerEnv.step()` with real mount latency/failure feedback from your NFS cluster

### 4. NFS Server Selector (Intelligent Server Routing)

Uses VAE health scores + LSTM-Transformer load predictions to select the optimal NFS server for each mount request via a DQN agent.

```bash
python run_server_selector.py
```

- **State** (per mount request):
  - VAE anomaly score per server (8 values) — real-time health
  - Predicted load per server (8 values) — from LSTM-Transformer
  - Normalized connection count per server (8 values)
  - Mount policy type (one-hot, 6 values)
  - Total state dim: 30
- **Action**: Select one of N servers (default N=8)
- **Reward**: Weighted combination of mount latency (0.4), success rate (0.35), and load balance (0.25)

**Production usage:**

```python
from server_selector import select_server, VAEHealthScorer

# With a trained VAE and policy network:
server_idx, q_values = select_server(
    policy_net,
    vae_scorer,
    server_telemetry,   # (8, 11) latest telemetry per server
    predicted_load,     # (8,) from LSTM-Transformer
    connections,        # (8,) active mounts per server
    policy_onehot,      # (6,) which policy is being mounted
)
```
