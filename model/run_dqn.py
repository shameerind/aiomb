from dqn import MountLifecycleEnv, train_dqn

env = MountLifecycleEnv()

print("Entering training loop...")
policy_net, target_net = train_dqn(
    env,
    episodes=10,   # increase later
    batch_size=32
)

print("Training complete.")

