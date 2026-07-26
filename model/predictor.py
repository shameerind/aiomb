"""LSTM-Transformer forecasting model for mount policy pre-staging.

Predicts which mount policies will be needed in the next 15–30 minutes,
enabling proactive pre-staging to reduce mount latency.

Architecture:
    Input  → LSTM Encoder (2 layers, 128 hidden) → Transformer Decoder (4 heads, 2 layers)
    Output → Binary logits for N mount-policy pair predictions
"""

from __future__ import annotations

import math

import torch
import torch.nn as nn

from model.config import (
    PREDICTOR_D_MODEL,
    PREDICTOR_DIM_FEEDFORWARD,
    PREDICTOR_INPUT_DIM,
    PREDICTOR_NHEAD,
    PREDICTOR_NUM_LAYERS,
    PREDICTOR_NUM_OUTPUTS,
)


class PositionalEncoding(nn.Module):
    """Sinusoidal positional encoding for Transformer inputs."""

    def __init__(self, d_model: int, max_len: int = 500) -> None:
        super().__init__()
        pe = torch.zeros(max_len, d_model)
        position = torch.arange(0, max_len, dtype=torch.float32).unsqueeze(1)
        div_term = torch.exp(
            torch.arange(0, d_model, 2).float() * (-math.log(10000.0) / d_model)
        )
        pe[:, 0::2] = torch.sin(position * div_term)
        pe[:, 1::2] = torch.cos(position * div_term)
        pe = pe.unsqueeze(1)  # (max_len, 1, d_model)
        self.register_buffer("pe", pe)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        """Add positional encoding. x shape: (seq_len, batch, d_model)."""
        return x + self.pe[: x.size(0)]


class LSTMEncoder(nn.Module):
    """Bidirectional LSTM encoder for temporal feature extraction."""

    def __init__(
        self,
        input_dim: int,
        hidden_dim: int = 128,
        num_layers: int = 2,
        dropout: float = 0.2,
    ) -> None:
        super().__init__()
        self.lstm = nn.LSTM(
            input_size=input_dim,
            hidden_size=hidden_dim,
            num_layers=num_layers,
            dropout=dropout,
            batch_first=False,
        )

    def forward(self, x: torch.Tensor) -> tuple[torch.Tensor, tuple]:
        """Encode input sequence.

        Args:
            x: Tensor of shape (seq_len, batch, input_dim).

        Returns:
            outputs: (seq_len, batch, hidden_dim) — all hidden states.
            (h_n, c_n): Final hidden and cell states.
        """
        return self.lstm(x)


class LSTMTransformerForecast(nn.Module):
    """LSTM encoder + Transformer decoder for mount-policy forecasting.

    Args:
        input_dim: Number of input features per timestep.
        num_outputs: Number of binary output predictions.
        d_model: Model dimension (LSTM hidden + Transformer).
        nhead: Number of attention heads in Transformer decoder.
        num_layers: Number of Transformer decoder layers.
        dim_feedforward: Feedforward dimension in Transformer layers.
    """

    def __init__(
        self,
        input_dim: int = PREDICTOR_INPUT_DIM,
        num_outputs: int = PREDICTOR_NUM_OUTPUTS,
        d_model: int = PREDICTOR_D_MODEL,
        nhead: int = PREDICTOR_NHEAD,
        num_layers: int = PREDICTOR_NUM_LAYERS,
        dim_feedforward: int = PREDICTOR_DIM_FEEDFORWARD,
    ) -> None:
        super().__init__()
        self.d_model = d_model

        self.encoder = LSTMEncoder(input_dim=input_dim, hidden_dim=d_model)
        self.query_token = nn.Parameter(torch.randn(1, 1, d_model))
        self.pos_enc = PositionalEncoding(d_model)

        decoder_layer = nn.TransformerDecoderLayer(
            d_model=d_model,
            nhead=nhead,
            dim_feedforward=dim_feedforward,
            batch_first=False,
        )
        self.decoder = nn.TransformerDecoder(decoder_layer, num_layers=num_layers)
        self.out_proj = nn.Linear(d_model, num_outputs)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        """Forward pass.

        Args:
            x: Input tensor of shape (batch, seq_len, input_dim).

        Returns:
            Logits of shape (batch, num_outputs).
        """
        # LSTM expects (seq, batch, feat)
        x = x.transpose(0, 1)
        memory, _ = self.encoder(x)

        batch_size = x.size(1)
        tgt = self.query_token.expand(1, batch_size, self.d_model)
        tgt = self.pos_enc(tgt)

        decoded = self.decoder(tgt=tgt, memory=memory)
        decoded = decoded.squeeze(0)  # (batch, d_model)
        return self.out_proj(decoded)
