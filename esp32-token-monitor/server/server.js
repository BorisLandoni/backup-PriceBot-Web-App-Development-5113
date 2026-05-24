'use strict';

const express = require('express');
const cors = require('cors');

const app = express();
const PORT = process.env.PORT || 3333;

app.use(cors());
app.use(express.json());

// Pricing per million tokens — claude-sonnet-4-x rates
const PRICE_PER_M = {
  input: 3.0,
  output: 15.0,
  cache_read: 0.30,
  cache_creation: 3.75,
};

let stats = freshStats();

function freshStats() {
  return {
    total_input: 0,
    total_output: 0,
    total_cache_read: 0,
    total_cache_creation: 0,
    requests_count: 0,
    last_updated: null,
  };
}

function costUSD(s) {
  return (
    (s.total_input * PRICE_PER_M.input +
      s.total_output * PRICE_PER_M.output +
      s.total_cache_read * PRICE_PER_M.cache_read +
      s.total_cache_creation * PRICE_PER_M.cache_creation) /
    1_000_000
  ).toFixed(6);
}

function fullStats() {
  return { ...stats, cost_usd: parseFloat(costUSD(stats)) };
}

// GET /api/tokens  — ESP32 polls this endpoint
app.get('/api/tokens', (_req, res) => {
  res.json(fullStats());
});

// POST /api/tokens  — called by your app after each Claude API response
// Body: { input_tokens, output_tokens, cache_read_tokens?, cache_creation_tokens? }
app.post('/api/tokens', (req, res) => {
  const {
    input_tokens = 0,
    output_tokens = 0,
    cache_read_tokens = 0,
    cache_creation_tokens = 0,
  } = req.body;

  stats.total_input += input_tokens;
  stats.total_output += output_tokens;
  stats.total_cache_read += cache_read_tokens;
  stats.total_cache_creation += cache_creation_tokens;
  stats.requests_count += 1;
  stats.last_updated = new Date().toISOString();

  console.log(
    `[+] req #${stats.requests_count} | in:${input_tokens} out:${output_tokens} | total cost: $${costUSD(stats)}`
  );

  res.json({ ok: true, stats: fullStats() });
});

// DELETE /api/tokens  — reset counter (e.g. start of new session)
app.delete('/api/tokens', (_req, res) => {
  stats = freshStats();
  res.json({ ok: true, message: 'Stats reset' });
});

// Health check for ESP32 connectivity test
app.get('/health', (_req, res) => res.json({ ok: true }));

app.listen(PORT, '0.0.0.0', () => {
  console.log(`Claude token monitor server listening on http://0.0.0.0:${PORT}`);
  console.log(`  ESP32 → GET  http://<your-ip>:${PORT}/api/tokens`);
  console.log(`  App   → POST http://localhost:${PORT}/api/tokens`);
});
