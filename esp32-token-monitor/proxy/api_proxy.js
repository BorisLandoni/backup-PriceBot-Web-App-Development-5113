'use strict';
/*
 * Proxy locale per Anthropic API — intercetta token senza certificati
 *
 * Funziona con: VS Code (Claude Code, Continue.dev, Cursor), script Node.js/Python,
 *               qualsiasi tool che usa l'Anthropic SDK con API key propria.
 *
 * NON funziona con: Claude Desktop, claude.ai web, app mobile
 * (per quelli usa il Tampermonkey userscript o il proxy di sistema)
 *
 * Avvio:
 *   node api_proxy.js
 *
 * Poi imposta la variabile d'ambiente PRIMA di aprire VS Code / terminale:
 *
 *   Mac/Linux:
 *     export ANTHROPIC_BASE_URL=http://localhost:9999
 *
 *   Windows (PowerShell):
 *     $env:ANTHROPIC_BASE_URL="http://localhost:9999"
 *
 *   Windows (CMD):
 *     set ANTHROPIC_BASE_URL=http://localhost:9999
 *
 * Oppure aggiungila in modo permanente nelle impostazioni di VS Code:
 *   settings.json → "terminal.integrated.env.osx": { "ANTHROPIC_BASE_URL": "http://localhost:9999" }
 */

const http  = require('http');
const https = require('https');

const REAL_API_HOST  = 'api.anthropic.com';
const TOKEN_SERVER   = 'http://127.0.0.1:3333';
const PROXY_PORT     = process.env.PROXY_PORT || 9999;

// ── Parsing token da corpo risposta (streaming SSE o JSON normale) ─────────────
function extractTokens(body) {
  let input = 0, output = 0, cacheRead = 0, cacheCreation = 0;

  // Prova JSON diretto (richieste non-streaming)
  try {
    const j = JSON.parse(body);
    if (j.usage) {
      input         = j.usage.input_tokens                  ?? 0;
      output        = j.usage.output_tokens                 ?? 0;
      cacheRead     = j.usage.cache_read_input_tokens       ?? 0;
      cacheCreation = j.usage.cache_creation_input_tokens   ?? 0;
    }
    return { input, output, cacheRead, cacheCreation };
  } catch { /* non è JSON, prova SSE */ }

  // Parsing SSE (streaming)
  for (const line of body.split('\n')) {
    if (!line.startsWith('data: ')) continue;
    const raw = line.slice(6).trim();
    if (!raw || raw === '[DONE]') continue;
    try {
      const evt = JSON.parse(raw);
      if (evt.type === 'message_start' && evt.message?.usage) {
        input         = evt.message.usage.input_tokens                  ?? 0;
        cacheRead     = evt.message.usage.cache_read_input_tokens       ?? 0;
        cacheCreation = evt.message.usage.cache_creation_input_tokens   ?? 0;
      }
      if (evt.type === 'message_delta' && evt.usage) {
        output = evt.usage.output_tokens ?? 0;
      }
    } catch { /* riga malformata, ignora */ }
  }
  return { input, output, cacheRead, cacheCreation };
}

// ── Invia al server ESP32 ────────────────────────────────────────────────────
function reportToServer({ input, output, cacheRead, cacheCreation }) {
  if (input + output === 0) return;

  const payload = JSON.stringify({
    input_tokens:          input,
    output_tokens:         output,
    cache_read_tokens:     cacheRead,
    cache_creation_tokens: cacheCreation,
  });

  const req = http.request({
    hostname: '127.0.0.1',
    port: 3333,
    path: '/api/tokens',
    method: 'POST',
    headers: {
      'Content-Type':   'application/json',
      'Content-Length': Buffer.byteLength(payload),
    },
  }, () => {});
  req.on('error', () => {}); // silenzioso se server è offline
  req.end(payload);

  console.log(`[+] in:${input}  out:${output}  cache_read:${cacheRead}`);
}

// ── Server proxy ──────────────────────────────────────────────────────────────
const server = http.createServer((clientReq, clientRes) => {
  // Raccoglie il body della richiesta
  const chunks = [];
  clientReq.on('data', c => chunks.push(c));
  clientReq.on('end', () => {
    const reqBody = Buffer.concat(chunks);

    const options = {
      hostname: REAL_API_HOST,
      port:     443,
      path:     clientReq.url,
      method:   clientReq.method,
      headers:  {
        ...clientReq.headers,
        host: REAL_API_HOST,        // sovrascrive l'host locale
      },
    };

    const proxyReq = https.request(options, (proxyRes) => {
      clientRes.writeHead(proxyRes.statusCode, proxyRes.headers);

      const resChunks = [];
      proxyRes.on('data', chunk => {
        resChunks.push(chunk);
        clientRes.write(chunk); // pass-through immediato (preserva streaming)
      });

      proxyRes.on('end', () => {
        clientRes.end();
        const body = Buffer.concat(resChunks).toString('utf8');
        const tokens = extractTokens(body);
        reportToServer(tokens);
      });
    });

    proxyReq.on('error', (e) => {
      console.error('Errore verso Anthropic API:', e.message);
      clientRes.writeHead(502);
      clientRes.end('Bad Gateway');
    });

    if (reqBody.length) proxyReq.write(reqBody);
    proxyReq.end();
  });
});

server.listen(PROXY_PORT, '127.0.0.1', () => {
  console.log(`\nAnthropic API Proxy in ascolto su http://127.0.0.1:${PROXY_PORT}`);
  console.log('\nImposta questa variabile d\'ambiente prima di avviare VS Code:\n');
  console.log(`  Mac/Linux:  export ANTHROPIC_BASE_URL=http://localhost:${PROXY_PORT}`);
  console.log(`  Win (PS):   $env:ANTHROPIC_BASE_URL="http://localhost:${PROXY_PORT}"`);
  console.log(`  Win (CMD):  set ANTHROPIC_BASE_URL=http://localhost:${PROXY_PORT}\n`);
});
