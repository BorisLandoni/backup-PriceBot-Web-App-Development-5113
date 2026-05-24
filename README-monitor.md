# Claude Token Monitor

Monitor l'utilizzo del tuo account Claude.ai — messaggi rimasti, reset timer, token per sessione e costi — su un display fisico dedicato, senza consumare nemmeno un token.

## Hardware consigliato

| Componente | Modello | Prezzo |
|---|---|---|
| Single-board computer | **Raspberry Pi 4 2 GB** | ~€45 |
| Display touch HDMI | [5" 800×480 capacitivo (Futuranet)](https://futuranet.it/prodotto/display-touch-screen-5-800x480-pixel/) | €59 |
| MicroSD | 16 GB Classe 10 | ~€8 |

> Il **RPi 4 da 2 GB** è il target principale: login Playwright in ~25s, kiosk 800×480 fluido, headroom sufficiente per il browser headless.  
> Il RPi 3B+ funziona, ma il login impiega ~50s e la RAM è più stretta.

## Come funziona

```
Claude.ai (web/desktop/mobile/VS Code)
       │
       │ uso normale — zero token consumati
       ▼
  claude.ai/settings > Utilizzo
       │  ◄─── polling automatico (Playwright + httpx + cookie)
       │        ogni 60 s (configurabile)
       ▼
┌─────────────────────────┐
│  RPi 4 · FastAPI :8080  │  ← anche Tampermonkey può inviargli dati
│  backend/server.py      │
└────────────┬────────────┘
             │
             ▼
    Chromium kiosk 800×480
    Dashboard touch (5 pagine)
```

## Struttura del repository

```
esp32-token-monitor/          ← versione ESP32 (hardware originale)
│   browser-extension/        ← Tampermonkey userscript (claude.ai → server)
│   firmware/                 ← Arduino sketch per ESP32 + display ILI9488
│   proxy/                    ← proxy per VS Code (Node.js) e Claude Desktop (Python)
│   server/                   ← Node.js/Express aggregator (porta 3333)

rpi-monitor/                  ← versione Raspberry Pi (sistema completo standalone)
│   backend/
│   │   server.py             ← FastAPI (porta 8080) + loop di polling
│   │   claude_client.py      ← login Playwright + poll httpx + DOM scraping
│   │   store.py              ← store dati in RAM + data.json
│   │   requirements.txt
│   frontend/
│   │   index.html            ← SPA 800×480 touch (5 pagine, arc gauge, Chart.js)
│   setup/
│       install.sh            ← installer automatico per RPi OS
│       claude-monitor.service
│       claude-kiosk.desktop
```

## Installazione rapida (Raspberry Pi)

```bash
# 1. Clona il repo
git clone https://github.com/BorisLandoni/esp32-claude-token-monitor.git
cd esp32-claude-token-monitor

# 2. Installa tutto con un comando
bash rpi-monitor/setup/install.sh

# 3. Apri il browser (PC o telefono sulla stessa rete)
#    http://raspberrypi.local:8080
#    Vai in IMPOSTAZIONI → inserisci email e password Claude.ai → Accedi

# 4. Riavvia il RPi — il kiosk si avvia automaticamente
sudo reboot
```

## Pagine della dashboard

| Pagina | Contenuto |
|---|---|
| **ADESSO** | Arc gauge sessione corrente (%), countdown reset, limiti settimanali, token/costo sessione |
| **ORA** | Grafico 24h a barre (input ciano / output verde) |
| **GIORNO** | Grafico 7 giorni |
| **SETTIMANA** | Grafico 4 settimane |
| **IMPOSTAZIONI** | Login Claude.ai, tema, intervallo aggiornamento |

## Dati monitorati

La dashboard legge dalla pagina `claude.ai/settings > Utilizzo`:

- **Sessione corrente**: % usato + countdown reset (es. "Si ripristina tra 3h 38m")
- **Settimanale**: % usato + giorno/ora di reset (es. "sab 17:59")
- **Token per messaggio**: input, output, cache (via Tampermonkey o proxy)
- **Costo stimato** in USD (prezzi claude-sonnet-4-x)

> Non viene effettuata nessuna chiamata alle API Anthropic a pagamento.  
> Il polling usa i cookie di sessione del browser per leggere gli stessi endpoint che claude.ai chiama già.

## Versione ESP32 (opzionale)

Se preferisci un display TFT standalone senza RPi:

- Hardware: [Makerfabs ESP32-S3 Parallel TFT 3.5"](https://www.makerfabs.com/esp32-s3-parallel-tft-with-touch-3-5-inch.html) (ILI9488, 320×480)
- Firmware: `esp32-token-monitor/firmware/`
- Richiede `server.js` su un PC nella stessa rete + Tampermonkey

## Tampermonkey (opzionale — per token per messaggio)

Installa `esp32-token-monitor/browser-extension/claude_monitor.user.js` su Tampermonkey  
e imposta l'URL del server su `http://IP-del-RPi:8080`.

## Licenza

MIT — Boris Landoni
