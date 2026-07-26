"""Shared replay buffer implementation for DQN-based agents."""

from __future__ import annotations

from typing import Tuple

import numpy as np


class PrioritizedReplayBuffer:
    """Prioritized experience replay buffer for DQN training.

    Uses proportional prioritization (Schaul et al., 2015) to sample
    transitions with probability proportional to their TD-error magnitude.

    Args:
        capacity: Maximum number of transitions stored.
        alpha: Priority exponent — controls how much prioritization is used.
               alpha=0 is uniform, alpha=1 is fully prioritized.
    """

    def __init__(self, capacity: int = 100_000, alpha: float = 0.6) -> None:
        self.capacity = capacity
        self.alpha = alpha
        self.buffer: list = []
        self.pos: int = 0
        self.priorities = np.zeros(capacity, dtype=np.float32)

    def push(self, transition: tuple, priority: float = 1.0) -> None:
        """Store a transition with initial priority."""
        max_prio = self.priorities.max() if self.buffer else 1.0

        if len(self.buffer) < self.capacity:
            self.buffer.append(transition)
        else:
            self.buffer[self.pos] = transition

        self.priorities[self.pos] = max(max_prio, priority)
        self.pos = (self.pos + 1) % self.capacity

    def sample(
        self, batch_size: int, beta: float = 0.4
    ) -> Tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
        """Sample a prioritized mini-batch with importance-sampling weights.

        Args:
            batch_size: Number of transitions to sample.
            beta: Importance-sampling exponent for bias correction.

        Returns:
            Tuple of (states, actions, rewards, next_states, dones, indices, weights).
        """
        n = len(self.buffer)
        prios = self.priorities[:n]

        probs = prios**self.alpha
        probs /= probs.sum()

        indices = np.random.choice(n, batch_size, p=probs)
        samples = [self.buffer[idx] for idx in indices]

        weights = (n * probs[indices]) ** (-beta)
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

    def update_priorities(self, indices: np.ndarray, priorities: np.ndarray) -> None:
        """Update priorities for sampled transitions after training step."""
        for idx, prio in zip(indices, priorities):
            self.priorities[idx] = prio

    def __len__(self) -> int:
        return len(self.buffer)

    @property
    def is_ready(self) -> bool:
        """Whether the buffer has enough samples for a batch."""
        return len(self.buffer) >= 1
