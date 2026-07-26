"""DQN-based NFS server selector with VAE health scoring.

Selects the optimal NFS server for each mount request by combining:
  - Real-time health anomaly scores from the VAE
  - Predicted future load from the LSTM-Transformer
  - Current connection distribution across the server pool

State:  3×N_servers + N_policies dimensions.
Action: Select one of N servers.
Reward: Weighted mount latency + success rate + load balance.
"""

from __future__ import annotations

import random
from typing import Tuple

import numpy as np
import tensorflow as tf
from tensorflow import keras
from tensorflow.keras import layers

from model.config import (
    DEFAULT_BATCH_SIZE,
    DEFAULT_GAMMA,
    DEFAULT_LEARNING_RATE,
    DEFAULT_TARGET_UPDATE_FREQ,
    SELECTOR_NUM_POLICIES,
    SELECTOR_NUM_SERVERS,
    SELECTOR_REWARD_WEIGHTS,
)
from model.replay_buffer import PrioritizedReplayBuffer
from model.vae import NFSVAE


# ─── Environment ──────────────────────────────────────────────────────────────


class NFSServerEnv:
    """Simulated NFS server pool environment for server selection training.

    Models a cluster of NFS servers with evolving load, health, and
    connection states. Produces rewards based on simulated mount latency,
    failure probability, and load balance.

    Args:
        num_servers: Number of NFS servers in the pool.
        num_policies: Number of mount policy types.
    """

    def __init__(
        self,
        num_servers: int = SELECTOR_NUM_SERVERS,
        num_policies: int = SELECTOR_NUM_POLICIES,
    ) -> None:
        self.num_servers = num_servers
        self.num_policies = num_policies
        self.state_dim = 3 * num_servers + num_policies
        self.n_actions = num_servers

        self.connections = np.zeros(num_servers, dtype=np.float32)
        self.server_load = np.zeros(num_servers, dtype=np.float32)
        self.health_scores = np.zeros(num_servers, dtype=np.float32)
        self.current_policy = np.zeros(num_policies, dtype=np.float32)

    def reset(self) -> np.ndarray:
        """Reset environment to randomized initial state."""
        self.connections = np.random.randint(5, 50, self.num_servers).astype(np.float32)
        self.server_load = np.random.rand(self.num_servers).astype(np.float32) * 0.6
        self.health_scores = np.random.rand(self.num_servers).astype(np.float32) * 0.3
        self.current_policy = np.zeros(self.num_policies, dtype=np.float32)
        self.current_policy[random.randrange(self.num_policies)] = 1.0
        return self._get_state()

    def _get_state(self) -> np.ndarray:
        conn_norm = self.connections / max(self.connections.max(), 1.0)
        return np.concatenate([
            self.health_scores,
            self.server_load,
            conn_norm,
            self.current_policy,
        ])

    def step(self, action: int) -> Tuple[np.ndarray, float, bool, dict]:
        """Execute server selection and compute reward.

        Args:
            action: Server index to route mount request to.

        Returns:
            Tuple of (next_state, reward, done, info).
        """
        idx = action

        # Simulate mount latency
        base_latency = 200.0
        latency = (
            base_latency
            + self.server_load[idx] * 600.0
            + self.health_scores[idx] * 800.0
            + (self.connections[idx] / 100.0) * 400.0
            + np.random.randn() * 30.0
        )
        latency = max(latency, 50.0)

        # Simulate failure
        fail_prob = 0.01 + 0.15 * self.health_scores[idx] + 0.08 * self.server_load[idx]
        failed = 1.0 if random.random() < fail_prob else 0.0

        # Update connections and compute balance penalty
        self.connections[idx] += 1.0
        balance_penalty = np.std(self.connections) / max(np.mean(self.connections), 1.0)

        # Compute reward
        w = SELECTOR_REWARD_WEIGHTS
        latency_norm = min(latency / 2000.0, 1.0)
        reward = (
            w["latency"] * (1.0 - latency_norm)
            + w["success_rate"] * (1.0 - failed)
            + w["load_balance"] * (1.0 - min(balance_penalty, 1.0))
        )

        self._evolve()

        # New mount request with random policy
        self.current_policy = np.zeros(self.num_policies, dtype=np.float32)
        self.current_policy[random.randrange(self.num_policies)] = 1.0

        info = {"latency_ms": latency, "failed": failed, "server": idx}
        return self._get_state(), reward, False, info

    def _evolve(self) -> None:
        """Simulate natural state drift between requests."""
        self.connections += np.random.randint(-2, 3, self.num_servers).astype(np.float32)
        self.connections = np.clip(self.connections, 0, 200)
        self.server_load += np.random.randn(self.num_servers).astype(np.float32) * 0.02
        self.server_load = np.clip(self.server_load, 0, 1)
        self.health_scores += np.random.randn(self.num_servers).astype(np.float32) * 0.01
        self.health_scores = np.clip(self.health_scores, 0, 1)


# ─── Q-Network ───────────────────────────────────────────────────────────────


class ServerSelectorDQN(keras.Model):
    """Q-network for NFS server selection.

    Args:
        state_dim: Input observation dimensionality.
        num_servers: Number of output actions (one per server).
    """

    def __init__(self, state_dim: int, num_servers: int) -> None:
        super().__init__()
        self.dense1 = layers.Dense(128, activation="relu")
        self.dense2 = layers.Dense(64, activation="relu")
        self.dense3 = layers.Dense(32, activation="relu")
        self.q_out = layers.Dense(num_servers)

    def call(self, inputs: tf.Tensor) -> tf.Tensor:
        """Compute Q-values for each server."""
        x = self.dense1(inputs)
        x = self.dense2(x)
        x = self.dense3(x)
        return self.q_out(x)


# ─── VAE Health Scorer ────────────────────────────────────────────────────────


class VAEHealthScorer:
    """Computes per-server health scores using a trained VAE.

    Transforms raw NFS telemetry into normalized anomaly scores where
    0 = perfectly healthy and >1 = beyond anomaly threshold.

    Args:
        vae_model: Trained NFSVAE instance.
        threshold: Anomaly threshold from fit_threshold().
    """

    def __init__(self, vae_model: NFSVAE, threshold: float) -> None:
        self.vae = vae_model
        self.threshold = threshold

    def score_servers(self, server_telemetry: np.ndarray) -> np.ndarray:
        """Compute normalized anomaly scores for each server.

        Args:
            server_telemetry: Array of shape (num_servers, 11).

        Returns:
            Array of shape (num_servers,) — scores normalized by threshold.
        """
        x = tf.cast(server_telemetry, tf.float32)
        x_hat, _, _ = self.vae(x, training=False)
        errors = tf.reduce_sum(tf.square(x - x_hat), axis=1).numpy()
        return errors / max(self.threshold, 1e-6)


# ─── Training ────────────────────────────────────────────────────────────────


def train_server_selector(
    env: NFSServerEnv,
    episodes: int = 2000,
    steps_per_episode: int = 200,
    batch_size: int = DEFAULT_BATCH_SIZE,
    gamma: float = DEFAULT_GAMMA,
    lr: float = DEFAULT_LEARNING_RATE,
    target_update: int = DEFAULT_TARGET_UPDATE_FREQ * 2,
    epsilon_start: float = 1.0,
    epsilon_end: float = 0.05,
    epsilon_decay_steps: int = 5000,
    verbose: bool = True,
) -> Tuple[ServerSelectorDQN, ServerSelectorDQN]:
    """Train the server selector DQN with prioritized replay.

    Args:
        env: NFSServerEnv instance.
        episodes: Number of training episodes.
        steps_per_episode: Max steps per episode.
        batch_size: Replay sampling batch size.
        gamma: Discount factor.
        lr: Learning rate.
        target_update: Steps between target network syncs.
        epsilon_start: Initial exploration rate.
        epsilon_end: Final exploration rate.
        epsilon_decay_steps: Linear decay duration.
        verbose: Print training progress every 50 episodes.

    Returns:
        Tuple of (policy_net, target_net).
    """
    policy_net = ServerSelectorDQN(env.state_dim, env.n_actions)
    target_net = ServerSelectorDQN(env.state_dim, env.n_actions)

    dummy = np.zeros((1, env.state_dim), dtype=np.float32)
    policy_net(dummy)
    target_net(dummy)
    target_net.set_weights(policy_net.get_weights())

    optimizer = keras.optimizers.Adam(learning_rate=lr)
    buffer = PrioritizedReplayBuffer(capacity=100_000)

    step_count = 0
    all_rewards: list[float] = []
    all_latencies: list[float] = []

    def get_epsilon(step: int) -> float:
        frac = min(1.0, step / epsilon_decay_steps)
        return epsilon_start + frac * (epsilon_end - epsilon_start)

    for ep in range(episodes):
        state = env.reset()
        ep_reward = 0.0
        ep_latencies: list[float] = []

        for _ in range(steps_per_episode):
            step_count += 1
            eps = get_epsilon(step_count)

            if random.random() < eps:
                action = random.randrange(env.n_actions)
            else:
                q_vals = policy_net(state.reshape(1, -1)).numpy()[0]
                action = int(np.argmax(q_vals))

            next_state, reward, done, info = env.step(action)
            ep_reward += reward
            ep_latencies.append(info["latency_ms"])

            buffer.push((state, action, reward, next_state, float(done)))
            state = next_state

            if len(buffer) >= batch_size:
                states, actions, rewards_b, next_states, dones, indices, weights = (
                    buffer.sample(batch_size)
                )

                with tf.GradientTape() as tape:
                    q_values = tf.reduce_sum(
                        policy_net(states) * tf.one_hot(actions, env.n_actions),
                        axis=1,
                    )
                    next_q_max = tf.reduce_max(target_net(next_states), axis=1)
                    targets = rewards_b + gamma * next_q_max * (1.0 - dones)
                    td_errors = targets - q_values
                    loss = tf.reduce_mean(weights * tf.square(td_errors))

                grads = tape.gradient(loss, policy_net.trainable_variables)
                optimizer.apply_gradients(zip(grads, policy_net.trainable_variables))
                buffer.update_priorities(indices, np.abs(td_errors.numpy()) + 1e-6)

            if step_count % target_update == 0:
                target_net.set_weights(policy_net.get_weights())

            if done:
                break

        all_rewards.append(ep_reward / steps_per_episode)
        all_latencies.append(np.mean(ep_latencies))

        if verbose and (ep + 1) % 50 == 0:
            print(
                f"Episode {ep + 1}/{episodes} | "
                f"Avg Reward: {np.mean(all_rewards[-50:]):.4f} | "
                f"Avg Latency: {np.mean(all_latencies[-50:]):.1f} ms | "
                f"ε: {get_epsilon(step_count):.3f}"
            )

    return policy_net, target_net


# ─── Production Inference ─────────────────────────────────────────────────────


def select_server(
    policy_net: ServerSelectorDQN,
    vae_scorer: VAEHealthScorer,
    server_telemetry: np.ndarray,
    predicted_load: np.ndarray,
    connections: np.ndarray,
    policy_onehot: np.ndarray,
) -> Tuple[int, np.ndarray]:
    """Select the best NFS server for a mount request.

    Args:
        policy_net: Trained ServerSelectorDQN.
        vae_scorer: VAEHealthScorer with loaded VAE model.
        server_telemetry: Array (num_servers, 11) of current telemetry.
        predicted_load: Array (num_servers,) of LSTM-predicted load.
        connections: Array (num_servers,) of active mount connections.
        policy_onehot: Array (num_policies,) one-hot encoding of policy type.

    Returns:
        Tuple of (server_index, q_values_array).
    """
    health_scores = vae_scorer.score_servers(server_telemetry)
    conn_norm = connections / max(connections.max(), 1.0)

    state = np.concatenate([
        health_scores,
        predicted_load,
        conn_norm,
        policy_onehot,
    ]).astype(np.float32)

    q_vals = policy_net(state.reshape(1, -1)).numpy()[0]
    return int(np.argmax(q_vals)), q_vals
