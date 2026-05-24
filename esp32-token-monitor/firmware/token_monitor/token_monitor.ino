/*
 * ╔══════════════════════════════════════════════════════════╗
 * ║     CLAUDE TOKEN MONITOR — ESP32-S3 + TFT 3.5" ILI9488  ║
 * ║     Display parallelo 8-bit, touch capacitivo FT6236     ║
 * ╚══════════════════════════════════════════════════════════╝
 *
 * Hardware: Futuranet / Makerfabs ESP32-S3 Parallel TFT 3.5"
 *   Controller: ILI9488  —  320×480 px  —  parallelo 8-bit
 *   Touch:      FT6236   —  capacitivo  —  I2C
 *
 * Librerie (Arduino Library Manager):
 *   • LovyanGFX  (rlooz)            — display
 *   • ArduinoJson (Benoit Blanchon) — parsing JSON v7+
 *
 * Impostazioni Arduino IDE:
 *   Board  : ESP32S3 Dev Module
 *   PSRAM  : OPI PSRAM  ← OBBLIGATORIO (sprite 300KB)
 *   Flash  : 16MB
 *   USB    : Hardware CDC
 *
 * Touch pin (verifica sul tuo schematico se diversi):
 *   TP_SDA = GPIO 38
 *   TP_SCL = GPIO 39
 *   TP_INT = GPIO 40  (non usato nel codice, opzionale)
 */

#include <LovyanGFX.hpp>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>

// ── Configurazione utente ─────────────────────────────────────────────────────
#define WIFI_SSID      "TUO_SSID"
#define WIFI_PASSWORD  "TUA_PASSWORD"
#define SERVER_BASE    "http://192.168.1.100:3333"  // IP del PC con server.js
#define POLL_MS        30000                         // polling ogni 30 secondi
#define BRIGHTNESS     220                           // retroilluminazione 0-255

// Touch FT6236 (I2C)
#define TP_SDA  38
#define TP_SCL  39
#define TP_ADDR 0x38

// ── LovyanGFX: ILI9488 8-bit parallelo ───────────────────────────────────────
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9488 _panel;
  lgfx::Bus_Parallel8 _bus;
  lgfx::Light_PWM     _bl;
public:
  LGFX() {
    { // Bus parallelo 8-bit
      auto cfg    = _bus.config();
      cfg.freq_write = 20000000;
      cfg.pin_wr  =  7;
      cfg.pin_rd  =  6;
      cfg.pin_rs  =  8;  // DC (Data/Command)
      cfg.pin_d0  = 12;
      cfg.pin_d1  = 13;
      cfg.pin_d2  = 14;
      cfg.pin_d3  = 15;
      cfg.pin_d4  = 16;
      cfg.pin_d5  = 21;
      cfg.pin_d6  =  5;
      cfg.pin_d7  =  4;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    { // Pannello ILI9488 320×480
      auto cfg = _panel.config();
      cfg.pin_cs   =  9;
      cfg.pin_rst  = 34;
      cfg.panel_width  = 320;
      cfg.panel_height = 480;
      cfg.readable     = false;
      cfg.invert       = false;
      cfg.rgb_order    = false;
      cfg.dlen_16bit   = false;
      cfg.bus_shared   = false;
      _panel.config(cfg);
    }
    { // Retroilluminazione PWM
      auto cfg = _bl.config();
      cfg.pin_bl      = 45;
      cfg.invert      = false;
      cfg.freq        = 44100;
      cfg.pwm_channel = 7;
      _bl.config(cfg);
      _panel.setLight(&_bl);
    }
    setPanel(&_panel);
  }
};

static LGFX        lcd;
static LGFX_Sprite spr(&lcd);  // frame buffer in PSRAM

// ── Palette colori (RGB888) ───────────────────────────────────────────────────
static constexpr uint32_t C_BG      = 0x0D1B2A;  // sfondo navy scuro
static constexpr uint32_t C_HEADER  = 0x0A2540;  // intestazione
static constexpr uint32_t C_INPUT   = 0x00C8FF;  // cyan — token input
static constexpr uint32_t C_OUTPUT  = 0x00E87A;  // verde — token output
static constexpr uint32_t C_COST    = 0xFFCC00;  // oro — costo USD
static constexpr uint32_t C_GRID    = 0x162534;  // griglia
static constexpr uint32_t C_TEXT    = 0xEEF2F7;  // testo principale
static constexpr uint32_t C_MUTED   = 0x4A6A80;  // testo secondario
static constexpr uint32_t C_DIVIDER = 0x1E3A52;  // separatori
static constexpr uint32_t C_NAV     = 0x0C1B28;  // nav inattivo
static constexpr uint32_t C_NAV_ACT = 0x183550;  // nav attivo
static constexpr uint32_t C_WHITE   = 0xFFFFFF;

// ── Layout (portrait 320×480) ─────────────────────────────────────────────────
static constexpr int SW      = 320;
static constexpr int SH      = 480;
static constexpr int HDR_H   = 46;
static constexpr int NAV_H   = 78;
static constexpr int NAV_Y   = SH - NAV_H;       // y=402
static constexpr int CONT_Y  = HDR_H;             // y=46
static constexpr int CONT_H  = SH - HDR_H - NAV_H; // 356px

// ── Vista attiva ──────────────────────────────────────────────────────────────
enum View { V_NOW = 0, V_HOURLY, V_DAILY, V_WEEKLY };
static View currentView = V_NOW;

// ── Strutture dati ────────────────────────────────────────────────────────────
struct Period {
  char  label[8];
  long  input;
  long  output;
  float cost;
};

static Period periods[24];
static int    nPeriods = 0;

struct LiveStats {
  long  input, output, cache_read;
  int   requests;
  float cost;
  bool  valid = false;
};
static LiveStats live;

// ── Touch FT6236 ─────────────────────────────────────────────────────────────
struct TouchPt { int x, y; bool pressed; };

static TouchPt readTouch() {
  Wire.beginTransmission(TP_ADDR);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return {0, 0, false};
  Wire.requestFrom((uint8_t)TP_ADDR, (uint8_t)6);
  if (Wire.available() < 6) return {0, 0, false};
  uint8_t count = Wire.read();
  uint8_t xh = Wire.read(), xl = Wire.read();
  Wire.read(); // touch ID nibble
  uint8_t yh = Wire.read(), yl = Wire.read();
  return {
    ((xh & 0x0F) << 8) | xl,
    ((yh & 0x0F) << 8) | yl,
    count > 0
  };
}

// ── Helpers ───────────────────────────────────────────────────────────────────
static String fmtK(long n) {
  if (n >= 1000000L) return String(n / 1000000.0f, 2) + "M";
  if (n >= 1000L)    return String(n / 1000.0f, 1) + "K";
  return String(n);
}

static String fmtCost(float c) {
  char buf[12];
  if (c < 0.01f)  snprintf(buf, sizeof(buf), "$%.5f", c);
  else if (c < 1) snprintf(buf, sizeof(buf), "$%.4f", c);
  else            snprintf(buf, sizeof(buf), "$%.2f", c);
  return String(buf);
}

// ── WiFi ──────────────────────────────────────────────────────────────────────
static void connectWiFi() {
  spr.fillSprite(C_BG);
  spr.setTextColor(C_INPUT);
  spr.setTextSize(2);
  spr.setCursor(28, 200);
  spr.print("Connessione WiFi");
  spr.setTextColor(C_MUTED);
  spr.setTextSize(1);
  spr.setCursor(28, 228);
  spr.print(WIFI_SSID);
  spr.pushSprite(0, 0);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int n = 0;
  while (WiFi.status() != WL_CONNECTED && n < 40) {
    delay(500);
    spr.fillRect(28 + (n % 22) * 6, 255, 4, 4, C_INPUT);
    spr.pushSprite(0, 0);
    n++;
  }
}

// ── HTTP fetch ────────────────────────────────────────────────────────────────
static bool httpGet(const char* endpoint, JsonDocument& doc) {
  if (WiFi.status() != WL_CONNECTED) { WiFi.reconnect(); return false; }
  HTTPClient http;
  http.begin(String(SERVER_BASE) + endpoint);
  http.setTimeout(6000);
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  return !err;
}

static void fetchLive() {
  JsonDocument doc;
  if (!httpGet("/api/tokens", doc)) return;
  live.input    = doc["total_input"]      | 0L;
  live.output   = doc["total_output"]     | 0L;
  live.cache_read = doc["total_cache_read"] | 0L;
  live.requests = doc["requests_count"]   | 0;
  live.cost     = doc["cost_usd"]         | 0.0f;
  live.valid    = true;
}

static void fetchPeriods(const char* endpoint) {
  JsonDocument doc;
  if (!httpGet(endpoint, doc)) return;
  nPeriods = 0;
  for (JsonObject o : doc.as<JsonArray>()) {
    if (nPeriods >= 24) break;
    strlcpy(periods[nPeriods].label, o["label"] | "?", 8);
    periods[nPeriods].input  = o["input"]  | 0L;
    periods[nPeriods].output = o["output"] | 0L;
    periods[nPeriods].cost   = o["cost"]   | 0.0f;
    nPeriods++;
  }
}

static void refresh() {
  fetchLive();
  const char* ep[] = { nullptr, "/api/tokens/hourly", "/api/tokens/daily", "/api/tokens/weekly" };
  if (currentView != V_NOW && ep[currentView]) fetchPeriods(ep[currentView]);
}

// ── Componenti UI ─────────────────────────────────────────────────────────────

static void drawHeader(const char* subtitle) {
  spr.fillRect(0, 0, SW, HDR_H, C_HEADER);
  // Pallino verde "live"
  spr.fillCircle(16, HDR_H / 2, 7, C_OUTPUT);
  spr.fillCircle(16, HDR_H / 2, 4, C_BG);
  spr.fillCircle(16, HDR_H / 2, 2, C_OUTPUT);
  // Titolo
  spr.setTextColor(C_TEXT);
  spr.setTextSize(2);
  spr.setCursor(30, 13);
  spr.print("CLAUDE");
  spr.setTextColor(C_INPUT);
  spr.print(" MONITOR");
  // Sottotitolo destra
  spr.setTextColor(C_MUTED);
  spr.setTextSize(1);
  int sw = strlen(subtitle) * 6;
  spr.setCursor(SW - sw - 6, 18);
  spr.print(subtitle);
  spr.drawFastHLine(0, HDR_H - 1, SW, C_DIVIDER);
}

static void drawNavBar() {
  const char* labels[] = { "NOW", "ORARIO", "GIORNO", "SETT." };
  spr.drawFastHLine(0, NAV_Y, SW, C_DIVIDER);
  for (int i = 0; i < 4; i++) {
    int bx = i * 80;
    bool active = (i == (int)currentView);
    spr.fillRect(bx, NAV_Y + 1, 80, NAV_H - 1, active ? C_NAV_ACT : C_NAV);
    if (active) {
      // Barra accent in cima
      spr.fillRect(bx + 6, NAV_Y + 1, 68, 3, C_INPUT);
    }
    // Icona
    uint32_t icol = active ? C_INPUT : C_MUTED;
    int cx = bx + 40, iy = NAV_Y + 28;
    if (i == 0) {
      // Orologio stilizzato
      spr.drawCircle(cx, iy, 13, icol);
      spr.drawFastVLine(cx, iy - 8, 8, icol);
      spr.drawFastHLine(cx, iy, 6, icol);
    } else {
      // Istogramma stilizzato (3 barre di altezza crescente)
      int bh[3] = { 8, 13, 18 };
      for (int b = 0; b < 3; b++) {
        spr.fillRect(cx - 13 + b * 10, iy + 13 - bh[b], 7, bh[b], icol);
      }
    }
    // Etichetta testo
    spr.setTextColor(active ? C_TEXT : C_MUTED);
    spr.setTextSize(1);
    int lw = strlen(labels[i]) * 6;
    spr.setCursor(bx + (80 - lw) / 2, NAV_Y + 58);
    spr.print(labels[i]);
  }
}

// ── Schermata NOW ─────────────────────────────────────────────────────────────
static void drawNowScreen() {
  drawHeader("ADESSO");

  if (!live.valid) {
    spr.setTextColor(C_MUTED);
    spr.setTextSize(1);
    spr.setCursor(80, 220);
    spr.print("Caricamento...");
    drawNavBar();
    return;
  }

  long total = live.input + live.output;

  // Funzione riga statistica
  auto statRow = [&](int y, const char* name, long value, uint32_t col, long barMax) {
    // Nome
    spr.setTextColor(C_MUTED);
    spr.setTextSize(1);
    spr.setCursor(14, y);
    spr.print(name);
    // Valore
    String s = fmtK(value);
    spr.setTextColor(col);
    spr.setTextSize(2);
    spr.setCursor(SW - (int)s.length() * 12 - 14, y - 3);
    spr.print(s);
    // Barra di progresso
    int bw = (barMax > 0) ? (int)((value * 220L) / barMax) : 0;
    if (bw > 220) bw = 220;
    spr.fillRoundRect(14, y + 14, bw,      6, 3, col);
    spr.fillRoundRect(14 + bw, y + 14, 220 - bw, 6, 3, C_GRID);
  };

  int y = CONT_Y + 16;
  statRow(y, "TOKEN INPUT",  live.input,      C_INPUT,  total > 0 ? total : 1); y += 58;
  statRow(y, "TOKEN OUTPUT", live.output,     C_OUTPUT, total > 0 ? total : 1); y += 58;
  statRow(y, "CACHE HIT",    live.cache_read, C_MUTED,  live.input > 0 ? live.input : 1); y += 58;

  spr.drawFastHLine(14, y - 6, SW - 28, C_DIVIDER);

  // Richieste
  spr.setTextColor(C_MUTED);
  spr.setTextSize(1);
  spr.setCursor(14, y);
  spr.print("RICHIESTE API");
  spr.setTextColor(C_TEXT);
  spr.setTextSize(2);
  String req = String(live.requests);
  spr.setCursor(SW - (int)req.length() * 12 - 14, y - 3);
  spr.print(req);
  y += 48;

  // Box costo — elemento principale
  spr.fillRoundRect(14, y, SW - 28, 56, 8, C_HEADER);
  spr.drawRoundRect(14, y, SW - 28, 56, 8, C_DIVIDER);
  spr.setTextColor(C_MUTED);
  spr.setTextSize(1);
  spr.setCursor(26, y + 9);
  spr.print("COSTO SESSIONE USD");
  String costStr = fmtCost(live.cost);
  spr.setTextColor(C_COST);
  spr.setTextSize(3);
  int cw = costStr.length() * 18;
  spr.setCursor((SW - cw) / 2, y + 24);
  spr.print(costStr);

  drawNavBar();
}

// ── Schermata grafici a barre ─────────────────────────────────────────────────
static void drawChartScreen() {
  const char* titles[] = { "", "ULTIME 24 ORE", "ULTIMI 7 GIORNI", "ULTIME 4 SETTIMANE" };
  drawHeader(titles[(int)currentView]);

  // Calcola totali del periodo
  long  totIn = 0, totOut = 0;
  float totCost = 0;
  for (int i = 0; i < nPeriods; i++) {
    totIn   += periods[i].input;
    totOut  += periods[i].output;
    totCost += periods[i].cost;
  }

  // Strip riepilogativa
  int sy = CONT_Y + 8;
  spr.setTextSize(1);
  spr.setTextColor(C_MUTED);
  spr.setCursor(14, sy);
  spr.print("IN ");
  spr.setTextColor(C_INPUT);
  spr.print(fmtK(totIn));
  spr.setTextColor(C_MUTED);
  spr.print("   OUT ");
  spr.setTextColor(C_OUTPUT);
  spr.print(fmtK(totOut));

  spr.setTextColor(C_MUTED);
  spr.setCursor(14, sy + 14);
  spr.print("COSTO PERIODO ");
  spr.setTextColor(C_COST);
  spr.setTextSize(2);
  spr.print(fmtCost(totCost));

  // Area grafico
  const int CX = 48;                   // left margin (asse Y)
  const int CY = CONT_Y + 46;          // top del grafico
  const int CW = SW - CX - 8;          // larghezza plot
  const int CH = NAV_Y - CY - 26;      // altezza plot (lascia spazio label X)

  // Sfondo grafico
  spr.fillRect(CX, CY, CW, CH, C_GRID);

  // Trova valore massimo
  long maxVal = 1;
  for (int i = 0; i < nPeriods; i++) {
    long t = periods[i].input + periods[i].output;
    if (t > maxVal) maxVal = t;
  }

  // Linee griglia Y + etichette asse Y
  for (int g = 1; g <= 4; g++) {
    int gy = CY + CH - (g * CH / 4);
    spr.drawFastHLine(CX, gy, CW, 0x1E3A50);
    String lbl = fmtK((maxVal * g) / 4);
    spr.setTextColor(C_MUTED);
    spr.setTextSize(1);
    spr.setCursor(CX - (int)lbl.length() * 6 - 2, gy - 4);
    spr.print(lbl);
  }

  // Baseline
  spr.drawFastHLine(CX, CY + CH, CW, C_DIVIDER);

  if (nPeriods == 0) {
    spr.setTextColor(C_MUTED);
    spr.setTextSize(1);
    spr.setCursor(CX + 60, CY + CH / 2 - 4);
    spr.print("Nessun dato");
  } else {
    // Larghezza barra e gap
    const int GAP = (nPeriods > 10) ? 1 : (nPeriods > 4 ? 2 : 4);
    int barW = (CW - GAP * (nPeriods - 1)) / nPeriods;
    if (barW < 3) barW = 3;

    for (int i = 0; i < nPeriods; i++) {
      int bx = CX + i * (barW + GAP);
      long total = periods[i].input + periods[i].output;

      if (total > 0) {
        int totalH  = (int)((total * (long)CH) / maxVal);
        if (totalH > CH) totalH = CH;
        int inputH  = (int)((periods[i].input * (long)totalH) / total);
        int outputH = totalH - inputH;
        int by      = CY + CH - totalH;

        // Barra OUTPUT (verde, in alto)
        if (outputH > 0)
          spr.fillRect(bx, by, barW, outputH, C_OUTPUT);
        // Barra INPUT (cyan, in basso)
        if (inputH > 0)
          spr.fillRect(bx, by + outputH, barW, inputH, C_INPUT);

        // Highlight bordo superiore
        spr.drawFastHLine(bx, by, barW, C_WHITE);
      }

      // Etichetta asse X (tutte se ≤7 periodi, ogni 6 se orario)
      bool showLbl = (nPeriods <= 7) || (i % 6 == 0) || (i == nPeriods - 1);
      if (showLbl) {
        spr.setTextColor(C_MUTED);
        spr.setTextSize(1);
        int lx = bx + barW / 2 - (int)strlen(periods[i].label) * 3;
        spr.setCursor(lx, CY + CH + 4);
        spr.print(periods[i].label);
      }
    }
  }

  // Legenda
  int ly = NAV_Y - 19;
  spr.fillRect(CX,      ly, 10, 8, C_INPUT);
  spr.fillRect(CX + 52, ly, 10, 8, C_OUTPUT);
  spr.setTextColor(C_MUTED);
  spr.setTextSize(1);
  spr.setCursor(CX + 13, ly);
  spr.print("Input");
  spr.setCursor(CX + 65, ly);
  spr.print("Output");

  drawNavBar();
}

// ── Render frame ──────────────────────────────────────────────────────────────
static void render() {
  spr.fillSprite(C_BG);
  if (currentView == V_NOW) drawNowScreen();
  else                      drawChartScreen();
  spr.pushSprite(0, 0);
}

// ── Schermata di boot ─────────────────────────────────────────────────────────
static void showBoot() {
  spr.fillSprite(C_BG);
  // Logo
  spr.fillRoundRect(80, 140, 160, 70, 12, C_HEADER);
  spr.drawRoundRect(80, 140, 160, 70, 12, C_INPUT);
  spr.setTextColor(C_INPUT);
  spr.setTextSize(2);
  spr.setCursor(104, 155);
  spr.print("CLAUDE");
  spr.setTextColor(C_OUTPUT);
  spr.setCursor(96, 177);
  spr.print("MONITOR");
  // Versione
  spr.setTextColor(C_MUTED);
  spr.setTextSize(1);
  spr.setCursor(116, 230);
  spr.print("v2.0  ESP32-S3");
  spr.pushSprite(0, 0);
  delay(1200);
}

// ── Setup / Loop ──────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  lcd.init();
  lcd.setRotation(0);
  lcd.setBrightness(BRIGHTNESS);

  spr.setColorDepth(16);
  bool ok = spr.createSprite(SW, SH);
  if (!ok) {
    // PSRAM non disponibile: usa draw diretto (leggero flickering)
    Serial.println("ATTENZIONE: PSRAM non trovata. Abilita OPI PSRAM nelle impostazioni board.");
  }

  showBoot();
  Wire.begin(TP_SDA, TP_SCL);
  connectWiFi();

  refresh();
  render();
}

static unsigned long lastPoll    = 0;
static bool          wasPressed  = false;

void loop() {
  unsigned long now = millis();

  // Auto-refresh periodico
  if (now - lastPoll >= POLL_MS) {
    lastPoll = now;
    refresh();
    render();
  }

  // Gestione touch
  TouchPt tp = readTouch();
  if (tp.pressed && !wasPressed) {
    wasPressed = true;
    if (tp.y >= NAV_Y) {
      int btn = tp.x / 80;
      if (btn < 4) {
        View next = (View)btn;
        if (next != currentView) {
          currentView = next;
          nPeriods    = 0;
          refresh();
          render();
          lastPoll = now;
        }
      }
    }
  } else if (!tp.pressed) {
    wasPressed = false;
  }

  delay(40);
}
