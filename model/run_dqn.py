"""Train the DQN mount lifecycle policy optimizer.

Usage:
    python -m model.run_dqn
    python model/run_dqn.py
"""

from __future__ import annotations

from model.config import DQN_WEIGHTS_FILE
from model.dqn import MountLifecycleEnv, get_action_name, select_action, train_dqn


def main() -> None:
    env = MountLifecycleEnv()

    print("=" * 60)
    print("DQN Mount Lifecycle Policy Optimizer")
    print(f"  State dim:  {env.state_dim}")
    print(f"  Actions:    {env.n_actions}")
    print("=" * 60)

    policy_net, target_net = train_dqn(
        env,
        episodes=100,
        batch_size=32,
        verbose=True,
    )

    # Save trained weights
    policy_net.save_weights(DQN_WEIGHTS_FILE)
    print(f"\nWeights saved to: {DQN_WEIGHTS_FILE}")

    # Quick inference test
    state = env.reset()
    action = select_action(policy_net, state)
    print(f"\nTest inference: action={action} ({get_action_name(action)})")


if __name__ == "__main__":
    main()

