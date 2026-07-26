"""Shared constants and configuration for the AI-OMB model package."""

from __future__ import annotations

# ─── LSTM-Transformer Predictor ───────────────────────────────────────────────
PREDICTOR_INPUT_DIM = 16
PREDICTOR_NUM_OUTPUTS = 20
PREDICTOR_SEQ_LEN = 288          # 24 hours × 12 samples/hour (5-min intervals)
PREDICTOR_D_MODEL = 128
PREDICTOR_NHEAD = 4
PREDICTOR_NUM_LAYERS = 2
PREDICTOR_DIM_FEEDFORWARD = 256
PREDICTOR_WEIGHTS_FILE = "best_model.pt"

# ─── VAE Anomaly Detector ────────────────────────────────────────────────────
VAE_INPUT_DIM = 11               # NFS telemetry dimensions
VAE_LATENT_DIM = 8
VAE_BETA = 0.5                   # β-VAE KL weighting
VAE_THRESHOLD_PERCENTILE = 99.0
VAE_WEIGHTS_FILE = "vae_weights.h5"
VAE_THRESHOLD_FILE = "vae_threshold.txt"

# NFS telemetry feature names (for documentation)
NFS_TELEMETRY_FEATURES = [
    "response_time_ms",
    "throughput_mbps",
    "error_rate",
    "connection_count",
    "read_ops_per_sec",
    "write_ops_per_sec",
    "read_bytes_per_sec",
    "write_bytes_per_sec",
    "retransmit_count",
    "cache_hit_ratio",
    "queue_depth",
]

# ─── DQN Policy Optimizer ────────────────────────────────────────────────────
DQN_STATE_DIM = 18
DQN_N_ACTIONS = 12
DQN_WEIGHTS_FILE = "dqn_weights.h5"

DQN_ACTION_NAMES = [
    "increase_gc_threshold",
    "decrease_gc_threshold",
    "increase_quota",
    "decrease_quota",
    "increase_pool_size",
    "decrease_pool_size",
    "increase_retry_backoff",
    "decrease_retry_backoff",
    "enable_prefetch",
    "disable_prefetch",
    "force_gc_now",
    "noop",
]

# Reward weights for DQN
DQN_REWARD_WEIGHTS = {
    "latency": 0.30,
    "failure_rate": 0.25,
    "storage_efficiency": 0.25,
    "stale_ratio": 0.20,
}

# ─── Server Selector ─────────────────────────────────────────────────────────
SELECTOR_NUM_SERVERS = 8
SELECTOR_NUM_POLICIES = 6
SELECTOR_WEIGHTS_FILE = "selector_weights.h5"

# Reward weights for server selection
SELECTOR_REWARD_WEIGHTS = {
    "latency": 0.40,
    "success_rate": 0.35,
    "load_balance": 0.25,
}

# ─── Training Defaults ────────────────────────────────────────────────────────
DEFAULT_BATCH_SIZE = 64
DEFAULT_LEARNING_RATE = 1e-3
DEFAULT_GAMMA = 0.99
DEFAULT_EPSILON_START = 1.0
DEFAULT_EPSILON_END = 0.05
DEFAULT_REPLAY_CAPACITY = 100_000
DEFAULT_TARGET_UPDATE_FREQ = 500
