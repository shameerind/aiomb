"""Train the VAE NFS anomaly detector and test anomaly detection.

Usage:
    python -m model.run_vae
    python model/run_vae.py
"""

from __future__ import annotations

import numpy as np

from model.config import VAE_THRESHOLD_FILE, VAE_WEIGHTS_FILE
from model.vae import fit_threshold, is_anomalous, reconstruction_errors, train_vae_model


def main() -> None:
    print("=" * 60)
    print("NFS VAE Anomaly Detector — Training & Evaluation")
    print("=" * 60)

    # Training data (replace with real NFS telemetry)
    x_train = np.random.rand(5000, 11).astype(np.float32)

    # Train
    print("\nTraining VAE...")
    vae = train_vae_model(x_train, epochs=5)

    # Save weights
    vae.save_weights(VAE_WEIGHTS_FILE)
    print(f"Weights saved to: {VAE_WEIGHTS_FILE}")

    # Fit threshold
    errors = reconstruction_errors(vae, x_train)
    threshold = fit_threshold(errors, 99.0)
    with open(VAE_THRESHOLD_FILE, "w") as f:
        f.write(str(threshold))
    print(f"Threshold: {threshold:.6f} (saved to {VAE_THRESHOLD_FILE})")

    # Test with normal and anomalous samples
    print("\n--- Anomaly Detection Test ---")
    normal_sample = np.random.rand(11).astype(np.float32) * 0.5
    err, flag = is_anomalous(vae, normal_sample, threshold)
    print(f"Normal sample:    error={err:.4f}  anomalous={flag}")

    anomalous_sample = np.ones(11, dtype=np.float32) * 5.0
    err, flag = is_anomalous(vae, anomalous_sample, threshold)
    print(f"Anomalous sample: error={err:.4f}  anomalous={flag}")


if __name__ == "__main__":
    main()

