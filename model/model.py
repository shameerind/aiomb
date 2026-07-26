import torch
import torch.nn as nn
import math

# -------------------------
# Positional Encoding
# -------------------------
class PositionalEncoding(nn.Module):
    def __init__(self, d_model, max_len=500):
        super().__init__()
        pe = torch.zeros(max_len, d_model)
        position = torch.arange(0, max_len, dtype=torch.float32).unsqueeze(1)
        div_term = torch.exp(torch.arange(0, d_model, 2).float() *
                             (-math.log(10000.0) / d_model))
        pe[:, 0::2] = torch.sin(position * div_term)
        pe[:, 1::2] = torch.cos(position * div_term)
        pe = pe.unsqueeze(1)  # (max_len, 1, d_model)
        self.register_buffer('pe', pe)

    def forward(self, x):
        # x: (seq_len, batch, d_model)
        seq_len = x.size(0)
        return x + self.pe[:seq_len]


# -------------------------
# LSTM Encoder
# -------------------------
class LSTMEncoder(nn.Module):
    def __init__(self, input_dim, hidden_dim=128, num_layers=2, dropout=0.2):
        super().__init__()
        self.lstm = nn.LSTM(
            input_size=input_dim,
            hidden_size=hidden_dim,
            num_layers=num_layers,
            dropout=dropout,
            batch_first=False  # (seq, batch, feat)
        )

    def forward(self, x):
        # x: (seq_len, batch, input_dim)
        outputs, (h_n, c_n) = self.lstm(x)
        # outputs: (seq_len, batch, hidden_dim)
        return outputs, (h_n, c_n)


# -------------------------
# Full Model: LSTM + TransformerDecoder
# -------------------------
class LSTMTransformerForecast(nn.Module):
    def __init__(self, input_dim, num_outputs, d_model=128, nhead=4, num_layers=2, dim_feedforward=256):
        super().__init__()
        self.d_model = d_model

        # Encoder
        self.encoder = LSTMEncoder(input_dim=input_dim, hidden_dim=d_model)

        # One query token for decoder
        self.query_token = nn.Parameter(torch.randn(1, 1, d_model))

        # Positional encoding for decoder input
        self.pos_enc = PositionalEncoding(d_model)

        # Transformer decoder
        decoder_layer = nn.TransformerDecoderLayer(
            d_model=d_model,
            nhead=nhead,
            dim_feedforward=dim_feedforward,
            batch_first=False  # (tgt_seq, batch, d_model)
        )
        self.decoder = nn.TransformerDecoder(decoder_layer, num_layers=num_layers)

        # Output projection
        self.out_proj = nn.Linear(d_model, num_outputs)

    def forward(self, x):
        """
        x: (batch, seq_len, input_dim)
        """
        # Transformer & LSTM expect (seq, batch, feat)
        x = x.transpose(0, 1)  # (seq_len, batch, input_dim)

        # Encode
        memory, _ = self.encoder(x)  # (seq_len, batch, d_model)

        batch_size = x.size(1)

        # Prepare single query token as decoder input
        tgt = self.query_token.expand(1, batch_size, self.d_model)  # (1, batch, d_model)
        tgt = self.pos_enc(tgt)  # add positional encoding

        # Decode with cross-attention over encoder outputs
        decoded = self.decoder(tgt=tgt, memory=memory)  # (1, batch, d_model)

        # Use the single token to predict
        decoded = decoded.squeeze(0)  # (batch, d_model)
        logits = self.out_proj(decoded)  # (batch, num_outputs)
        return logits


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

