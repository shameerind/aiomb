from server_selector import NFSServerEnv, train_server_selector, select_server, VAEHealthScorer
import numpy as np

# -------------------------------------------------------
# 1. Train the server selector with simulated environment
# -------------------------------------------------------
env = NFSServerEnv(num_servers=8, num_policies=6)

print("Training NFS Server Selector...")
print(f"  Servers: {env.num_servers}")
print(f"  Policies: {env.num_policies}")
print(f"  State dim: {env.state_dim}")
print(f"  Action space: {env.n_actions} (one per server)")
print()

policy_net, target_net = train_server_selector(
    env,
    episodes=500,       # increase for production
    steps_per_episode=100,
    batch_size=64,
    gamma=0.99,
    lr=1e-3,
)

print("\nTraining complete.")

# -------------------------------------------------------
# 2. Test server selection
# -------------------------------------------------------
print("\n--- Test: Server Selection ---")

# Simulate telemetry from 8 servers (11 dims each)
server_telemetry = np.random.rand(8, 11).astype(np.float32)
predicted_load = np.array([0.3, 0.7, 0.2, 0.9, 0.4, 0.1, 0.5, 0.6], dtype=np.float32)
connections = np.array([20, 45, 10, 80, 30, 5, 35, 50], dtype=np.float32)
policy_onehot = np.array([0, 0, 1, 0, 0, 0], dtype=np.float32)  # policy index 2

# Without VAE (use raw scores for demo)
conn_norm = connections / connections.max()
state = np.concatenate([
    server_telemetry.mean(axis=1) * 0.1,  # placeholder health scores
    predicted_load,
    conn_norm,
    policy_onehot,
]).astype(np.float32)

q_vals = policy_net(state.reshape(1, -1)).numpy()[0]
best_server = int(np.argmax(q_vals))

print(f"Q-values per server: {np.round(q_vals, 3)}")
print(f"Selected server: {best_server}")
print(f"  - Predicted load: {predicted_load[best_server]:.2f}")
print(f"  - Connections: {connections[best_server]:.0f}")
