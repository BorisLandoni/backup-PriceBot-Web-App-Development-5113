// ==UserScript==
// @name         Claude Monitor → ESP32  (token + limiti account)
// @namespace    https://github.com/BorisLandoni/esp32-claude-token-monitor
// @version      2.0.0
// @description  Legge automaticamente i token usati e i limiti account da claude.ai — nessuna installazione altrove
// @author       Boris Landoni
// @match        https://claude.ai/*
// @grant        GM_xmlhttpRequest
// @grant        GM_getValue
// @grant        GM_setValue
// @grant        unsafeWindow
// @connect      localhost
// @connect      127.0.0.1
// @run-at       document-start
// ==/UserScript==

/*
 * COME FUNZIONA
 * ─────────────
 * Claude.ai fa già da sola chiamate API per sapere quanti messaggi ti restano
 * (quelle che aggiornano il contatore in sidebar). Lo script le intercetta
 * silenziosamente e invia i dati al server locale senza consumare token.
 *
 * COSA TRACCIA
 * ────────────
 * 1. Limiti account  — messaggi rimasti, totale, orario di reset
 *    Aggiornato ogni volta che claude.ai carica/aggiorna il contatore
 *    → funziona anche dopo sessioni da VS Code, mobile, Desktop
 *    (la prossima volta che apri claude.ai nel browser il display si aggiorna)
 *
 * 2. Token per messaggio — input + output + cache per ogni risposta
 *    Aggiornato in tempo reale durante ogni conversazione
 *
 * SETUP
 * ─────
 * 1. Installa Tampermonkey nel browser
 * 2. Incolla questo file come nuovo script
 * 3. Assicurati che server.js sia in esecuzione (npm start)
 * 4. Apri claude.ai — i dati arrivano automaticamente
 */

(function () {
  'use strict';

  const SERVER = GM_getValue('server_url', 'http://localhost:3333');
  const DEBUG  = false;
  const log    = (...a) => DEBUG && console.log('[Claude Monitor]', ...a);

  // ── POST al server locale ─────────────────────────────────────────────────
  function post(path, data) {
    GM_xmlhttpRequest({
      method:  'POST',
      url:     SERVER + path,
      headers: { 'Content-Type': 'application/json' },
      data:    JSON.stringify(data),
      onerror: () => log('server offline'),
    });
  }

  // ── Ricerca ricorsiva di dati di utilizzo in qualsiasi oggetto JSON ───────
  // claude.ai non ha un endpoint documentato; questa funzione riconosce
  // i campi relativi ai limiti indipendentemente dalla struttura esatta.
  function extractAccountLimits(obj, depth = 0) {
    if (!obj || typeof obj !== 'object' || depth > 8) return null;
    if (Array.isArray(obj)) {
      for (const item of obj) {
        const r = extractAccountLimits(item, depth + 1);
        if (r) return r;
      }
      return null;
    }

    const keys = Object.keys(obj);
    const keyStr = keys.join(' ').toLowerCase();

    // Riconosce oggetti che contengono dati di limite/utilizzo
    const looksLikeLimit =
      keyStr.includes('remaining') ||
      keyStr.includes('messages_left') ||
      (keyStr.includes('limit') && (keyStr.includes('used') || keyStr.includes('reset'))) ||
      keyStr.includes('rate_limit') ||
      keyStr.includes('quota') ||
      (keyStr.includes('usage') && keyStr.includes('reset'));

    if (looksLikeLimit) {
      const r = {};
      for (const [k, v] of Object.entries(obj)) {
        const kl = k.toLowerCase();
        if (kl.includes('remaining') || kl.includes('left'))  r.messages_remaining = Number(v);
        if ((kl.includes('limit') || kl.includes('max') || kl.includes('total')) && typeof v === 'number' && v > 0)
          r.messages_limit = v;
        if ((kl.includes('used') || kl.includes('consumed') || kl.includes('count')) && typeof v === 'number')
          r.messages_used = v;
        if (kl.includes('reset') && v) r.reset_at = v;
        if (kl.includes('plan') || kl.includes('tier')) r.plan = String(v);
      }
      if (r.messages_remaining !== undefined || r.messages_limit !== undefined) {
        // Ricostruisce used se mancante
        if (r.messages_used === undefined && r.messages_limit && r.messages_remaining !== undefined)
          r.messages_used = r.messages_limit - r.messages_remaining;
        return r;
      }
    }

    // Ricerca nei sottoggetti
    for (const v of Object.values(obj)) {
      if (v && typeof v === 'object') {
        const found = extractAccountLimits(v, depth + 1);
        if (found) return found;
      }
    }
    return null;
  }

  // ── Analizza qualsiasi risposta JSON da claude.ai ─────────────────────────
  function analyzeJsonResponse(url, json) {
    // 1. Cerca limiti account
    const limits = extractAccountLimits(json);
    if (limits && (limits.messages_remaining !== undefined || limits.messages_limit)) {
      log('Limiti account trovati:', limits, 'da', url);
      post('/api/account', limits);
    }

    // 2. Cerca token per messaggio (endpoint non-streaming)
    if (json.usage && (json.usage.input_tokens || json.usage.output_tokens)) {
      log('Token (JSON diretto):', json.usage);
      post('/api/tokens', {
        input_tokens:          json.usage.input_tokens          ?? 0,
        output_tokens:         json.usage.output_tokens         ?? 0,
        cache_read_tokens:     json.usage.cache_read_input_tokens    ?? 0,
        cache_creation_tokens: json.usage.cache_creation_input_tokens ?? 0,
      });
    }
  }

  // ── Legge token da flusso SSE (risposte streaming) ───────────────────────
  async function readTokensFromStream(response) {
    if (!response.body) return;
    const reader  = response.body.getReader();
    const decoder = new TextDecoder();
    let inputTokens = 0, outputTokens = 0, cacheRead = 0, cacheCreation = 0;

    try {
      while (true) {
        const { done, value } = await reader.read();
        if (done) break;
        for (const line of decoder.decode(value, { stream: true }).split('\n')) {
          if (!line.startsWith('data: ')) continue;
          const raw = line.slice(6).trim();
          if (!raw || raw === '[DONE]') continue;
          try {
            const evt = JSON.parse(raw);
            if (evt.type === 'message_start' && evt.message?.usage) {
              inputTokens   = evt.message.usage.input_tokens                  ?? 0;
              cacheRead     = evt.message.usage.cache_read_input_tokens       ?? 0;
              cacheCreation = evt.message.usage.cache_creation_input_tokens   ?? 0;
            }
            if (evt.type === 'message_delta' && evt.usage) {
              outputTokens  = evt.usage.output_tokens ?? 0;
            }
          } catch { /* riga non JSON, ignora */ }
        }
      }
    } catch { /* stream interrotto */ } finally {
      reader.releaseLock();
    }

    if (inputTokens + outputTokens > 0) {
      log(`Token streaming: in=${inputTokens} out=${outputTokens}`);
      post('/api/tokens', {
        input_tokens: inputTokens, output_tokens: outputTokens,
        cache_read_tokens: cacheRead, cache_creation_tokens: cacheCreation,
      });
    }
  }

  // ── Intercetta fetch ───────────────────────────────────────────────────────
  const _fetch = unsafeWindow.fetch.bind(unsafeWindow);
  unsafeWindow.fetch = async function (input, init) {
    const url = (typeof input === 'string') ? input
              : (input instanceof Request)  ? input.url : String(input);

    const response = await _fetch(input, init);
    const ct = response.headers.get('content-type') || '';

    if (url.includes('claude.ai') || url.includes('anthropic.com')) {
      if (url.includes('/completion')) {
        // Risposta streaming — leggi SSE
        readTokensFromStream(response.clone());
      } else if (ct.includes('application/json')) {
        // Qualsiasi altra risposta JSON — cerca dati di utilizzo
        response.clone().json()
          .then(json => analyzeJsonResponse(url, json))
          .catch(() => {});
      }
    }

    return response;
  };

  // ── Intercetta anche XMLHttpRequest (alcune versioni di claude.ai lo usano)
  const _open = unsafeWindow.XMLHttpRequest.prototype.open;
  const _send = unsafeWindow.XMLHttpRequest.prototype.send;

  unsafeWindow.XMLHttpRequest.prototype.open = function (method, url, ...rest) {
    this._monitorUrl = url;
    return _open.call(this, method, url, ...rest);
  };

  unsafeWindow.XMLHttpRequest.prototype.send = function (...args) {
    this.addEventListener('load', function () {
      const url = this._monitorUrl || '';
      if (!(url.includes('claude.ai') || url.includes('anthropic.com'))) return;
      try {
        const json = JSON.parse(this.responseText);
        analyzeJsonResponse(url, json);
      } catch { /* non JSON */ }
    });
    return _send.apply(this, args);
  };

  log('Claude Monitor v2.0 attivo su', location.hostname);
})();
