"""Backward-compatible shim — imports from model.predictor.

The canonical implementation lives in model/predictor.py.
This file exists so that existing code importing from 'model' continues to work:

    from model import LSTMTransformerForecast

For new code, prefer:

    from model.predictor import LSTMTransformerForecast
"""

from model.predictor import LSTMEncoder, LSTMTransformerForecast, PositionalEncoding

__all__ = ["PositionalEncoding", "LSTMEncoder", "LSTMTransformerForecast"]


# -------------------------
# Quick sanity check
# -------------------------
if __name__ == "__main__":
    batch_size = 64
    seq_len = 288
    input_dim = 16
    num_outputs = 20

    model = LSTMTransformerForecast(input_dim=input_dim, num_outputs=num_outputs)
    x = torch.randn(batch_size, seq_len, input_dim)
    logits = model(x)
    print("logits shape:", logits.shape)  # should be (64, 20)

