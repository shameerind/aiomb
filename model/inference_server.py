#!/usr/bin/env python3
"""
AI-OMB Model Inference Sidecar Server

A lightweight UNIX-socket server that loads all ML models and serves
inference requests from the mrepod daemon. The daemon sends JSON requests
and receives JSON responses over /run/mrepod/model.sock.

Protocol:
  - Client sends a 4-byte big-endian length prefix followed by JSON payload.
  - Server responds with the same framing.

Commands:
  select_server   — Select optimal NFS server for a mount request
  check_anomaly   — Check NFS telemetry for anomalies via VAE
  predict_load    — Predict future mount load via LSTM-Transformer
  optimize_policy — Get DQN policy action for lifecycle tuning
  health          — Check if the inference server is alive
"""

import argparse
import json
import logging
import os
import signal
import socket
import struct
import sys
import threading
import traceback

import numpy as np

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [inference] %(levelname)s %(message)s",
    datefmt="%Y-%m-%dT%H:%M:%S",
)
log = logging.getLogger("inference")

# ---------------------------------------------------------------------------
#  Model Loader
# ---------------------------------------------------------------------------

class ModelRegistry:
    """Loads and holds all ML model instances."""

    def __init__(self, model_dir):
        self.model_dir = model_dir
        self.predictor = None        # LSTM-Transformer (PyTorch)
        self.vae = None              # NFSVAE (TensorFlow/Keras)
        self.vae_threshold = None    # float
        self.dqn = None              # DQN policy net (TensorFlow/Keras)
        self.server_selector = None  # ServerSelectorDQN (TensorFlow/Keras)
        self.loaded = {}

    def load_predictor(self):
        """Load LSTM-Transformer mount predictor."""
        weights_path = os.path.join(self.model_dir, "best_model.pt")
        if not os.path.isfile(weights_path):
            log.warning("Predictor weights not found at %s — skipping", weights_path)
            return
        try:
            import torch
            sys.path.insert(0, self.model_dir)
            from model import LSTMTransformerForecast
            m = LSTMTransformerForecast(input_dim=16, num_outputs=20)
            m.load_state_dict(torch.load(weights_path, map_location="cpu", weights_only=True))
            m.eval()
            self.predictor = m
            self.loaded["predictor"] = weights_path
            log.info("Loaded predictor from %s", weights_path)
        except Exception as e:
            log.error("Failed to load predictor: %s", e)

    def load_vae(self):
        """Load VAE anomaly detector."""
        weights_path = os.path.join(self.model_dir, "vae_weights.h5")
        threshold_path = os.path.join(self.model_dir, "vae_threshold.txt")
        if not os.path.isfile(weights_path):
            log.warning("VAE weights not found at %s — skipping", weights_path)
            return
        try:
            sys.path.insert(0, self.model_dir)
            from vae import NFSVAE
            import tensorflow as tf
            vae = NFSVAE(input_dim=11)
            # Build with dummy input
            vae(tf.zeros((1, 11)))
            vae.load_weights(weights_path)
            self.vae = vae

            if os.path.isfile(threshold_path):
                with open(threshold_path) as f:
                    self.vae_threshold = float(f.read().strip())
            else:
                self.vae_threshold = 1.0  # fallback
                log.warning("VAE threshold file not found; using default %.2f",
                            self.vae_threshold)

            self.loaded["vae"] = weights_path
            log.info("Loaded VAE from %s (threshold=%.4f)",
                     weights_path, self.vae_threshold)
        except Exception as e:
            log.error("Failed to load VAE: %s", e)

    def load_dqn(self):
        """Load DQN lifecycle policy optimizer."""
        weights_path = os.path.join(self.model_dir, "dqn_weights.h5")
        if not os.path.isfile(weights_path):
            log.warning("DQN weights not found at %s — skipping", weights_path)
            return
        try:
            sys.path.insert(0, self.model_dir)
            from dqn import DQN
            import tensorflow as tf
            net = DQN(state_dim=18, n_actions=12)
            net(tf.zeros((1, 18)))
            net.load_weights(weights_path)
            self.dqn = net
            self.loaded["dqn"] = weights_path
            log.info("Loaded DQN from %s", weights_path)
        except Exception as e:
            log.error("Failed to load DQN: %s", e)

    def load_server_selector(self):
        """Load NFS server selector DQN."""
        weights_path = os.path.join(self.model_dir, "selector_weights.h5")
        if not os.path.isfile(weights_path):
            log.warning("Server selector weights not found at %s — skipping",
                        weights_path)
            return
        try:
            sys.path.insert(0, self.model_dir)
            from server_selector import ServerSelectorDQN
            import tensorflow as tf
            # Default: 8 servers, 6 policies → state_dim = 3*8+6 = 30
            net = ServerSelectorDQN(state_dim=30, num_servers=8)
            net(tf.zeros((1, 30)))
            net.load_weights(weights_path)
            self.server_selector = net
            self.loaded["server_selector"] = weights_path
            log.info("Loaded server selector from %s", weights_path)
        except Exception as e:
            log.error("Failed to load server selector: %s", e)

    def load_all(self):
        """Attempt to load every model. Missing weights are non-fatal."""
        self.load_predictor()
        self.load_vae()
        self.load_dqn()
        self.load_server_selector()
        log.info("Model loading complete — %d/%d available: %s",
                 len(self.loaded), 4, list(self.loaded.keys()))


# ---------------------------------------------------------------------------
#  Request Handlers
# ---------------------------------------------------------------------------

def handle_select_server(registry, params):
    """Select the best NFS server.

    Expected params:
      telemetry: list of lists (num_servers × 11)  — NFS metrics
      predicted_load: list (num_servers,)           — from LSTM predictor
      connections: list (num_servers,)              — active connections
      policy_index: int                             — mount policy type
      num_policies: int (default 6)
    """
    if registry.server_selector is None:
        return {"error": "server_selector model not loaded", "fallback": True}

    telemetry = np.array(params["telemetry"], dtype=np.float32)
    predicted_load = np.array(params["predicted_load"], dtype=np.float32)
    connections = np.array(params["connections"], dtype=np.float32)
    num_servers = len(connections)
    num_policies = params.get("num_policies", 6)

    policy_onehot = np.zeros(num_policies, dtype=np.float32)
    pi = params.get("policy_index", 0)
    if 0 <= pi < num_policies:
        policy_onehot[pi] = 1.0

    # Compute health scores from VAE if available
    if registry.vae is not None and telemetry.shape == (num_servers, 11):
        import tensorflow as tf
        x = tf.cast(telemetry, tf.float32)
        x_hat, _, _ = registry.vae(x, training=False)
        errors = tf.reduce_sum(tf.square(x - x_hat), axis=1).numpy()
        health_scores = errors / max(registry.vae_threshold, 1e-6)
    else:
        health_scores = np.zeros(num_servers, dtype=np.float32)

    conn_norm = connections / max(connections.max(), 1.0)
    state = np.concatenate([health_scores, predicted_load, conn_norm,
                            policy_onehot]).astype(np.float32)

    q_vals = registry.server_selector(state.reshape(1, -1)).numpy()[0]
    best = int(np.argmax(q_vals))

    return {
        "server_index": best,
        "q_values": q_vals.tolist(),
        "health_scores": health_scores.tolist(),
    }


def handle_check_anomaly(registry, params):
    """Check NFS telemetry for anomalies.

    Expected params:
      nfs_metrics: list of lists (num_servers × 11)
    """
    if registry.vae is None:
        return {"error": "vae model not loaded", "fallback": True}

    import tensorflow as tf
    metrics = np.array(params["nfs_metrics"], dtype=np.float32)
    x = tf.cast(metrics, tf.float32)
    x_hat, _, _ = registry.vae(x, training=False)
    errors = tf.reduce_sum(tf.square(x - x_hat), axis=1).numpy()
    threshold = registry.vae_threshold

    results = []
    for i, err in enumerate(errors):
        results.append({
            "server_index": i,
            "error": float(err),
            "threshold": float(threshold),
            "anomalous": bool(err > threshold),
        })

    return {
        "servers": results,
        "any_anomalous": any(r["anomalous"] for r in results),
    }


def handle_predict_load(registry, params):
    """Predict upcoming mount load via LSTM-Transformer.

    Expected params:
      history: list of lists (288 × 16) — 24-hour feature window
    """
    if registry.predictor is None:
        return {"error": "predictor model not loaded", "fallback": True}

    import torch
    history = np.array(params["history"], dtype=np.float32)
    if history.ndim == 2:
        history = history[np.newaxis, :]  # add batch dim

    with torch.no_grad():
        x = torch.tensor(history, dtype=torch.float32)
        logits = registry.predictor(x)
        predictions = (torch.sigmoid(logits) > 0.5).int().squeeze(0).tolist()

    return {"predictions": predictions}


def handle_optimize_policy(registry, params):
    """Get DQN action for mount lifecycle tuning.

    Expected params:
      state: list (18,) — lifecycle state vector
    """
    if registry.dqn is None:
        return {"error": "dqn model not loaded", "fallback": True}

    state = np.array(params["state"], dtype=np.float32).reshape(1, -1)
    q_vals = registry.dqn(state).numpy()[0]
    action = int(np.argmax(q_vals))

    ACTION_NAMES = [
        "increase_gc_threshold", "decrease_gc_threshold",
        "increase_quota", "decrease_quota",
        "increase_pool_size", "decrease_pool_size",
        "increase_retry_backoff", "decrease_retry_backoff",
        "enable_prefetch", "disable_prefetch",
        "force_gc_now", "noop",
    ]

    return {
        "action": action,
        "action_name": ACTION_NAMES[action] if action < len(ACTION_NAMES) else "unknown",
        "q_values": q_vals.tolist(),
    }


HANDLERS = {
    "select_server": handle_select_server,
    "check_anomaly": handle_check_anomaly,
    "predict_load": handle_predict_load,
    "optimize_policy": handle_optimize_policy,
}


# ---------------------------------------------------------------------------
#  Wire Protocol: 4-byte big-endian length prefix + JSON payload
# ---------------------------------------------------------------------------

def recv_message(conn):
    """Read a length-prefixed JSON message from the socket."""
    header = b""
    while len(header) < 4:
        chunk = conn.recv(4 - len(header))
        if not chunk:
            return None
        header += chunk
    length = struct.unpack(">I", header)[0]
    if length > 4 * 1024 * 1024:  # 4 MB hard limit
        raise ValueError(f"Message too large: {length} bytes")
    data = b""
    while len(data) < length:
        chunk = conn.recv(min(length - len(data), 65536))
        if not chunk:
            raise ConnectionError("Connection closed mid-message")
        data += chunk
    return json.loads(data.decode("utf-8"))


def send_message(conn, obj):
    """Send a length-prefixed JSON message over the socket."""
    payload = json.dumps(obj).encode("utf-8")
    conn.sendall(struct.pack(">I", len(payload)) + payload)


# ---------------------------------------------------------------------------
#  Server Loop
# ---------------------------------------------------------------------------

def handle_connection(conn, registry):
    """Handle one client connection (one request-response cycle)."""
    try:
        request = recv_message(conn)
        if request is None:
            return

        cmd = request.get("cmd", "")
        if cmd == "health":
            send_message(conn, {
                "status": "ok",
                "models_loaded": list(registry.loaded.keys()),
            })
            return

        handler = HANDLERS.get(cmd)
        if handler is None:
            send_message(conn, {"error": f"unknown command: {cmd}"})
            return

        params = request.get("params", {})
        result = handler(registry, params)
        send_message(conn, result)

    except Exception as e:
        log.error("Error handling request: %s\n%s", e, traceback.format_exc())
        try:
            send_message(conn, {"error": str(e)})
        except Exception:
            pass
    finally:
        conn.close()


def run_server(sock_path, registry, max_threads=4):
    """Main server loop listening on a UNIX socket."""

    # Clean up stale socket
    if os.path.exists(sock_path):
        os.unlink(sock_path)

    sock_dir = os.path.dirname(sock_path)
    if sock_dir and not os.path.isdir(sock_dir):
        os.makedirs(sock_dir, mode=0o755, exist_ok=True)

    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(sock_path)
    os.chmod(sock_path, 0o666)
    server.listen(16)
    server.settimeout(1.0)

    log.info("Inference server listening on %s", sock_path)

    running = True

    def stop(signum, frame):
        nonlocal running
        running = False

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)

    semaphore = threading.Semaphore(max_threads)

    def worker(conn):
        try:
            handle_connection(conn, registry)
        finally:
            semaphore.release()

    while running:
        try:
            conn, _ = server.accept()
        except socket.timeout:
            continue
        except OSError:
            if running:
                log.error("Accept failed", exc_info=True)
            break

        semaphore.acquire()
        t = threading.Thread(target=worker, args=(conn,), daemon=True)
        t.start()

    server.close()
    if os.path.exists(sock_path):
        os.unlink(sock_path)
    log.info("Inference server stopped")


# ---------------------------------------------------------------------------
#  Entry Point
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="AI-OMB Model Inference Server")
    parser.add_argument("--socket", default="/run/mrepod/model.sock",
                        help="UNIX socket path (default: /run/mrepod/model.sock)")
    parser.add_argument("--model-dir", default=None,
                        help="Directory containing model weights "
                             "(default: same directory as this script)")
    parser.add_argument("--max-threads", type=int, default=4,
                        help="Max concurrent inference threads (default: 4)")
    args = parser.parse_args()

    model_dir = args.model_dir or os.path.dirname(os.path.abspath(__file__))

    registry = ModelRegistry(model_dir)
    registry.load_all()

    run_server(args.socket, registry, max_threads=args.max_threads)


if __name__ == "__main__":
    main()
