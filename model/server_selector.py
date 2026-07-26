import numpy as np
import random
from collections import deque
import tensorflow as tf
from tensorflow import keras
from tensorflow.keras import layers

from vae import NFSVAE


# ============================================================
#  NFS Server Environment
# ============================================================

class NFSServerEnv:
    """
    Environment for NFS server selection.

    State per decision:
      - VAE anomaly score per server (N values)
      - Predicted load per server from LSTM-Transformer (N values)
      - Active connection count per server (N values)
      - Requested mount policy type (one-hot, P values)
      - Total state dim = 3*N + P

    Action:
      - Select server index (0..N-1)

    Reward:
      - Based on mount latency, success rate, and load balance
    """

    def __init__(self, num_servers=8, num_policies=6):
        self.num_servers = num_servers
        self.num_policies = num_policies
        self.state_dim = 3 * num_servers + num_policies
        self.n_actions = num_servers

        # Per-server tracking
        self.connections = np.zeros(num_servers, dtype=np.float32)
        self.server_load = np.zeros(num_servers, dtype=np.float32)
        self.health_scores = np.zeros(num_servers, dtype=np.float32)

        self.reset()

    def reset(self):
        self.connections = np.random.randint(5, 50, self.num_servers).astype(np.float32)
        self.server_load = np.random.rand(self.num_servers).astype(np.float32) * 0.6
        self.health_scores = np.random.rand(self.num_servers).astype(np.float32) * 0.3
        self.current_policy = np.zeros(self.num_policies, dtype=np.float32)
        self.current_policy[random.randrange(self.num_policies)] = 1.0
        return self._get_state()

    def _get_state(self):
        # Normalize connections to [0, 1]
        conn_norm = self.connections / max(self.connections.max(), 1.0)
        return np.concatenate([
            self.health_scores,   # VAE anomaly scores (lower = healthier)
            self.server_load,     # Predicted load (lower = more capacity)
            conn_norm,            # Normalized connection count
            self.current_policy,  # One-hot policy type
        ])

    def step(self, action):
        """
        Execute mount on selected server, return reward.
        Replace internals with real mount metrics in production.
        """
        server_idx = action

        # Simulate mount latency based on server state
        base_latency = 200.0  # ms baseline
        load_penalty = self.server_load[server_idx] * 600.0
        health_penalty = self.health_scores[server_idx] * 800.0
        conn_penalty = (self.connections[server_idx] / 100.0) * 400.0
        noise = np.random.randn() * 30.0

        latency = base_latency + load_penalty + health_penalty + conn_penalty + noise
        latency = max(latency, 50.0)

        # Simulate failure probability
        fail_prob = (
            0.01
            + 0.15 * self.health_scores[server_idx]
            + 0.08 * self.server_load[server_idx]
        )
        failed = 1.0 if random.random() < fail_prob else 0.0

        # Load balance penalty (std dev of connections)
        self.connections[server_idx] += 1.0
        balance_penalty = np.std(self.connections) / max(np.mean(self.connections), 1.0)

        # Reward: lower latency + no failure + balanced load
        latency_norm = min(latency / 2000.0, 1.0)
        reward = (
            0.4 * (1.0 - latency_norm)
            + 0.35 * (1.0 - failed)
            + 0.25 * (1.0 - min(balance_penalty, 1.0))
        )

        # Evolve environment state for next step
        self._evolve()

        # New mount request
        self.current_policy = np.zeros(self.num_policies, dtype=np.float32)
        self.current_policy[random.randrange(self.num_policies)] = 1.0

        next_state = self._get_state()
        done = False

        info = {"latency_ms": latency, "failed": failed, "server": server_idx}
        return next_state, reward, done, info

    def _evolve(self):
        """Simulate server state changes between requests."""
        # Random connection churn
        self.connections += np.random.randint(-2, 3, self.num_servers).astype(np.float32)
        self.connections = np.clip(self.connections, 0, 200)

        # Load drifts
        self.server_load += np.random.randn(self.num_servers).astype(np.float32) * 0.02
        self.server_load = np.clip(self.server_load, 0, 1)

        # Health scores drift (anomaly can appear)
        self.health_scores += np.random.randn(self.num_servers).astype(np.float32) * 0.01
        self.health_scores = np.clip(self.health_scores, 0, 1)


# ============================================================
#  Server Selector DQN
# ============================================================

class ServerSelectorDQN(keras.Model):
    """Q-network for server selection. Output = Q-value per server."""

    def __init__(self, state_dim, num_servers):
        super().__init__()
        self.d1 = layers.Dense(128, activation="relu")
        self.d2 = layers.Dense(64, activation="relu")
        self.d3 = layers.Dense(32, activation="relu")
        self.out = layers.Dense(num_servers, activation=None)

    def call(self, inputs):
        x = self.d1(inputs)
        x = self.d2(x)
        x = self.d3(x)
        return self.out(x)


# ============================================================
#  Prioritized Replay Buffer
# ============================================================

class ReplayBuffer:
    def __init__(self, capacity=100000, alpha=0.6):
        self.capacity = capacity
        self.alpha = alpha
        self.buffer = []
        self.pos = 0
        self.priorities = np.zeros((capacity,), dtype=np.float32)

    def push(self, transition, priority=1.0):
        max_prio = self.priorities.max() if self.buffer else 1.0
        if len(self.buffer) < self.capacity:
            self.buffer.append(transition)
        else:
            self.buffer[self.pos] = transition
        self.priorities[self.pos] = max(max_prio, priority)
        self.pos = (self.pos + 1) % self.capacity

    def sample(self, batch_size, beta=0.4):
        if len(self.buffer) == self.capacity:
            prios = self.priorities
        else:
            prios = self.priorities[: self.pos]

        probs = prios ** self.alpha
        probs /= probs.sum()

        indices = np.random.choice(len(self.buffer), batch_size, p=probs)
        samples = [self.buffer[idx] for idx in indices]

        total = len(self.buffer)
        weights = (total * probs[indices]) ** (-beta)
        weights /= weights.max()

        states, actions, rewards, next_states, dones = zip(*samples)
        return (
            np.stack(states).astype(np.float32),
            np.array(actions, dtype=np.int32),
            np.array(rewards, dtype=np.float32),
            np.stack(next_states).astype(np.float32),
            np.array(dones, dtype=np.float32),
            indices,
            weights.astype(np.float32),
        )

    def update_priorities(self, indices, priorities):
        for idx, prio in zip(indices, priorities):
            self.priorities[idx] = prio

    def __len__(self):
        return len(self.buffer)


# ============================================================
#  VAE-Informed State Builder
# ============================================================

class VAEHealthScorer:
    """
    Uses a trained VAE to compute per-server anomaly scores.
    Call update() with fresh telemetry each decision cycle.
    """

    def __init__(self, vae_model, threshold):
        self.vae = vae_model
        self.threshold = threshold

    def score_servers(self, server_telemetry):
        """
        Args:
            server_telemetry: np.array of shape (num_servers, 11)
                Each row is the latest 11-dim telemetry for one server.

        Returns:
            anomaly_scores: np.array of shape (num_servers,)
                Normalized reconstruction error (0 = healthy, >1 = anomalous)
        """
        telemetry = tf.cast(server_telemetry, tf.float32)
        x_hat, _, _ = self.vae(telemetry, training=False)
        errors = tf.reduce_sum(tf.square(telemetry - x_hat), axis=1).numpy()
        # Normalize by threshold so score ~1.0 = at threshold boundary
        return errors / max(self.threshold, 1e-6)


# ============================================================
#  Training Loop
# ============================================================

def train_server_selector(
    env,
    episodes=2000,
    steps_per_episode=200,
    batch_size=64,
    gamma=0.99,
    lr=1e-3,
    target_update=1000,
    epsilon_start=1.0,
    epsilon_end=0.05,
    epsilon_decay_steps=5000,
):
    """Train the server selector DQN."""

    state_dim = env.state_dim
    n_actions = env.n_actions

    policy_net = ServerSelectorDQN(state_dim, n_actions)
    target_net = ServerSelectorDQN(state_dim, n_actions)

    # Build networks
    dummy = np.zeros((1, state_dim), dtype=np.float32)
    policy_net(dummy)
    target_net(dummy)
    target_net.set_weights(policy_net.get_weights())

    optimizer = keras.optimizers.Adam(learning_rate=lr)
    buffer = ReplayBuffer(capacity=100000)

    step_count = 0

    def get_epsilon(step):
        frac = min(1.0, step / epsilon_decay_steps)
        return epsilon_start + frac * (epsilon_end - epsilon_start)

    all_rewards = []
    all_latencies = []

    for ep in range(episodes):
        state = env.reset()
        ep_reward = 0.0
        ep_latencies = []

        for _ in range(steps_per_episode):
            step_count += 1
            eps = get_epsilon(step_count)

            if random.random() < eps:
                action = random.randrange(n_actions)
            else:
                q_vals = policy_net(state.reshape(1, -1)).numpy()[0]
                action = int(np.argmax(q_vals))

            next_state, reward, done, info = env.step(action)
            ep_reward += reward
            ep_latencies.append(info["latency_ms"])

            buffer.push((state, action, reward, next_state, float(done)))
            state = next_state

            # Train
            if len(buffer) >= batch_size:
                (
                    states, actions, rewards, next_states, dones, indices, weights
                ) = buffer.sample(batch_size)

                with tf.GradientTape() as tape:
                    q_values = policy_net(states)
                    q_values = tf.reduce_sum(
                        q_values * tf.one_hot(actions, n_actions), axis=1
                    )
                    next_q = target_net(next_states)
                    next_q_max = tf.reduce_max(next_q, axis=1)
                    target = rewards + gamma * next_q_max * (1.0 - dones)

                    td_errors = target - q_values
                    loss = tf.reduce_mean(weights * tf.square(td_errors))

                grads = tape.gradient(loss, policy_net.trainable_variables)
                optimizer.apply_gradients(zip(grads, policy_net.trainable_variables))

                new_prios = np.abs(td_errors.numpy()) + 1e-6
                buffer.update_priorities(indices, new_prios)

            if step_count % target_update == 0:
                target_net.set_weights(policy_net.get_weights())

            if done:
                break

        avg_lat = np.mean(ep_latencies)
        all_rewards.append(ep_reward / steps_per_episode)
        all_latencies.append(avg_lat)

        if (ep + 1) % 50 == 0:
            print(
                f"Episode {ep+1}/{episodes} | "
                f"Avg Reward: {np.mean(all_rewards[-50:]):.4f} | "
                f"Avg Latency: {np.mean(all_latencies[-50:]):.1f} ms | "
                f"ε: {get_epsilon(step_count):.3f}"
            )

    return policy_net, target_net


# ============================================================
#  Production Server Selection
# ============================================================

def select_server(policy_net, vae_scorer, server_telemetry, predicted_load,
                  connections, policy_onehot):
    """
    Select the best NFS server for a mount request.

    Args:
        policy_net: Trained ServerSelectorDQN
        vae_scorer: VAEHealthScorer instance
        server_telemetry: np.array (num_servers, 11) - current telemetry
        predicted_load: np.array (num_servers,) - LSTM-Transformer predicted load
        connections: np.array (num_servers,) - current active connections
        policy_onehot: np.array (num_policies,) - one-hot mount policy

    Returns:
        server_index: int - selected server
        q_values: np.array - Q-values for all servers (for logging)
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
    server_index = int(np.argmax(q_vals))

    return server_index, q_vals
