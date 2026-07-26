import tensorflow as tf
from tensorflow import keras
from tensorflow.keras import layers

class NFSVAE(keras.Model):
    def __init__(self, input_dim=11, latent_dim=8, beta=0.5, **kwargs):
        super().__init__(**kwargs)
        self.latent_dim = latent_dim
        self.beta = beta

        # Encoder
        self.enc_fc1 = layers.Dense(256, activation=None)
        self.bn1 = layers.BatchNormalization()
        self.enc_fc2 = layers.Dense(128, activation=None)
        self.bn2 = layers.BatchNormalization()
        self.enc_fc3 = layers.Dense(64, activation=None)
        self.bn3 = layers.BatchNormalization()
        self.enc_fc4 = layers.Dense(16, activation=None)
        self.bn4 = layers.BatchNormalization()
        self.mu_head = layers.Dense(latent_dim)
        self.logvar_head = layers.Dense(latent_dim)

        # Decoder
        self.dec_fc1 = layers.Dense(16, activation="relu")
        self.dec_fc2 = layers.Dense(64, activation="relu")
        self.dec_fc3 = layers.Dense(256, activation="relu")
        self.out_head = layers.Dense(input_dim, activation=None)

        self.total_loss_tracker = keras.metrics.Mean(name="loss")
        self.recon_loss_tracker = keras.metrics.Mean(name="recon_loss")
        self.kl_loss_tracker = keras.metrics.Mean(name="kl_loss")

    @property
    def metrics(self):
        return [self.total_loss_tracker, self.recon_loss_tracker, self.kl_loss_tracker]

    def encode(self, x, training=False):
        x = self.enc_fc1(x)
        x = self.bn1(x, training=training)
        x = tf.nn.relu(x)
        x = self.enc_fc2(x)
        x = self.bn2(x, training=training)
        x = tf.nn.relu(x)
        x = self.enc_fc3(x)
        x = self.bn3(x, training=training)
        x = tf.nn.relu(x)
        x = self.enc_fc4(x)
        x = self.bn4(x, training=training)
        x = tf.nn.relu(x)
        mu = self.mu_head(x)
        logvar = self.logvar_head(x)
        return mu, logvar

    def reparameterize(self, mu, logvar):
        eps = tf.random.normal(shape=tf.shape(mu))
        std = tf.exp(0.5 * logvar)
        return mu + eps * std

    def decode(self, z):
        x = self.dec_fc1(z)
        x = self.dec_fc2(x)
        x = self.dec_fc3(x)
        return self.out_head(x)

    def call(self, inputs, training=False):
        mu, logvar = self.encode(inputs, training=training)
        z = self.reparameterize(mu, logvar)
        x_hat = self.decode(z)
        return x_hat, mu, logvar

    def train_step(self, data):
        if isinstance(data, tuple):
            x = data[0]
        else:
            x = data

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


# ============================================================
#  Helper Functions (used by run_vae.py and inference_server)
# ============================================================

def train_vae_model(x_train, epochs=20, batch_size=128, learning_rate=1e-3):
    """Train an NFSVAE model on the given training data.

    Args:
        x_train: np.ndarray of shape (N, 11)
        epochs: number of training epochs
        batch_size: training batch size
        learning_rate: Adam learning rate

    Returns:
        Trained NFSVAE model
    """
    input_dim = x_train.shape[1] if x_train.ndim > 1 else 11
    vae = NFSVAE(input_dim=input_dim)
    vae.compile(optimizer=keras.optimizers.Adam(learning_rate=learning_rate))
    vae.fit(x_train, epochs=epochs, batch_size=batch_size, verbose=1)
    return vae


def reconstruction_errors(vae, x):
    """Compute per-sample reconstruction error (sum of squared errors).

    Args:
        vae: trained NFSVAE model
        x: np.ndarray of shape (N, input_dim)

    Returns:
        np.ndarray of shape (N,) — reconstruction error per sample
    """
    x_tf = tf.cast(x, tf.float32)
    x_hat, _, _ = vae(x_tf, training=False)
    return tf.reduce_sum(tf.square(x_tf - x_hat), axis=1).numpy()


def fit_threshold(errors, percentile=99.0):
    """Compute the anomaly threshold at the given percentile of training errors.

    Args:
        errors: np.ndarray of shape (N,) — reconstruction errors from training set
        percentile: percentile cutoff (default 99.0)

    Returns:
        float — threshold value
    """
    import numpy as np
    return float(np.percentile(errors, percentile))


def is_anomalous(vae, sample, threshold):
    """Check whether a single sample is anomalous.

    Args:
        vae: trained NFSVAE model
        sample: np.ndarray of shape (input_dim,)
        threshold: float — anomaly threshold from fit_threshold()

    Returns:
        (error, is_anomaly): tuple of (float reconstruction error, bool)
    """
    import numpy as np
    err = reconstruction_errors(vae, sample[np.newaxis])[0]
    return float(err), bool(err > threshold)

