// ==UserScript==
// @name         Claude Token Monitor → ESP32
// @namespace    https://github.com/BorisLandoni/esp32-claude-token-monitor
// @version      1.0.0
// @description  Intercetta silenziosamente i token usati su Claude.ai e li invia al monitor ESP32
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

(function () {
  'use strict';

  // ── Configurazione ────────────────────────────────────────────────────────
  const SERVER   = GM_getValue('server_url', 'http://localhost:3333');
  const DEBUG    = false;   // true = mostra log in console

  const log = (...args) => DEBUG && console.log('[Claude Monitor]', ...args);

  // ── Intercetta fetch ──────────────────────────────────────────────────────
  // Claude.ai usa fetch() per chiamare la propria API di completamento.
  // Cloniamo ogni risposta verso /completion e leggiamo il flusso SSE.

  const _fetch = unsafeWindow.fetch.bind(unsafeWindow);

  unsafeWindow.fetch = async function (input, init) {
    const url = typeof input === 'string' ? input
              : (input instanceof Request ? input.url : String(input));

    const response = await _fetch(input, init);

    // Intercetta solo le chiamate al completamento (streaming)
    if (url && url.includes('/completion')) {
      log('Intercettata risposta di completamento');
      parseStream(response.clone());
    }

    return response;
  };

  // ── Legge il flusso SSE e cerca i dati di utilizzo ────────────────────────
  async function parseStream(response) {
    if (!response.body) return;

    const reader  = response.body.getReader();
    const decoder = new TextDecoder();
    let inputTokens  = 0;
    let outputTokens = 0;
    let cacheRead    = 0;
    let cacheCreation = 0;

    try {
      while (true) {
        const { done, value } = await reader.read();
        if (done) break;

        const chunk = decoder.decode(value, { stream: true });

        // Ogni riga SSE inizia con "data: "
        for (const line of chunk.split('\n')) {
          if (!line.startsWith('data: ')) continue;
          const raw = line.slice(6).trim();
          if (!raw || raw === '[DONE]') continue;

          let evt;
          try { evt = JSON.parse(raw); } catch { continue; }

          // message_start → token di INPUT
          if (evt.type === 'message_start' && evt.message?.usage) {
            const u = evt.message.usage;
            inputTokens   = u.input_tokens            ?? 0;
            cacheRead     = u.cache_read_input_tokens  ?? 0;
            cacheCreation = u.cache_creation_input_tokens ?? 0;
            log(`Input: ${inputTokens}  cache_read: ${cacheRead}`);
          }

          // message_delta → token di OUTPUT (valore finale)
          if (evt.type === 'message_delta' && evt.usage) {
            outputTokens = evt.usage.output_tokens ?? 0;
            log(`Output: ${outputTokens}`);
          }
        }
      }
    } catch (e) {
      log('Errore lettura stream:', e.message);
    } finally {
      reader.releaseLock();
    }

    if (inputTokens > 0 || outputTokens > 0) {
      sendToServer(inputTokens, outputTokens, cacheRead, cacheCreation);
    }
  }

  // ── Invia al server locale ────────────────────────────────────────────────
  // GM_xmlhttpRequest bypassa il CORS del browser → può parlare con localhost
  function sendToServer(input, output, cacheRead, cacheCreation) {
    const payload = JSON.stringify({
      input_tokens:          input,
      output_tokens:         output,
      cache_read_tokens:     cacheRead,
      cache_creation_tokens: cacheCreation,
    });

    GM_xmlhttpRequest({
      method:  'POST',
      url:     `${SERVER}/api/tokens`,
      headers: { 'Content-Type': 'application/json' },
      data:    payload,
      onload:  (r) => log(`Server risposto ${r.status} | in:${input} out:${output}`),
      onerror: ()  => log('Server non raggiungibile (server.js in esecuzione?)'),
    });
  }

})();
