import torch
import torch.nn as nn
from torch.utils.data import Dataset, DataLoader
import numpy as np
from sklearn.model_selection import train_test_split

# ---------------------------------------------------------
# 1. MODEL (import your model here)
# ---------------------------------------------------------
from model import LSTMTransformerForecast   # your model file


# ---------------------------------------------------------
# 2. DATASET
# ---------------------------------------------------------
class MountForecastDataset(Dataset):
    """
    X: numpy array of shape (N, 288, input_dim)
    y: numpy array of shape (N, num_outputs)
    """
    def __init__(self, X, y):
        self.X = torch.tensor(X, dtype=torch.float32)
        self.y = torch.tensor(y, dtype=torch.float32)

    def __len__(self):
        return len(self.X)

    def __getitem__(self, idx):
        return self.X[idx], self.y[idx]


# ---------------------------------------------------------
# 3. TRAINING + VALIDATION FUNCTIONS
# ---------------------------------------------------------
def train_one_epoch(model, dataloader, optimizer, criterion, device):
    model.train()
    total_loss = 0

    for batch_x, batch_y in dataloader:
        batch_x = batch_x.to(device)
        batch_y = batch_y.to(device)

        optimizer.zero_grad()
        logits = model(batch_x)
        loss = criterion(logits, batch_y)
        loss.backward()
        optimizer.step()

        total_loss += loss.item() * batch_x.size(0)

    return total_loss / len(dataloader.dataset)


def validate(model, dataloader, criterion, device):
    model.eval()
    total_loss = 0

    with torch.no_grad():
        for batch_x, batch_y in dataloader:
            batch_x = batch_x.to(device)
            batch_y = batch_y.to(device)

            logits = model(batch_x)
            loss = criterion(logits, batch_y)
            total_loss += loss.item() * batch_x.size(0)

    return total_loss / len(dataloader.dataset)


# ---------------------------------------------------------
# 4. MAIN TRAINING SCRIPT
# ---------------------------------------------------------
def main():

    # -----------------------------
    # Hyperparameters
    # -----------------------------
    input_dim = 16          # number of features per timestep
    num_outputs = 20        # number of mount-policy pairs
    seq_len = 288           # 24 hours * 12 samples/hour
    batch_size = 64
    epochs = 15
    lr = 1e-3
    weight_decay = 1e-5

    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")

    # -----------------------------
    # Load your data here
    # Replace with real data
    # -----------------------------
    N = 5000  # dummy dataset size
    X = np.random.randn(N, seq_len, input_dim)
    y = np.random.randint(0, 2, (N, num_outputs))

    # -----------------------------
    # Train/Val Split
    # -----------------------------
    X_train, X_val, y_train, y_val = train_test_split(
        X, y, test_size=0.15, random_state=42
    )

    train_dataset = MountForecastDataset(X_train, y_train)
    val_dataset = MountForecastDataset(X_val, y_val)

    train_loader = DataLoader(train_dataset, batch_size=batch_size, shuffle=True)
    val_loader = DataLoader(val_dataset, batch_size=batch_size)

    # -----------------------------
    # Model, Loss, Optimizer
    # -----------------------------
    model = LSTMTransformerForecast(
        input_dim=input_dim,
        num_outputs=num_outputs
    ).to(device)

    criterion = nn.BCEWithLogitsLoss()
    optimizer = torch.optim.Adam(
        model.parameters(),
        lr=lr,
        weight_decay=weight_decay
    )

    # -----------------------------
    # Training Loop
    # -----------------------------
    best_val_loss = float("inf")

    for epoch in range(1, epochs + 1):
        train_loss = train_one_epoch(model, train_loader, optimizer, criterion, device)
        val_loss = validate(model, val_loader, criterion, device)

        print(f"Epoch {epoch}/{epochs} | Train Loss: {train_loss:.4f} | Val Loss: {val_loss:.4f}")

        # Save best model
        if val_loss < best_val_loss:
            best_val_loss = val_loss
            torch.save(model.state_dict(), "best_model.pt")
            print("  → Saved new best model")

    print("Training complete.")


if __name__ == "__main__":
    main()

