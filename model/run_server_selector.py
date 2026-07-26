"""Train and test the NFS server selector DQN.

Usage:
    python -m model.run_server_selector
    python model/run_server_selector.py
"""

from __future__ import annotations

import numpy as np

from model.config import SELECTOR_NUM_POLICIES, SELECTOR_NUM_SERVERS, SELECTOR_WEIGHTS_FILE
from model.server_selector import NFSServerEnv, train_server_selector


def main() -> None:
    env = NFSServerEnv(num_servers=SELECTOR_NUM_SERVERS, num_policies=SELECTOR_NUM_POLICIES)

    print("=" * 60)
    print("NFS Server Selector — DQN Training")
    print(f"  Servers:      {env.num_servers}")
    print(f"  Policies:     {env.num_policies}")
    print(f"  State dim:    {env.state_dim}")
    print(f"  Action space: {env.n_actions} (one per server)")
    print("=" * 60)

    policy_net, target_net = train_server_selector(
        env,
        episodes=500,
        steps_per_episode=100,
        batch_size=64,
        verbose=True,
    )

    # Save weights
    policy_net.save_weights(SELECTOR_WEIGHTS_FILE)
    print(f"\nWeights saved to: {SELECTOR_WEIGHTS_FILE}")

    # ── Test inference ──
    print("\n--- Test: Server Selection ---")

    server_telemetry = np.random.rand(SELECTOR_NUM_SERVERS, 11).astype(np.float32)
    predicted_load = np.array([0.3, 0.7, 0.2, 0.9, 0.4, 0.1, 0.5, 0.6], dtype=np.float32)
    connections = np.array([20, 45, 10, 80, 30, 5, 35, 50], dtype=np.float32)
    policy_onehot = np.zeros(SELECTOR_NUM_POLICIES, dtype=np.float32)
    policy_onehot[2] = 1.0  # Policy index 2

    # Build state without VAE (placeholder health scores)
    conn_norm = connections / connections.max()
    state = np.concatenate([
        server_telemetry.mean(axis=1) * 0.1,
        predicted_load,
        conn_norm,
        policy_onehot,
    ]).astype(np.float32)

    q_vals = policy_net(state.reshape(1, -1)).numpy()[0]
    best_server = int(np.argmax(q_vals))

    print(f"Q-values: {np.round(q_vals, 3)}")
    print(f"Selected: server {best_server}")
    print(f"  Load:        {predicted_load[best_server]:.2f}")
    print(f"  Connections: {connections[best_server]:.0f}")


if __name__ == "__main__":
    main()
