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
        position = torch.arange(0, max_len, dtype=torch.float).unsqueeze(1)
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
            batch_first=True  # (batch, seq, feat)
        )

    def forward(self, x):
        # x: (batch, seq_len, input_dim)
        outputs, (h_n, c_n) = self.lstm(x)
        # outputs: (batch, seq_len, hidden_dim)
        # h_n: (num_layers, batch, hidden_dim)
        return outputs, (h_n, c_n)


# -------------------------
# Transformer Decoder
# -------------------------
class TransformerDecoder(nn.Module):
    def __init__(self, d_model=128, nhead=4, num_layers=2, dim_feedforward=256, num_outputs=10):
        super().__init__()
        decoder_layer = nn.TransformerDecoderLayer(
            d_model=d_model,
            nhead=nhead,
            dim_feedforward=dim_feedforward,
            batch_first=True  # (batch, seq, d_model)
        )
        self.decoder = nn.TransformerDecoder(decoder_layer, num_layers=num_layers)
        self.pos_enc = PositionalEncoding(d_model)
        self.out_proj = nn.Linear(d_model, num_outputs)  # multi-label logits

    def forward(self, tgt, memory):
        # tgt: (batch, tgt_seq_len, d_model) - e.g., learned query tokens
        # memory: (batch, src_seq_len, d_model) - encoder outputs
        tgt = tgt.transpose(0, 1)      # (tgt_seq_len, batch, d_model)
        memory = memory.transpose(0, 1)  # (src_seq_len, batch, d_model)
        tgt = self.pos_enc(tgt)
        decoded = self.decoder(tgt, memory)  # (tgt_seq_len, batch, d_model)
        decoded = decoded.transpose(0, 1)    # (batch, tgt_seq_len, d_model)
        logits = self.out_proj(decoded[:, 0, :])  # use first token for prediction
        return logits


# -------------------------
# Full Model
# -------------------------
class LSTMTransformerForecast(nn.Module):
    def __init__(self, input_dim, num_outputs, d_model=128):
        super().__init__()
        self.encoder = LSTMEncoder(input_dim=input_dim, hidden_dim=d_model)
        
        # NEW: projection layer ensures correct dimension
        self.proj = nn.Linear(d_model, d_model)
        self.query_tokens = nn.Parameter(torch.randn(1, 1, d_model))  # single query token
        self.decoder = TransformerDecoder(
            d_model=d_model,
            nhead=4,
            num_layers=2,
            dim_feedforward=256,
            num_outputs=num_outputs
        )

    def forward(self, x):
        # x: (batch, seq_len=288, input_dim)
        enc_outputs, _ = self.encoder(x)  # (batch, seq_len, d_model)

        # repeat query token for batch
        batch_size = x.size(0)
        tgt = self.query_tokens.expand(batch_size, -1, -1)  # (batch, 1, d_model)

        logits = self.decoder(tgt, enc_outputs)  # (batch, num_outputs)
        return logits


# -------------------------
# Example usage
# -------------------------
if __name__ == "__main__":
    batch_size = 32
    seq_len = 288      # 24h * 5min
    input_dim = 16     # example feature size
    num_outputs = 20   # number of policy-mount pairs

    model = LSTMTransformerForecast(input_dim=input_dim, num_outputs=num_outputs)
    x = torch.randn(batch_size, seq_len, input_dim)
    logits = model(x)  # (batch, num_outputs)

    # BCE with logits for multi-label prediction
    target = torch.randint(0, 2, (batch_size, num_outputs)).float()
    criterion = nn.BCEWithLogitsLoss()
    loss = criterion(logits, target)
    loss.backward()

