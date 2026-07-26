"""Training script for the LSTM-Transformer mount policy predictor.

Trains the model on historical mount activity sequences and saves the
best checkpoint to `best_model.pt`.

Usage:
    python -m model.train
    python model/train.py
"""

from __future__ import annotations

import numpy as np
import torch
import torch.nn as nn
from sklearn.model_selection import train_test_split
from torch.utils.data import DataLoader, Dataset

from model.config import (
    PREDICTOR_INPUT_DIM,
    PREDICTOR_NUM_OUTPUTS,
    PREDICTOR_SEQ_LEN,
    PREDICTOR_WEIGHTS_FILE,
)
from model.predictor import LSTMTransformerForecast


# ─── Dataset ──────────────────────────────────────────────────────────────────


class MountForecastDataset(Dataset):
    """Dataset wrapping (X, y) arrays for the mount predictor.

    Args:
        X: Array of shape (N, seq_len, input_dim).
        y: Array of shape (N, num_outputs).
    """

    def __init__(self, X: np.ndarray, y: np.ndarray) -> None:
        self.X = torch.tensor(X, dtype=torch.float32)
        self.y = torch.tensor(y, dtype=torch.float32)

    def __len__(self) -> int:
        return len(self.X)

    def __getitem__(self, idx: int) -> tuple[torch.Tensor, torch.Tensor]:
        return self.X[idx], self.y[idx]


# ─── Training Utilities ───────────────────────────────────────────────────────


def train_one_epoch(
    model: nn.Module,
    dataloader: DataLoader,
    optimizer: torch.optim.Optimizer,
    criterion: nn.Module,
    device: torch.device,
) -> float:
    """Run one training epoch and return average loss."""
    model.train()
    total_loss = 0.0

    for batch_x, batch_y in dataloader:
        batch_x, batch_y = batch_x.to(device), batch_y.to(device)

        optimizer.zero_grad()
        logits = model(batch_x)
        loss = criterion(logits, batch_y)
        loss.backward()
        optimizer.step()

        total_loss += loss.item() * batch_x.size(0)

    return total_loss / len(dataloader.dataset)


def validate(
    model: nn.Module,
    dataloader: DataLoader,
    criterion: nn.Module,
    device: torch.device,
) -> float:
    """Run validation and return average loss."""
    model.eval()
    total_loss = 0.0

    with torch.no_grad():
        for batch_x, batch_y in dataloader:
            batch_x, batch_y = batch_x.to(device), batch_y.to(device)
            logits = model(batch_x)
            loss = criterion(logits, batch_y)
            total_loss += loss.item() * batch_x.size(0)

    return total_loss / len(dataloader.dataset)


# ─── Main ─────────────────────────────────────────────────────────────────────


def main() -> None:
    """Train the LSTM-Transformer predictor."""

    # Hyperparameters
    input_dim = PREDICTOR_INPUT_DIM
    num_outputs = PREDICTOR_NUM_OUTPUTS
    seq_len = PREDICTOR_SEQ_LEN
    batch_size = 64
    epochs = 15
    lr = 1e-3
    weight_decay = 1e-5

    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    print(f"Device: {device}")

    # ── Load data (replace with real mount log sequences) ──
    N = 5000
    X = np.random.randn(N, seq_len, input_dim).astype(np.float32)
    y = np.random.randint(0, 2, (N, num_outputs)).astype(np.float32)

    # ── Train/Val split ──
    X_train, X_val, y_train, y_val = train_test_split(X, y, test_size=0.15, random_state=42)

    train_loader = DataLoader(MountForecastDataset(X_train, y_train), batch_size=batch_size, shuffle=True)
    val_loader = DataLoader(MountForecastDataset(X_val, y_val), batch_size=batch_size)

    # ── Model ──
    model = LSTMTransformerForecast(input_dim=input_dim, num_outputs=num_outputs).to(device)
    criterion = nn.BCEWithLogitsLoss()
    optimizer = torch.optim.Adam(model.parameters(), lr=lr, weight_decay=weight_decay)

    print(f"Model parameters: {sum(p.numel() for p in model.parameters()):,}")
    print(f"Training samples: {len(X_train):,} | Validation: {len(X_val):,}")
    print("-" * 60)

    # ── Training loop ──
    best_val_loss = float("inf")

    for epoch in range(1, epochs + 1):
        train_loss = train_one_epoch(model, train_loader, optimizer, criterion, device)
        val_loss = validate(model, val_loader, criterion, device)

        marker = ""
        if val_loss < best_val_loss:
            best_val_loss = val_loss
            torch.save(model.state_dict(), PREDICTOR_WEIGHTS_FILE)
            marker = " ✓ saved"

        print(f"Epoch {epoch:2d}/{epochs} | Train: {train_loss:.4f} | Val: {val_loss:.4f}{marker}")

    print("-" * 60)
    print(f"Training complete. Best val loss: {best_val_loss:.4f}")
    print(f"Weights saved to: {PREDICTOR_WEIGHTS_FILE}")


if __name__ == "__main__":
    main()

