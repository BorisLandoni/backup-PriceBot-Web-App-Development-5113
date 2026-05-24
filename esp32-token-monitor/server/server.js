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

// ── Prezzi per milione di token (claude-sonnet-4-x) ───────────────────────────
const PRICE_PER_M = { input: 3.00, output: 15.00, cache_read: 0.30, cache_creation: 3.75 };

// ── Stato in memoria ──────────────────────────────────────────────────────────
let history = [];           // { ts, input, output, cache_read, cache_creation, cost }
let account = null;         // { messages_used, messages_limit, messages_remaining, reset_at, plan, ts }

// ── Persistenza ───────────────────────────────────────────────────────────────
function load() {
  try {
    if (fs.existsSync(DATA_FILE)) {
      const d = JSON.parse(fs.readFileSync(DATA_FILE, 'utf8'));
      history = d.history || [];
      account = d.account || null;
      console.log(`Caricati ${history.length} eventi + dati account`);
    }
  } catch (e) { console.warn('Caricamento fallito:', e.message); }
}

function save() {
  try {
    if (history.length > 100_000) history = history.slice(-100_000);
    fs.writeFileSync(DATA_FILE, JSON.stringify({ history, account }));
  } catch (e) { console.warn('Salvataggio fallito:', e.message); }
}

// ── Helpers ───────────────────────────────────────────────────────────────────
function calcCost(e) {
  return (e.input * PRICE_PER_M.input + e.output * PRICE_PER_M.output +
          e.cache_read * PRICE_PER_M.cache_read + e.cache_creation * PRICE_PER_M.cache_creation) / 1_000_000;
}

function sessionTotals() {
  const t = history.reduce(
    (a, e) => ({ input: a.input + e.input, output: a.output + e.output,
                 cache_read: a.cache_read + e.cache_read,
                 cache_creation: a.cache_creation + e.cache_creation, cost: a.cost + e.cost }),
    { input: 0, output: 0, cache_read: 0, cache_creation: 0, cost: 0 }
  );
  return {
    total_input: t.input, total_output: t.output,
    total_cache_read: t.cache_read, total_cache_creation: t.cache_creation,
    requests_count: history.length,
    cost_usd: parseFloat(t.cost.toFixed(6)),
    last_updated: history.length ? new Date(history.at(-1).ts).toISOString() : null,
  };
}

function aggregate(events) {
  return events.reduce(
    (a, e) => ({ input: a.input + e.input, output: a.output + e.output, cost: a.cost + e.cost }),
    { input: 0, output: 0, cost: 0 }
  );
}

// Converte reset_at (stringa ISO o unix ts) in unix timestamp seconds
function toUnixTs(v) {
  if (!v) return null;
  if (typeof v === 'number') return v > 1e10 ? Math.floor(v / 1000) : v; // ms → s
  const d = new Date(v);
  return isNaN(d) ? null : Math.floor(d.getTime() / 1000);
}

// ── Endpoints token per messaggio ─────────────────────────────────────────────

app.get('/api/tokens', (_req, res) => res.json(sessionTotals()));

app.post('/api/tokens', (req, res) => {
  const { input_tokens = 0, output_tokens = 0,
          cache_read_tokens = 0, cache_creation_tokens = 0 } = req.body;
  const event = { ts: Date.now(), input: input_tokens, output: output_tokens,
                  cache_read: cache_read_tokens, cache_creation: cache_creation_tokens, cost: 0 };
  event.cost = calcCost(event);
  history.push(event);
  save();
  const stats = sessionTotals();
  console.log(`[token] #${stats.requests_count} in:${input_tokens} out:${output_tokens} | $${stats.cost_usd}`);
  res.json({ ok: true, stats });
});

app.delete('/api/tokens', (_req, res) => { history = []; save(); res.json({ ok: true }); });

// ── Endpoints limiti account (alimentati dal Tampermonkey) ────────────────────

// POST /api/account — il browser invia i limiti account intercettati da claude.ai
app.post('/api/account', (req, res) => {
  const { messages_remaining, messages_limit, messages_used, reset_at, plan } = req.body;

  // Ignora dati palesemente non validi
  if (messages_remaining === undefined && messages_limit === undefined) {
    return res.json({ ok: false, reason: 'no useful data' });
  }

  const reset_at_ts = toUnixTs(reset_at);
  account = {
    messages_remaining: messages_remaining ?? (messages_limit - (messages_used ?? 0)),
    messages_limit:     messages_limit     ?? null,
    messages_used:      messages_used      ?? (messages_limit !== undefined && messages_remaining !== undefined
                                               ? messages_limit - messages_remaining : null),
    reset_at:           reset_at           ?? null,
    reset_at_ts:        reset_at_ts,
    plan:               plan               ?? 'pro',
    ts:                 Date.now(),
  };
  save();
  console.log(`[account] rimasti:${account.messages_remaining}/${account.messages_limit} reset:${reset_at}`);
  res.json({ ok: true, account });
});

// GET /api/account — ESP32 legge i limiti account
app.get('/api/account', (_req, res) => {
  if (!account) return res.json({ has_data: false });
  res.json({ has_data: true, ...account });
});

// ── Endpoints aggregati per grafici ───────────────────────────────────────────
const IT_DAYS = ['Dom','Lun','Mar','Mer','Gio','Ven','Sab'];

app.get('/api/tokens/hourly', (_req, res) => {
  const now = Date.now(), HOUR = 3_600_000, result = [];
  for (let i = 23; i >= 0; i--) {
    const start = now - (i + 1) * HOUR, end = now - i * HOUR;
    const agg = aggregate(history.filter(e => e.ts >= start && e.ts < end));
    result.push({ label: String(new Date(start).getHours()).padStart(2,'0'),
                  input: agg.input, output: agg.output, cost: parseFloat(agg.cost.toFixed(6)) });
  }
  res.json(result);
});

app.get('/api/tokens/daily', (_req, res) => {
  const now = Date.now(), DAY = 86_400_000, result = [];
  for (let i = 6; i >= 0; i--) {
    const start = now - (i + 1) * DAY, end = now - i * DAY;
    const agg = aggregate(history.filter(e => e.ts >= start && e.ts < end));
    result.push({ label: IT_DAYS[new Date(start).getDay()],
                  input: agg.input, output: agg.output, cost: parseFloat(agg.cost.toFixed(6)) });
  }
  res.json(result);
});

app.get('/api/tokens/weekly', (_req, res) => {
  const now = Date.now(), WEEK = 7 * 86_400_000, result = [];
  for (let i = 3; i >= 0; i--) {
    const start = now - (i + 1) * WEEK, end = now - i * WEEK;
    const agg = aggregate(history.filter(e => e.ts >= start && e.ts < end));
    const d = new Date(start), jan1 = new Date(d.getFullYear(), 0, 1);
    const week = Math.ceil((((d - jan1) / 86400000) + jan1.getDay() + 1) / 7);
    result.push({ label: `W${week}`, input: agg.input, output: agg.output,
                  cost: parseFloat(agg.cost.toFixed(6)) });
  }
  res.json(result);
});

app.get('/health', (_req, res) => res.json({ ok: true }));

load();
app.listen(PORT, '0.0.0.0', () => {
  console.log(`\nClaude Token Monitor → http://0.0.0.0:${PORT}`);
  console.log('  GET  /api/tokens          → totali sessione');
  console.log('  POST /api/tokens          → registra token per messaggio');
  console.log('  GET  /api/account         → limiti account (ESP32)');
  console.log('  POST /api/account         → aggiorna limiti (Tampermonkey)');
  console.log('  GET  /api/tokens/hourly|daily|weekly → grafici\n');
});
