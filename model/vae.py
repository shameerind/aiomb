"""Variational Autoencoder for NFS telemetry anomaly detection.

Learns the distribution of normal NFS telemetry vectors and flags samples
with high reconstruction error as anomalous. Uses a β-VAE formulation
with batch-normalized encoder layers.

Architecture:
    Encoder: 256 → 128 → 64 → 16 → (μ, log σ²) with BatchNorm + ReLU
    Decoder: latent → 16 → 64 → 256 → input_dim
    Loss:    SSE reconstruction + β · KL divergence
"""

from __future__ import annotations

import numpy as np
import tensorflow as tf
from tensorflow import keras
from tensorflow.keras import layers

from model.config import VAE_BETA, VAE_INPUT_DIM, VAE_LATENT_DIM, VAE_THRESHOLD_PERCENTILE


class NFSVAE(keras.Model):
    """β-VAE for NFS health anomaly detection.

    Args:
        input_dim: Dimensionality of telemetry input (default: 11).
        latent_dim: Latent space dimensionality (default: 8).
        beta: KL divergence weight (default: 0.5).
    """

    def __init__(
        self,
        input_dim: int = VAE_INPUT_DIM,
        latent_dim: int = VAE_LATENT_DIM,
        beta: float = VAE_BETA,
        **kwargs,
    ) -> None:
        super().__init__(**kwargs)
        self.latent_dim = latent_dim
        self.beta = beta

        # Encoder layers
        self.enc_fc1 = layers.Dense(256)
        self.bn1 = layers.BatchNormalization()
        self.enc_fc2 = layers.Dense(128)
        self.bn2 = layers.BatchNormalization()
        self.enc_fc3 = layers.Dense(64)
        self.bn3 = layers.BatchNormalization()
        self.enc_fc4 = layers.Dense(16)
        self.bn4 = layers.BatchNormalization()
        self.mu_head = layers.Dense(latent_dim)
        self.logvar_head = layers.Dense(latent_dim)

        # Decoder layers
        self.dec_fc1 = layers.Dense(16, activation="relu")
        self.dec_fc2 = layers.Dense(64, activation="relu")
        self.dec_fc3 = layers.Dense(256, activation="relu")
        self.out_head = layers.Dense(input_dim)

        # Metrics
        self.total_loss_tracker = keras.metrics.Mean(name="loss")
        self.recon_loss_tracker = keras.metrics.Mean(name="recon_loss")
        self.kl_loss_tracker = keras.metrics.Mean(name="kl_loss")

    @property
    def metrics(self):
        return [self.total_loss_tracker, self.recon_loss_tracker, self.kl_loss_tracker]

    def encode(self, x: tf.Tensor, training: bool = False) -> tuple[tf.Tensor, tf.Tensor]:
        """Encode input to latent distribution parameters (μ, log σ²)."""
        h = tf.nn.relu(self.bn1(self.enc_fc1(x), training=training))
        h = tf.nn.relu(self.bn2(self.enc_fc2(h), training=training))
        h = tf.nn.relu(self.bn3(self.enc_fc3(h), training=training))
        h = tf.nn.relu(self.bn4(self.enc_fc4(h), training=training))
        return self.mu_head(h), self.logvar_head(h)

    def reparameterize(self, mu: tf.Tensor, logvar: tf.Tensor) -> tf.Tensor:
        """Sample from latent distribution using reparameterization trick."""
        eps = tf.random.normal(shape=tf.shape(mu))
        return mu + tf.exp(0.5 * logvar) * eps

    def decode(self, z: tf.Tensor) -> tf.Tensor:
        """Decode latent vector to reconstruction."""
        h = self.dec_fc1(z)
        h = self.dec_fc2(h)
        h = self.dec_fc3(h)
        return self.out_head(h)

    def call(self, inputs: tf.Tensor, training: bool = False) -> tuple[tf.Tensor, tf.Tensor, tf.Tensor]:
        """Forward pass: encode → sample → decode.

        Returns:
            (x_hat, mu, logvar) tuple.
        """
        mu, logvar = self.encode(inputs, training=training)
        z = self.reparameterize(mu, logvar)
        x_hat = self.decode(z)
        return x_hat, mu, logvar

    def train_step(self, data):
        x = data[0] if isinstance(data, tuple) else data

        with tf.GradientTape() as tape:
            x_hat, mu, logvar = self(x, training=True)
            recon_loss = tf.reduce_mean(tf.reduce_sum(tf.square(x - x_hat), axis=1))
            kl_loss = -0.5 * tf.reduce_mean(
                tf.reduce_sum(1 + logvar - tf.square(mu) - tf.exp(logvar), axis=1)
            )
            loss = recon_loss + self.beta * kl_loss

        grads = tape.gradient(loss, self.trainable_variables)
        self.optimizer.apply_gradients(zip(grads, self.trainable_variables))

        self.total_loss_tracker.update_state(loss)
        self.recon_loss_tracker.update_state(recon_loss)
        self.kl_loss_tracker.update_state(kl_loss)
        return {
            "loss": self.total_loss_tracker.result(),
            "recon_loss": self.recon_loss_tracker.result(),
            "kl_loss": self.kl_loss_tracker.result(),
        }


# ─── Public Helper Functions ──────────────────────────────────────────────────


def train_vae_model(
    x_train: np.ndarray,
    epochs: int = 20,
    batch_size: int = 128,
    learning_rate: float = 1e-3,
) -> NFSVAE:
    """Train an NFSVAE model on telemetry data.

    Args:
        x_train: Training data of shape (N, input_dim).
        epochs: Number of training epochs.
        batch_size: Training batch size.
        learning_rate: Adam learning rate.

    Returns:
        Trained NFSVAE instance.
    """
    input_dim = x_train.shape[1] if x_train.ndim > 1 else VAE_INPUT_DIM
    vae = NFSVAE(input_dim=input_dim)
    vae.compile(optimizer=keras.optimizers.Adam(learning_rate=learning_rate))
    vae.fit(x_train, epochs=epochs, batch_size=batch_size, verbose=1)
    return vae


def reconstruction_errors(vae: NFSVAE, x: np.ndarray) -> np.ndarray:
    """Compute per-sample reconstruction error (sum of squared errors).

    Args:
        vae: Trained NFSVAE model.
        x: Input array of shape (N, input_dim).

    Returns:
        Array of shape (N,) with reconstruction error per sample.
    """
    x_tf = tf.cast(x, tf.float32)
    x_hat, _, _ = vae(x_tf, training=False)
    return tf.reduce_sum(tf.square(x_tf - x_hat), axis=1).numpy()


def fit_threshold(errors: np.ndarray, percentile: float = VAE_THRESHOLD_PERCENTILE) -> float:
    """Compute anomaly threshold at the given percentile of training errors.

    Args:
        errors: Reconstruction errors from training set, shape (N,).
        percentile: Percentile cutoff (default: 99.0).

    Returns:
        Threshold value as float.
    """
    return float(np.percentile(errors, percentile))


def is_anomalous(vae: NFSVAE, sample: np.ndarray, threshold: float) -> tuple[float, bool]:
    """Check whether a single telemetry sample is anomalous.

    Args:
        vae: Trained NFSVAE model.
        sample: Telemetry vector of shape (input_dim,).
        threshold: Anomaly threshold from fit_threshold().

    Returns:
        Tuple of (reconstruction_error, is_anomaly_flag).
    """
    err = reconstruction_errors(vae, sample[np.newaxis])[0]
    return float(err), bool(err > threshold)

