/*
 * Claude Token Monitor — ESP32 + SSD1306 OLED 128x64 (I2C)
 *
 * Wiring:
 *   OLED VCC → 3.3V
 *   OLED GND → GND
 *   OLED SDA → GPIO 21
 *   OLED SCL → GPIO 22
 *
 * Libraries (install via Arduino Library Manager):
 *   - Adafruit SSD1306
 *   - Adafruit GFX Library
 *   - ArduinoJson  (Benoit Blanchon, v7.x)
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ── Configuration ────────────────────────────────────────────────────────────
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// IP of the machine running server.js (must be reachable from ESP32)
const char* SERVER_URL    = "http://192.168.1.100:3333/api/tokens";

const unsigned long POLL_INTERVAL_MS = 30000; // 30 seconds
// ─────────────────────────────────────────────────────────────────────────────

#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1   // share reset with ESP32 reset pin
#define OLED_ADDRESS  0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

unsigned long lastPoll = 0;
bool wifiOk = false;

struct TokenStats {
  long   input;
  long   output;
  long   cache_read;
  long   cache_creation;
  int    requests;
  float  cost_usd;
  bool   valid;
};

TokenStats lastStats = {0, 0, 0, 0, 0, 0.0f, false};

// ── Display helpers ───────────────────────────────────────────────────────────

void showBoot() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(20, 10);
  display.println("CLAUDE MONITOR");
  display.setCursor(30, 26);
  display.println("Connecting...");
  display.display();
}

void showError(const char* msg) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("! ERROR");
  display.println(msg);
  display.display();
}

// Format large numbers: 1234567 → "1.23M", 12345 → "12.3K"
String fmtTokens(long n) {
  if (n >= 1000000) {
    return String(n / 1000000.0f, 2) + "M";
  } else if (n >= 1000) {
    return String(n / 1000.0f, 1) + "K";
  }
  return String(n);
}

void showStats(const TokenStats& s) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // Title bar
  display.fillRect(0, 0, 128, 10, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setTextSize(1);
  display.setCursor(20, 1);
  display.print("CLAUDE TOKENS");
  display.setTextColor(SSD1306_WHITE);

  // Stats rows
  int y = 13;
  const int LINE_H = 10;

  display.setCursor(0, y);
  display.print("IN:  ");
  display.println(fmtTokens(s.input));
  y += LINE_H;

  display.setCursor(0, y);
  display.print("OUT: ");
  display.println(fmtTokens(s.output));
  y += LINE_H;

  display.setCursor(0, y);
  display.print("CHE: ");           // cache hits (abbreviated)
  display.println(fmtTokens(s.cache_read));
  y += LINE_H;

  display.setCursor(0, y);
  display.print("REQ: ");
  display.println(s.requests);
  y += LINE_H;

  // Cost — highlighted
  display.fillRect(0, y - 1, 128, 12, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setCursor(0, y);
  display.print("COST $");
  display.print(s.cost_usd, 4);
  display.setTextColor(SSD1306_WHITE);

  display.display();
}

void showWifiConnecting(int attempt) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("WiFi connecting");
  display.setCursor(0, 16);
  display.print("SSID: ");
  display.println(WIFI_SSID);
  display.setCursor(0, 32);
  display.print("Attempt: ");
  display.println(attempt);
  display.display();
}

// ── Network ───────────────────────────────────────────────────────────────────

bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED && attempt < 20) {
    showWifiConnecting(attempt + 1);
    delay(500);
    attempt++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("WiFi OK");
    display.println(WiFi.localIP().toString());
    display.display();
    delay(1500);
    return true;
  }
  return false;
}

TokenStats fetchStats() {
  TokenStats s = {0, 0, 0, 0, 0, 0.0f, false};

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    return s;
  }

  HTTPClient http;
  http.begin(SERVER_URL);
  http.setTimeout(5000);

  int code = http.GET();
  if (code != 200) {
    Serial.printf("HTTP error: %d\n", code);
    http.end();
    return s;
  }

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("JSON parse error: %s\n", err.c_str());
    return s;
  }

  s.input         = doc["total_input"]        | 0L;
  s.output        = doc["total_output"]       | 0L;
  s.cache_read    = doc["total_cache_read"]   | 0L;
  s.cache_creation= doc["total_cache_creation"]| 0L;
  s.requests      = doc["requests_count"]     | 0;
  s.cost_usd      = doc["cost_usd"]           | 0.0f;
  s.valid         = true;

  Serial.printf("Fetched: in=%ld out=%ld cost=$%.4f\n", s.input, s.output, s.cost_usd);
  return s;
}

// ── Arduino lifecycle ─────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("SSD1306 init failed — check wiring");
    for (;;);
  }

  display.clearDisplay();
  showBoot();

  wifiOk = connectWiFi();
  if (!wifiOk) {
    showError("WiFi failed");
  }
}

void loop() {
  unsigned long now = millis();

  if (now - lastPoll >= POLL_INTERVAL_MS || lastPoll == 0) {
    lastPoll = now;
    TokenStats s = fetchStats();
    if (s.valid) {
      lastStats = s;
    }
  }

  if (lastStats.valid) {
    showStats(lastStats);
  } else if (!wifiOk) {
    showError("No WiFi");
  }

  delay(1000);
}
