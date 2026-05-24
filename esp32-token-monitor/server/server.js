'use strict';

const express = require('express');
const cors    = require('cors');
const fs      = require('fs');
const path    = require('path');

const app       = express();
const PORT      = process.env.PORT || 3333;
const DATA_FILE = path.join(__dirname, 'data.json');

app.use(cors());
app.use(express.json());

// USD per milione di token — tariffe claude-sonnet-4-x
const PRICE_PER_M = {
  input:            3.00,
  output:          15.00,
  cache_read:       0.30,
  cache_creation:   3.75,
};

// Storico eventi: { ts, input, output, cache_read, cache_creation, cost }
let history = [];

function loadHistory() {
  try {
    if (fs.existsSync(DATA_FILE)) {
      history = JSON.parse(fs.readFileSync(DATA_FILE, 'utf8'));
      console.log(`Caricati ${history.length} eventi da disco`);
    }
  } catch (e) {
    console.warn('Impossibile caricare storico:', e.message);
    history = [];
  }
}

function saveHistory() {
  try {
    if (history.length > 100_000) history = history.slice(-100_000);
    fs.writeFileSync(DATA_FILE, JSON.stringify(history));
  } catch (e) {
    console.warn('Impossibile salvare storico:', e.message);
  }
}

function calcCost(e) {
  return (
    (e.input          * PRICE_PER_M.input +
     e.output         * PRICE_PER_M.output +
     e.cache_read     * PRICE_PER_M.cache_read +
     e.cache_creation * PRICE_PER_M.cache_creation) / 1_000_000
  );
}

function sessionTotals() {
  const t = history.reduce(
    (a, e) => ({
      input:          a.input          + e.input,
      output:         a.output         + e.output,
      cache_read:     a.cache_read     + e.cache_read,
      cache_creation: a.cache_creation + e.cache_creation,
      cost:           a.cost           + e.cost,
    }),
    { input: 0, output: 0, cache_read: 0, cache_creation: 0, cost: 0 }
  );
  return {
    total_input:          t.input,
    total_output:         t.output,
    total_cache_read:     t.cache_read,
    total_cache_creation: t.cache_creation,
    requests_count:       history.length,
    cost_usd:             parseFloat(t.cost.toFixed(6)),
    last_updated:         history.length ? new Date(history.at(-1).ts).toISOString() : null,
  };
}

function aggregate(events) {
  return events.reduce(
    (a, e) => ({ input: a.input + e.input, output: a.output + e.output, cost: a.cost + e.cost }),
    { input: 0, output: 0, cost: 0 }
  );
}

// ── Endpoints base ────────────────────────────────────────────────────────────

// GET /api/tokens — totali sessione (ESP32 schermata NOW)
app.get('/api/tokens', (_req, res) => res.json(sessionTotals()));

// POST /api/tokens — registra chiamata Claude API
app.post('/api/tokens', (req, res) => {
  const {
    input_tokens = 0, output_tokens = 0,
    cache_read_tokens = 0, cache_creation_tokens = 0,
  } = req.body;

  const event = {
    ts:             Date.now(),
    input:          input_tokens,
    output:         output_tokens,
    cache_read:     cache_read_tokens,
    cache_creation: cache_creation_tokens,
    cost:           0,
  };
  event.cost = calcCost(event);
  history.push(event);
  saveHistory();

  const stats = sessionTotals();
  console.log(`[+] #${stats.requests_count} | in:${input_tokens} out:${output_tokens} | totale: $${stats.cost_usd}`);
  res.json({ ok: true, stats });
});

// DELETE /api/tokens — reset completo
app.delete('/api/tokens', (_req, res) => {
  history = [];
  saveHistory();
  res.json({ ok: true });
});

// ── Endpoints aggregati per ESP32 ─────────────────────────────────────────────

// Ultime 24 ore → 24 bucket orari
app.get('/api/tokens/hourly', (_req, res) => {
  const now  = Date.now();
  const HOUR = 3_600_000;
  const result = [];
  for (let i = 23; i >= 0; i--) {
    const start = now - (i + 1) * HOUR;
    const end   = now - i       * HOUR;
    const agg   = aggregate(history.filter(e => e.ts >= start && e.ts < end));
    const d     = new Date(start);
    result.push({
      label:  String(d.getHours()).padStart(2, '0'),
      input:  agg.input,
      output: agg.output,
      cost:   parseFloat(agg.cost.toFixed(6)),
    });
  }
  res.json(result);
});

// Ultimi 7 giorni → 7 bucket giornalieri
const IT_DAYS = ['Dom','Lun','Mar','Mer','Gio','Ven','Sab'];
app.get('/api/tokens/daily', (_req, res) => {
  const now = Date.now();
  const DAY = 86_400_000;
  const result = [];
  for (let i = 6; i >= 0; i--) {
    const start = now - (i + 1) * DAY;
    const end   = now - i       * DAY;
    const agg   = aggregate(history.filter(e => e.ts >= start && e.ts < end));
    const d     = new Date(start);
    result.push({
      label:  IT_DAYS[d.getDay()],
      input:  agg.input,
      output: agg.output,
      cost:   parseFloat(agg.cost.toFixed(6)),
    });
  }
  res.json(result);
});

// Ultime 4 settimane → 4 bucket settimanali
app.get('/api/tokens/weekly', (_req, res) => {
  const now  = Date.now();
  const WEEK = 7 * 86_400_000;
  const result = [];
  for (let i = 3; i >= 0; i--) {
    const start = now - (i + 1) * WEEK;
    const end   = now - i       * WEEK;
    const agg   = aggregate(history.filter(e => e.ts >= start && e.ts < end));
    const d     = new Date(start);
    const jan1  = new Date(d.getFullYear(), 0, 1);
    const week  = Math.ceil((((d - jan1) / 86400000) + jan1.getDay() + 1) / 7);
    result.push({
      label:  `W${week}`,
      input:  agg.input,
      output: agg.output,
      cost:   parseFloat(agg.cost.toFixed(6)),
    });
  }
  res.json(result);
});

app.get('/health', (_req, res) => res.json({ ok: true }));

loadHistory();
app.listen(PORT, '0.0.0.0', () => {
  console.log(`\nClaude Token Monitor Server → http://0.0.0.0:${PORT}`);
  console.log('  GET  /api/tokens          → totali sessione');
  console.log('  GET  /api/tokens/hourly   → ultime 24 ore  (ESP32)');
  console.log('  GET  /api/tokens/daily    → ultimi 7 giorni (ESP32)');
  console.log('  GET  /api/tokens/weekly   → ultime 4 settimane (ESP32)');
  console.log('  POST /api/tokens          → registra uso API');
  console.log('  DEL  /api/tokens          → reset\n');
});
