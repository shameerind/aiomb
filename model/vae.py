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

