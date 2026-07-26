"""Deep Q-Network for mount lifecycle policy optimization.

Dynamically tunes garbage collection thresholds, storage quotas,
NFS connection pool sizes, and retry backoff parameters using
reinforcement learning with prioritized experience replay.

State:  18-dimensional vector (mount counts, storage, queue, latency, health).
Action: 12 discrete actions (adjust GC/quota/pool/backoff, prefetch, noop).
Reward: Weighted latency + failure rate + storage efficiency + stale ratio.
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
    DQN_ACTION_NAMES,
    DQN_N_ACTIONS,
    DQN_REWARD_WEIGHTS,
    DQN_STATE_DIM,
)
from model.replay_buffer import PrioritizedReplayBuffer


# ─── Environment ──────────────────────────────────────────────────────────────


class MountLifecycleEnv:
    """Simulated environment for mount lifecycle policy optimization.

    Produces synthetic rewards based on random metrics. Replace `step()`
    with real system telemetry integration for production use.

    Attributes:
        state_dim: Observation space dimensionality (18).
        n_actions: Number of discrete actions (12).
    """

    state_dim: int = DQN_STATE_DIM
    n_actions: int = DQN_N_ACTIONS

    def __init__(self) -> None:
        self.state = np.zeros(self.state_dim, dtype=np.float32)

    def reset(self) -> np.ndarray:
        """Reset environment to initial state."""
        self.state = np.zeros(self.state_dim, dtype=np.float32)
        return self.state.copy()

    def step(self, action: int) -> Tuple[np.ndarray, float, bool, dict]:
        """Execute action and return (next_state, reward, done, info).

        TODO: Replace with real system metrics in production.
        """
        next_state = np.random.randn(self.state_dim).astype(np.float32) * 0.1

        w = DQN_REWARD_WEIGHTS
        reward = (
            w["latency"] * (1.0 - np.clip(np.random.rand(), 0, 1))
            + w["failure_rate"] * (1.0 - np.clip(np.random.rand(), 0, 1))
            + w["storage_efficiency"] * np.clip(np.random.rand(), 0, 1)
            + w["stale_ratio"] * (1.0 - np.clip(np.random.rand(), 0, 1))
        )

        self.state = next_state
        return next_state, reward, False, {}


# ─── DQN Network ─────────────────────────────────────────────────────────────


class DQN(keras.Model):
    """Deep Q-Network with 3 hidden layers.

    Args:
        state_dim: Input observation dimensionality.
        n_actions: Number of discrete output actions.
    """

    def __init__(self, state_dim: int = DQN_STATE_DIM, n_actions: int = DQN_N_ACTIONS) -> None:
        super().__init__()
        self.dense1 = layers.Dense(128, activation="relu")
        self.dense2 = layers.Dense(64, activation="relu")
        self.dense3 = layers.Dense(32, activation="relu")
        self.q_out = layers.Dense(n_actions)

    def call(self, inputs: tf.Tensor) -> tf.Tensor:
        """Compute Q-values for all actions given state batch."""
        x = self.dense1(inputs)
        x = self.dense2(x)
        x = self.dense3(x)
        return self.q_out(x)


# ─── Training ────────────────────────────────────────────────────────────────


def train_dqn(
    env: MountLifecycleEnv,
    episodes: int = 1000,
    batch_size: int = DEFAULT_BATCH_SIZE,
    gamma: float = DEFAULT_GAMMA,
    lr: float = DEFAULT_LEARNING_RATE,
    target_update: int = DEFAULT_TARGET_UPDATE_FREQ,
    epsilon_start: float = 1.0,
    epsilon_end: float = 0.05,
    epsilon_decay_steps: int = 864,
    verbose: bool = True,
) -> Tuple[DQN, DQN]:
    """Train a DQN policy with prioritized experience replay.

    Args:
        env: MountLifecycleEnv instance.
        episodes: Number of training episodes.
        batch_size: Mini-batch size for replay sampling.
        gamma: Discount factor.
        lr: Learning rate.
        target_update: Steps between target network syncs.
        epsilon_start: Initial exploration rate.
        epsilon_end: Final exploration rate.
        epsilon_decay_steps: Steps to linearly decay epsilon.
        verbose: Whether to print episode rewards.

    Returns:
        Tuple of (policy_net, target_net).
    """
    policy_net = DQN(env.state_dim, env.n_actions)
    target_net = DQN(env.state_dim, env.n_actions)

    # Initialize network weights
    dummy = np.zeros((1, env.state_dim), dtype=np.float32)
    policy_net(dummy)
    target_net(dummy)
    target_net.set_weights(policy_net.get_weights())

    optimizer = keras.optimizers.Adam(learning_rate=lr)
    buffer = PrioritizedReplayBuffer(capacity=50_000, alpha=0.6)

    step_count = 0

    def get_epsilon(step: int) -> float:
        frac = min(1.0, step / epsilon_decay_steps)
        return epsilon_start + frac * (epsilon_end - epsilon_start)

    for ep in range(episodes):
        state = env.reset()
        ep_reward = 0.0
        done = False

        while not done:
            step_count += 1
            eps = get_epsilon(step_count)

            # ε-greedy action selection
            if random.random() < eps:
                action = random.randrange(env.n_actions)
            else:
                q_vals = policy_net(state.reshape(1, -1)).numpy()[0]
                action = int(np.argmax(q_vals))

            next_state, reward, done, _ = env.step(action)
            ep_reward += reward

            buffer.push((state, action, reward, next_state, done))
            state = next_state

            # Train on mini-batch
            if len(buffer) >= batch_size:
                states, actions, rewards, next_states, dones, indices, weights = (
                    buffer.sample(batch_size, beta=0.4)
                )

                with tf.GradientTape() as tape:
                    q_values = tf.reduce_sum(
                        policy_net(states) * tf.one_hot(actions, env.n_actions),
                        axis=1,
                    )
                    next_q_max = tf.reduce_max(target_net(next_states), axis=1)
                    targets = rewards + gamma * next_q_max * (1.0 - dones)
                    td_errors = targets - q_values
                    loss = tf.reduce_mean(weights * tf.square(td_errors))

                grads = tape.gradient(loss, policy_net.trainable_variables)
                optimizer.apply_gradients(zip(grads, policy_net.trainable_variables))

                buffer.update_priorities(indices, np.abs(td_errors.numpy()) + 1e-6)

            # Sync target network
            if step_count % target_update == 0:
                target_net.set_weights(policy_net.get_weights())

        if verbose:
            print(f"Episode {ep + 1}/{episodes} | Reward: {ep_reward:.4f} | ε: {get_epsilon(step_count):.3f}")

    return policy_net, target_net


# ─── Inference ────────────────────────────────────────────────────────────────


def select_action(policy_net: DQN, state: np.ndarray) -> int:
    """Select the greedy action for a given state.

    Args:
        policy_net: Trained DQN policy network.
        state: State vector of shape (state_dim,).

    Returns:
        Action index with highest Q-value.
    """
    q_vals = policy_net(state.reshape(1, -1)).numpy()[0]
    return int(np.argmax(q_vals))


def get_action_name(action: int) -> str:
    """Get human-readable name for an action index."""
    if 0 <= action < len(DQN_ACTION_NAMES):
        return DQN_ACTION_NAMES[action]
    return "unknown"

