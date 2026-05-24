/*
 * ╔══════════════════════════════════════════════════════════╗
 * ║     CLAUDE TOKEN MONITOR — ESP32-S3 + TFT 3.5" ILI9488  ║
 * ╚══════════════════════════════════════════════════════════╝
 *
 * Hardware: Futuranet / Makerfabs ESP32-S3 Parallel TFT 3.5"
 *   Controller ILI9488 · 320×480 px · parallelo 8-bit · touch FT6236
 *
 * Librerie (Arduino Library Manager):
 *   LovyanGFX · ArduinoJson v7+
 *
 * Arduino IDE → Board: ESP32S3 Dev Module · PSRAM: OPI PSRAM · Flash: 16MB
 */

#include <LovyanGFX.hpp>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <time.h>

// ── Configurazione ────────────────────────────────────────────────────────────
#define WIFI_SSID      "TUO_SSID"
#define WIFI_PASSWORD  "TUA_PASSWORD"
#define SERVER_BASE    "http://192.168.1.100:3333"
#define POLL_MS        30000
#define BRIGHTNESS     220
#define TIMEZONE_OFFSET_H  1    // UTC+1 Italia (ora solare); usa 2 per ora legale

#define TP_SDA  38
#define TP_SCL  39
#define TP_ADDR 0x38

// ── LovyanGFX ─────────────────────────────────────────────────────────────────
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9488 _panel;
  lgfx::Bus_Parallel8 _bus;
  lgfx::Light_PWM     _bl;
public:
  LGFX() {
    { auto c = _bus.config(); c.freq_write=20000000; c.pin_wr=7; c.pin_rd=6; c.pin_rs=8;
      c.pin_d0=12; c.pin_d1=13; c.pin_d2=14; c.pin_d3=15;
      c.pin_d4=16; c.pin_d5=21; c.pin_d6=5;  c.pin_d7=4;
      _bus.config(c); _panel.setBus(&_bus); }
    { auto c = _panel.config(); c.pin_cs=9; c.pin_rst=34;
      c.panel_width=320; c.panel_height=480;
      c.readable=false; c.invert=false; c.rgb_order=false; c.dlen_16bit=false; c.bus_shared=false;
      _panel.config(c); }
    { auto c = _bl.config(); c.pin_bl=45; c.invert=false; c.freq=44100; c.pwm_channel=7;
      _bl.config(c); _panel.setLight(&_bl); }
    setPanel(&_panel);
  }
};
static LGFX        lcd;
static LGFX_Sprite spr(&lcd);

// ── Colori ────────────────────────────────────────────────────────────────────
static constexpr uint32_t C_BG      = 0x0D1B2A;
static constexpr uint32_t C_HEADER  = 0x0A2540;
static constexpr uint32_t C_INPUT   = 0x00C8FF;
static constexpr uint32_t C_OUTPUT  = 0x00E87A;
static constexpr uint32_t C_COST    = 0xFFCC00;
static constexpr uint32_t C_GRID    = 0x162534;
static constexpr uint32_t C_TEXT    = 0xEEF2F7;
static constexpr uint32_t C_MUTED   = 0x4A6A80;
static constexpr uint32_t C_DIVIDER = 0x1E3A52;
static constexpr uint32_t C_NAV     = 0x0C1B28;
static constexpr uint32_t C_NAV_ACT = 0x183550;
static constexpr uint32_t C_GREEN   = 0x00E87A;
static constexpr uint32_t C_AMBER   = 0xFFA040;
static constexpr uint32_t C_RED     = 0xFF3838;
static constexpr uint32_t C_WHITE   = 0xFFFFFF;

// ── Layout ────────────────────────────────────────────────────────────────────
static constexpr int SW=320, SH=480, HDR_H=46, NAV_H=78;
static constexpr int NAV_Y=SH-NAV_H, CONT_Y=HDR_H;

// ── Viste ─────────────────────────────────────────────────────────────────────
enum View { V_NOW=0, V_HOURLY, V_DAILY, V_WEEKLY };
static View currentView = V_NOW;

// ── Dati ─────────────────────────────────────────────────────────────────────
struct AccountData {
  int   remaining  = -1;   // -1 = non ancora ricevuto
  int   limit      = 0;
  int   used       = 0;
  long  reset_ts   = 0;    // Unix timestamp UTC reset
  char  plan[12]   = "";
  bool  valid      = false;
};
struct LiveStats {
  long  input=0, output=0, cache_read=0;
  int   requests=0;
  float cost=0;
  bool  valid=false;
};
struct Period { char label[8]; long input, output; float cost; };

static AccountData acc;
static LiveStats   live;
static Period      periods[24];
static int         nPeriods = 0;

// ── Touch ─────────────────────────────────────────────────────────────────────
struct TouchPt { int x,y; bool pressed; };
static TouchPt readTouch() {
  Wire.beginTransmission(TP_ADDR); Wire.write(0x02);
  if (Wire.endTransmission(false)!=0) return {0,0,false};
  Wire.requestFrom((uint8_t)TP_ADDR,(uint8_t)6);
  if (Wire.available()<6) return {0,0,false};
  uint8_t cnt=Wire.read(), xh=Wire.read(), xl=Wire.read();
  Wire.read(); uint8_t yh=Wire.read(), yl=Wire.read();
  return {((xh&0x0F)<<8)|xl, ((yh&0x0F)<<8)|yl, cnt>0};
}

// ── Helpers ───────────────────────────────────────────────────────────────────
static String fmtK(long n) {
  if (n>=1000000L) return String(n/1000000.0f,2)+"M";
  if (n>=1000L)    return String(n/1000.0f,1)+"K";
  return String(n);
}
static String fmtCost(float c) {
  char b[12];
  if (c<0.01f)  snprintf(b,12,"$%.5f",c);
  else if(c<1)  snprintf(b,12,"$%.4f",c);
  else          snprintf(b,12,"$%.2f",c);
  return String(b);
}

// Colore in base alla percentuale rimasta
static uint32_t limitColor(int remaining, int total) {
  if (total<=0 || remaining<0) return C_MUTED;
  float pct = (float)remaining/total;
  if (pct>0.5f)  return C_GREEN;
  if (pct>0.25f) return C_AMBER;
  return C_RED;
}

// Countdown human-readable
static String fmtCountdown(long secs) {
  if (secs<=0) return "reset ora";
  long h = secs/3600, m = (secs%3600)/60;
  if (h>0) return String(h)+"h "+String(m)+"m";
  return String(m)+"m "+String(secs%60)+"s";
}

// Ora corrente UTC via NTP
static long nowUTC() {
  time_t t; time(&t); return (long)t;
}

// ── WiFi + NTP ────────────────────────────────────────────────────────────────
static void connectWiFi() {
  spr.fillSprite(C_BG); spr.setTextColor(C_INPUT); spr.setTextSize(2);
  spr.setCursor(28,200); spr.print("Connessione WiFi...");
  spr.pushSprite(0,0);
  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int n=0;
  while (WiFi.status()!=WL_CONNECTED && n<40) {
    delay(500);
    spr.fillRect(28+(n%22)*6,255,4,4,C_INPUT); spr.pushSprite(0,0); n++;
  }
  // Sincronizza orologio via NTP
  configTime(TIMEZONE_OFFSET_H*3600L, 0, "pool.ntp.org", "time.nist.gov");
  spr.fillRect(28,270,200,20,C_BG); spr.setTextColor(C_MUTED); spr.setTextSize(1);
  spr.setCursor(28,272); spr.print("Sincronizzazione NTP...");
  spr.pushSprite(0,0);
  delay(2000); // attende primo sync NTP
}

// ── HTTP fetch ────────────────────────────────────────────────────────────────
static bool httpGet(const char* ep, JsonDocument& doc) {
  if (WiFi.status()!=WL_CONNECTED) { WiFi.reconnect(); return false; }
  HTTPClient http;
  http.begin(String(SERVER_BASE)+ep); http.setTimeout(6000);
  int code=http.GET(); if (code!=200) { http.end(); return false; }
  DeserializationError err=deserializeJson(doc,http.getStream());
  http.end(); return !err;
}

static void fetchAll() {
  { JsonDocument d;
    if (httpGet("/api/tokens",d)) {
      live.input=d["total_input"]|0L; live.output=d["total_output"]|0L;
      live.cache_read=d["total_cache_read"]|0L; live.requests=d["requests_count"]|0;
      live.cost=d["cost_usd"]|0.0f; live.valid=true; } }

  { JsonDocument d;
    if (httpGet("/api/account",d) && (bool)(d["has_data"]|false)) {
      acc.remaining = d["messages_remaining"]|-1;
      acc.limit     = d["messages_limit"]|0;
      acc.used      = d["messages_used"]|0;
      acc.reset_ts  = d["reset_at_ts"]|0L;
      strlcpy(acc.plan, d["plan"]|"pro", 12);
      acc.valid=true; } }

  const char* ep[]={nullptr,"/api/tokens/hourly","/api/tokens/daily","/api/tokens/weekly"};
  if (currentView!=V_NOW && ep[currentView]) {
    JsonDocument d;
    if (httpGet(ep[currentView],d)) {
      nPeriods=0;
      for (JsonObject o:d.as<JsonArray>()) {
        if (nPeriods>=24) break;
        strlcpy(periods[nPeriods].label,o["label"]|"?",8);
        periods[nPeriods].input=o["input"]|0L; periods[nPeriods].output=o["output"]|0L;
        periods[nPeriods].cost=o["cost"]|0.0f; nPeriods++;
      }
    }
  }
}

// ── Componenti UI ─────────────────────────────────────────────────────────────
static void drawHeader(const char* sub) {
  spr.fillRect(0,0,SW,HDR_H,C_HEADER);
  spr.fillCircle(16,HDR_H/2,7,C_GREEN); spr.fillCircle(16,HDR_H/2,4,C_BG); spr.fillCircle(16,HDR_H/2,2,C_GREEN);
  spr.setTextColor(C_TEXT); spr.setTextSize(2); spr.setCursor(30,13); spr.print("CLAUDE");
  spr.setTextColor(C_INPUT); spr.print(" MONITOR");
  spr.setTextColor(C_MUTED); spr.setTextSize(1);
  spr.setCursor(SW-(int)strlen(sub)*6-6,18); spr.print(sub);
  spr.drawFastHLine(0,HDR_H-1,SW,C_DIVIDER);
}

static void drawNavBar() {
  const char* lbl[]={"NOW","ORARIO","GIORNO","SETT."};
  spr.drawFastHLine(0,NAV_Y,SW,C_DIVIDER);
  for (int i=0;i<4;i++) {
    int bx=i*80; bool act=(i==(int)currentView);
    spr.fillRect(bx,NAV_Y+1,80,NAV_H-1,act?C_NAV_ACT:C_NAV);
    if (act) spr.fillRect(bx+6,NAV_Y+1,68,3,C_INPUT);
    uint32_t ic=act?C_INPUT:C_MUTED; int cx=bx+40,iy=NAV_Y+28;
    if (i==0) { spr.drawCircle(cx,iy,13,ic); spr.drawFastVLine(cx,iy-8,8,ic); spr.drawFastHLine(cx,iy,6,ic); }
    else { int bh[]={8,13,18}; for(int b=0;b<3;b++) spr.fillRect(cx-13+b*10,iy+13-bh[b],7,bh[b],ic); }
    spr.setTextColor(act?C_TEXT:C_MUTED); spr.setTextSize(1);
    spr.setCursor(bx+(80-(int)strlen(lbl[i])*6)/2,NAV_Y+58); spr.print(lbl[i]);
  }
}

// ── Schermata NOW ─────────────────────────────────────────────────────────────
static void drawNowScreen() {
  drawHeader("ADESSO");
  int y = CONT_Y + 10;

  // ── Sezione limiti account ────────────────────────────────────────────────
  if (acc.valid && acc.remaining >= 0) {
    uint32_t col = limitColor(acc.remaining, acc.limit);

    // Label
    spr.setTextColor(C_MUTED); spr.setTextSize(1);
    spr.setCursor(14, y); spr.print("MESSAGGI RIMANENTI");
    y += 14;

    // Numero grande
    String remStr = String(acc.remaining);
    spr.setTextColor(col); spr.setTextSize(5);
    int nw = remStr.length()*30;
    spr.setCursor((SW-nw)/2, y); spr.print(remStr);
    y += 42;

    // Totale
    if (acc.limit > 0) {
      String totStr = "/ " + String(acc.limit);
      spr.setTextColor(C_MUTED); spr.setTextSize(2);
      int tw = totStr.length()*12;
      spr.setCursor((SW-tw)/2, y); spr.print(totStr);
    }
    y += 26;

    // Barra progresso
    if (acc.limit > 0) {
      int barW = SW - 28;
      int filled = barW - (int)((acc.remaining * (long)barW) / acc.limit);
      if (filled < 0) filled = 0;
      if (filled > barW) filled = barW;
      spr.fillRoundRect(14, y, filled, 14, 4, col);
      spr.fillRoundRect(14+filled, y, barW-filled, 14, 4, C_GRID);
      y += 20;

      // Etichette barra
      spr.setTextColor(C_MUTED); spr.setTextSize(1);
      spr.setCursor(14, y);
      spr.print(String(acc.used)+" usati");
      String remLbl = String(acc.remaining)+" rimasti";
      spr.setCursor(SW-(int)remLbl.length()*6-14, y);
      spr.print(remLbl);
      y += 16;
    }

    // Countdown reset
    if (acc.reset_ts > 0) {
      long secsLeft = acc.reset_ts - nowUTC();
      spr.setTextColor(C_MUTED); spr.setTextSize(1);
      spr.setCursor(14, y); spr.print("RESET TRA");
      spr.setTextColor(C_AMBER); spr.setTextSize(2);
      String cd = fmtCountdown(secsLeft);
      spr.setCursor(SW-(int)cd.length()*12-14, y-3); spr.print(cd);
      y += 22;
    }

    spr.drawFastHLine(14, y, SW-28, C_DIVIDER);
    y += 8;

  } else {
    // Nessun dato account ancora
    spr.fillRoundRect(14, y, SW-28, 60, 8, C_HEADER);
    spr.drawRoundRect(14, y, SW-28, 60, 8, C_DIVIDER);
    spr.setTextColor(C_MUTED); spr.setTextSize(1);
    spr.setCursor(30, y+10); spr.print("Apri claude.ai nel browser");
    spr.setCursor(30, y+24); spr.print("per vedere i limiti account.");
    spr.setCursor(30, y+38); spr.print("(Tampermonkey deve essere attivo)");
    y += 70;
  }

  // ── Sezione token sessione ────────────────────────────────────────────────
  if (live.valid) {
    spr.setTextColor(C_MUTED); spr.setTextSize(1);
    spr.setCursor(14, y); spr.print("SESSIONE CORRENTE");
    y += 13;

    // Token in/out su una riga
    spr.setTextColor(C_INPUT); spr.setTextSize(1);
    spr.setCursor(14, y); spr.print("IN ");
    spr.setTextColor(C_TEXT); spr.setTextSize(2);
    spr.print(fmtK(live.input));
    spr.setTextColor(C_OUTPUT); spr.setTextSize(1);
    spr.print("  OUT ");
    spr.setTextColor(C_TEXT); spr.setTextSize(2);
    spr.print(fmtK(live.output));
    y += 24;

    // Richieste
    spr.setTextColor(C_MUTED); spr.setTextSize(1);
    spr.setCursor(14, y);
    spr.print(String(live.requests)+" richieste API");
    y += 14;

    // Box costo
    int boxH = NAV_Y - y - 8;
    if (boxH > 36) {
      spr.fillRoundRect(14, y, SW-28, boxH, 8, C_HEADER);
      spr.drawRoundRect(14, y, SW-28, boxH, 8, C_DIVIDER);
      spr.setTextColor(C_MUTED); spr.setTextSize(1);
      spr.setCursor(26, y+8); spr.print("COSTO SESSIONE USD");
      String cs = fmtCost(live.cost);
      spr.setTextColor(C_COST); spr.setTextSize(3);
      spr.setCursor((SW-(int)cs.length()*18)/2, y+22); spr.print(cs);
    }
  }

  drawNavBar();
}

// ── Schermata grafici ─────────────────────────────────────────────────────────
static void drawChartScreen() {
  const char* titles[]={"","ULTIME 24 ORE","ULTIMI 7 GIORNI","ULTIME 4 SETTIMANE"};
  drawHeader(titles[(int)currentView]);

  long totIn=0,totOut=0; float totCost=0;
  for (int i=0;i<nPeriods;i++) { totIn+=periods[i].input; totOut+=periods[i].output; totCost+=periods[i].cost; }

  int sy=CONT_Y+8;
  spr.setTextSize(1); spr.setTextColor(C_MUTED); spr.setCursor(14,sy); spr.print("IN ");
  spr.setTextColor(C_INPUT); spr.print(fmtK(totIn));
  spr.setTextColor(C_MUTED); spr.print("   OUT ");
  spr.setTextColor(C_OUTPUT); spr.print(fmtK(totOut));
  spr.setTextColor(C_MUTED); spr.setCursor(14,sy+14); spr.print("COSTO ");
  spr.setTextColor(C_COST); spr.setTextSize(2); spr.print(fmtCost(totCost));

  const int CX=48, CY=CONT_Y+46, CW=SW-CX-8, CH=NAV_Y-CY-26;
  spr.fillRect(CX,CY,CW,CH,C_GRID);

  long maxVal=1;
  for (int i=0;i<nPeriods;i++) { long t=periods[i].input+periods[i].output; if(t>maxVal) maxVal=t; }

  for (int g=1;g<=4;g++) {
    int gy=CY+CH-(g*CH/4);
    spr.drawFastHLine(CX,gy,CW,0x1E3A50);
    String lbl=fmtK((maxVal*g)/4);
    spr.setTextColor(C_MUTED); spr.setTextSize(1);
    spr.setCursor(CX-(int)lbl.length()*6-2,gy-4); spr.print(lbl);
  }
  spr.drawFastHLine(CX,CY+CH,CW,C_DIVIDER);

  if (nPeriods==0) {
    spr.setTextColor(C_MUTED); spr.setTextSize(1);
    spr.setCursor(CX+60,CY+CH/2-4); spr.print("Nessun dato");
  } else {
    const int GAP=(nPeriods>10)?1:(nPeriods>4?2:4);
    int barW=(CW-GAP*(nPeriods-1))/nPeriods; if(barW<3) barW=3;
    for (int i=0;i<nPeriods;i++) {
      int bx=CX+i*(barW+GAP);
      long total=periods[i].input+periods[i].output;
      if (total>0) {
        int totalH=(int)((total*(long)CH)/maxVal); if(totalH>CH) totalH=CH;
        int inputH=(int)((periods[i].input*(long)totalH)/total);
        int outputH=totalH-inputH, by=CY+CH-totalH;
        if (outputH>0) spr.fillRect(bx,by,barW,outputH,C_OUTPUT);
        if (inputH>0)  spr.fillRect(bx,by+outputH,barW,inputH,C_INPUT);
        spr.drawFastHLine(bx,by,barW,C_WHITE);
      }
      bool showLbl=(nPeriods<=7)||(i%6==0)||(i==nPeriods-1);
      if (showLbl) {
        spr.setTextColor(C_MUTED); spr.setTextSize(1);
        spr.setCursor(bx+barW/2-(int)strlen(periods[i].label)*3,CY+CH+4);
        spr.print(periods[i].label);
      }
    }
  }

  int ly=NAV_Y-19;
  spr.fillRect(CX,ly,10,8,C_INPUT); spr.fillRect(CX+52,ly,10,8,C_OUTPUT);
  spr.setTextColor(C_MUTED); spr.setTextSize(1);
  spr.setCursor(CX+13,ly); spr.print("Input");
  spr.setCursor(CX+65,ly); spr.print("Output");
  drawNavBar();
}

static void render() {
  spr.fillSprite(C_BG);
  if (currentView==V_NOW) drawNowScreen();
  else                    drawChartScreen();
  spr.pushSprite(0,0);
}

static void showBoot() {
  spr.fillSprite(C_BG);
  spr.fillRoundRect(80,140,160,70,12,C_HEADER);
  spr.drawRoundRect(80,140,160,70,12,C_INPUT);
  spr.setTextColor(C_INPUT); spr.setTextSize(2); spr.setCursor(104,155); spr.print("CLAUDE");
  spr.setTextColor(C_OUTPUT); spr.setCursor(96,177); spr.print("MONITOR");
  spr.setTextColor(C_MUTED); spr.setTextSize(1); spr.setCursor(116,230); spr.print("v3.0  ESP32-S3");
  spr.pushSprite(0,0); delay(1200);
}

// ── Setup / Loop ──────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  lcd.init(); lcd.setRotation(0); lcd.setBrightness(BRIGHTNESS);
  spr.setColorDepth(16);
  if (!spr.createSprite(SW,SH))
    Serial.println("WARN: PSRAM non trovata — abilita OPI PSRAM");
  showBoot();
  Wire.begin(TP_SDA, TP_SCL);
  connectWiFi();
  fetchAll();
  render();
}

static unsigned long lastPoll=0;
static bool wasPressed=false;

void loop() {
  unsigned long now=millis();
  if (now-lastPoll>=POLL_MS) { lastPoll=now; fetchAll(); render(); }

  // Aggiorna countdown ogni secondo senza refetch di rete
  if (currentView==V_NOW && acc.valid && acc.reset_ts>0 && (now%1000<40)) render();

  TouchPt tp=readTouch();
  if (tp.pressed && !wasPressed) {
    wasPressed=true;
    if (tp.y>=NAV_Y) {
      int btn=tp.x/80;
      if (btn<4) {
        View next=(View)btn;
        if (next!=currentView) { currentView=next; nPeriods=0; fetchAll(); render(); lastPoll=now; }
      }
    }
  } else if (!tp.pressed) wasPressed=false;
  delay(40);
}
