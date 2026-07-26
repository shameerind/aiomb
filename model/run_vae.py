import numpy as np
from vae import NFSVAE, train_vae_model, reconstruction_errors, fit_threshold, is_anomalous
import tensorflow as tf

# Fake training data (replace with real NFS telemetry)
x_train = np.random.rand(5000, 11).astype(np.float32)

# Train model
vae = train_vae_model(x_train, epochs=5)

# Fit threshold
errors = reconstruction_errors(vae, x_train)
threshold = fit_threshold(errors, 99.0)
print("Threshold:", threshold)

# Test with a new sample
sample = np.random.rand(11).astype(np.float32)
err, flag = is_anomalous(vae, sample, threshold)
print("Error:", err)
print("Anomaly:", flag)

