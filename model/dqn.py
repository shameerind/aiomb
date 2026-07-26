import numpy as np
import random
from collections import deque
import tensorflow as tf
from tensorflow import keras
from tensorflow.keras import layers


# ============================================================
#  Environment Stub (replace with real AI‑OMB lifecycle logic)
# ============================================================

class MountLifecycleEnv:
    """
    State dim = 18 (as in the paper)
    Action space = 12 discrete actions
    Replace step() with real GC/quota/backoff/pool-size logic
    """
    def __init__(self):
        self.state_dim = 18
        self.n_actions = 12
        self.reset()

    def reset(self):
        self.state = np.zeros(self.state_dim, dtype=np.float32)
        return self.state

    def step(self, action):
        # TODO: Replace with real system metrics
        next_state = np.random.randn(self.state_dim).astype(np.float32) * 0.1

        lat_norm = np.clip(np.random.rand(), 0, 1)
        fail_rate = np.clip(np.random.rand(), 0, 1)
        stor_eff = np.clip(np.random.rand(), 0, 1)
        stale_ratio = np.clip(np.random.rand(), 0, 1)

        reward = (
            0.3 * (1 - lat_norm)
            + 0.25 * (1 - fail_rate)
            + 0.25 * stor_eff
            + 0.2 * (1 - stale_ratio)
        )

        done = False
        self.state = next_state
        return next_state, reward, done, {}


# ============================================================
#  DQN Network
# ============================================================

class DQN(keras.Model):
    def __init__(self, state_dim=18, n_actions=12):
        super().__init__()
        self.d1 = layers.Dense(128, activation="relu")
        self.d2 = layers.Dense(64, activation="relu")
        self.d3 = layers.Dense(32, activation="relu")
        self.out = layers.Dense(n_actions, activation=None)

    def call(self, inputs):
        x = self.d1(inputs)
        x = self.d2(x)
        x = self.d3(x)
        return self.out(x)


# ============================================================
#  Prioritized Replay Buffer
# ============================================================

class PrioritizedReplayBuffer:
    def __init__(self, capacity=50000, alpha=0.6):
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
            prios = self.priorities[:self.pos]

        probs = prios ** self.alpha
        probs /= probs.sum()

        indices = np.random.choice(len(self.buffer), batch_size, p=probs)
        samples = [self.buffer[idx] for idx in indices]

        total = len(self.buffer)
        weights = (total * probs[indices]) ** (-beta)
        weights /= weights.max()
        weights = weights.astype(np.float32)

        states, actions, rewards, next_states, dones = zip(*samples)

        return (
            np.stack(states).astype(np.float32),
            np.array(actions, dtype=np.int32),
            np.array(rewards, dtype=np.float32),
            np.stack(next_states).astype(np.float32),
            np.array(dones, dtype=np.float32),
            indices,
            weights,
        )

    def update_priorities(self, indices, priorities):
        for idx, prio in zip(indices, priorities):
            self.priorities[idx] = prio

    def __len__(self):
        return len(self.buffer)


# ============================================================
#  DQN Training Loop
# ============================================================

def train_dqn(
    env,
    episodes=1000,
    batch_size=64,
    gamma=0.99,
    lr=1e-3,
    target_update=500,
):
    state_dim = env.state_dim
    n_actions = env.n_actions

    policy_net = DQN(state_dim, n_actions)
    target_net = DQN(state_dim, n_actions)

    # Build networks
    dummy = np.zeros((1, state_dim), dtype=np.float32)
    policy_net(dummy)
    target_net(dummy)
    target_net.set_weights(policy_net.get_weights())

    optimizer = keras.optimizers.Adam(learning_rate=lr)
    buffer = PrioritizedReplayBuffer(capacity=50000, alpha=0.6)

    epsilon_start, epsilon_end, epsilon_decay_steps = 1.0, 0.05, 72 * 12
    step_count = 0

    def epsilon_by_step(step):
        frac = min(1.0, step / epsilon_decay_steps)
        return epsilon_start + frac * (epsilon_end - epsilon_start)

    for ep in range(episodes):
        state = env.reset()
        ep_reward = 0.0
        done = False

        while not done:
            step_count += 1
            eps = epsilon_by_step(step_count)

            if random.random() < eps:
                action = random.randrange(n_actions)
            else:
                q_vals = policy_net(state.reshape(1, -1)).numpy()[0]
                action = int(np.argmax(q_vals))

            next_state, reward, done, _ = env.step(action)
            ep_reward += reward

            buffer.push((state, action, reward, next_state, done))
            state = next_state

            if len(buffer) >= batch_size:
                (
                    states,
                    actions,
                    rewards,
                    next_states,
                    dones,
                    indices,
                    weights,
                ) = buffer.sample(batch_size, beta=0.4)

                with tf.GradientTape() as tape:
                    q_values = policy_net(states)
                    q_values = tf.reduce_sum(
                        q_values * tf.one_hot(actions, n_actions), axis=1
                    )

                    next_q_values = target_net(next_states)
                    next_q_max = tf.reduce_max(next_q_values, axis=1)
                    target = rewards + gamma * next_q_max * (1.0 - dones)

                    td_errors = target - q_values
                    loss = tf.reduce_mean(weights * tf.square(td_errors))

                grads = tape.gradient(loss, policy_net.trainable_variables)
                optimizer.apply_gradients(zip(grads, policy_net.trainable_variables))

                new_prios = np.abs(td_errors.numpy()) + 1e-6
                buffer.update_priorities(indices, new_prios)

            if step_count % target_update == 0:
                target_net.set_weights(policy_net.get_weights())

        print(f"Episode {ep+1}: reward={ep_reward:.4f}")

    return policy_net, target_net


# ============================================================
#  Action Selection for Production
# ============================================================

def select_action(policy_net, state):
    q_vals = policy_net(state.reshape(1, -1)).numpy()[0]
    return int(np.argmax(q_vals))

