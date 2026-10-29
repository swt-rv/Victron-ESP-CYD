/*
 * Camper power dashboard
 * ----------------------
 * Board : ESP32-2432S028R ("Cheap Yellow Display", 2.8" ILI9341 320x240)
 * Reads : Victron SmartSolar MPPT + SmartShunt/BMV via "Instant Readout"
 *         (encrypted BLE advertisements) and up to 3 Ruuvi tags (data format 5).
 *
 * Settings (MACs, Victron keys, tag names) are stored in flash (NVS) and are
 * changed over USB serial with simple text commands. The web setup page sends
 * these for you. Commands (115200 baud, one per line):
 *   show
 *   scan
 *   set solar <mac> [key32hex]
 *   set shunt <mac> [key32hex]
 *   set tag <1-3> <mac> <name>
 *   clear solar | clear shunt | clear tag <1-3>
 *   reboot
 */

#include <Arduino.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <NimBLEDevice.h>
#include "mbedtls/aes.h"
#include <strings.h>
#include <ctype.h>

static const uint32_t STALE_MS = 15000;   // data older than this is shown as "--"

TFT_eSPI tft;
Preferences prefs;

// ---------- Configuration (loaded from flash) ----------
static char    solarMac[18] = "", shuntMac[18] = "";
static char    solarKeyHex[33] = "", shuntKeyHex[33] = "";
static uint8_t solarKey[16], shuntKey[16];
static bool    hasSolar = false, hasShunt = false;

struct RuuviTag {
  char name[12];
  char mac[18];
  float tempC, hum, battV;
  uint32_t last;
};
static RuuviTag tags[3];

// ---------- Live data ----------
struct SolarData {
  uint8_t state = 0xFF;
  uint8_t error = 0;
  float vBat = NAN, iBat = NAN, pvW = NAN, yieldKWh = NAN, iLoad = NAN;
  uint32_t last = 0;
} solar;

struct ShuntData {
  float v = NAN, i = NAN, soc = NAN, ahConsumed = NAN;
  float auxV = NAN, tempC = NAN;
  int ttgMin = -1;
  uint16_t alarm = 0;
  uint32_t last = 0;
} shunt;

// ---------- Helpers ----------
static bool hexToBytes(const char* hex, uint8_t* out, size_t n) {
  if (strlen(hex) != n * 2) return false;
  for (size_t i = 0; i < n; i++) {
    char b[3] = { hex[2 * i], hex[2 * i + 1], 0 };
    if (!isxdigit((unsigned char)b[0]) || !isxdigit((unsigned char)b[1])) return false;
    out[i] = (uint8_t)strtoul(b, nullptr, 16);
  }
  return true;
}

static bool normMac(const char* in, char* out) {
  if (strlen(in) != 17) return false;
  for (int i = 0; i < 17; i++) {
    char c = in[i];
    if (i % 3 == 2) { if (c != ':') return false; out[i] = ':'; }
    else { if (!isxdigit((unsigned char)c)) return false; out[i] = (char)tolower(c); }
  }
  out[17] = 0;
  return true;
}

// Little-endian bit reader over the decrypted payload
static uint32_t getBits(const uint8_t* d, int pos, int n) {
  uint32_t v = 0;
  for (int i = 0; i < n; i++) {
    int p = pos + i;
    if (d[p >> 3] & (1 << (p & 7))) v |= (1UL << i);
  }
  return v;
}
static int32_t getSigned(const uint8_t* d, int pos, int n) {
  uint32_t v = getBits(d, pos, n);
  if (v & (1UL << (n - 1))) return (int32_t)(v | (~0UL << n));
  return (int32_t)v;
}

static bool fresh(uint32_t t) { return t && (millis() - t) < STALE_MS; }

// Remember devices we have already reported, so each is printed once per scan
static char seenList[24][18];
static int  seenCount = 0;
static bool seenNew(const char* mac) {
  for (int i = 0; i < seenCount; i++) if (!strcmp(seenList[i], mac)) return false;
  if (seenCount < 24) { strlcpy(seenList[seenCount++], mac, 18); return true; }
  return false;
}

// ---------- Config storage ----------
static void loadConfig() {
  prefs.begin("cfg", true);
  strlcpy(solarMac,    prefs.getString("sMac", "").c_str(), sizeof solarMac);
  strlcpy(solarKeyHex, prefs.getString("sKey", "").c_str(), sizeof solarKeyHex);
  strlcpy(shuntMac,    prefs.getString("hMac", "").c_str(), sizeof shuntMac);
  strlcpy(shuntKeyHex, prefs.getString("hKey", "").c_str(), sizeof shuntKeyHex);
  for (int i = 0; i < 3; i++) {
    char k[10];
    snprintf(k, sizeof k, "t%dMac", i);
    strlcpy(tags[i].mac, prefs.getString(k, "").c_str(), sizeof tags[i].mac);
    snprintf(k, sizeof k, "t%dName", i);
    String def = "TAG" + String(i + 1);
    strlcpy(tags[i].name, prefs.getString(k, def).c_str(), sizeof tags[i].name);
    tags[i].tempC = tags[i].hum = tags[i].battV = NAN;
    tags[i].last = 0;
  }
  prefs.end();

  hasSolar = solarMac[0] && hexToBytes(solarKeyHex, solarKey, 16);
  hasShunt = shuntMac[0] && hexToBytes(shuntKeyHex, shuntKey, 16);
}

static void saveStr(const char* key, const char* val) {
  prefs.begin("cfg", false);
  prefs.putString(key, val);
  prefs.end();
}

// ---------- Serial commands ----------
static void cmdShow() {
  Serial.printf("CFG solar %s %s\n", solarMac[0] ? solarMac : "-", solarKeyHex[0] ? "yes" : "no");
  Serial.printf("CFG shunt %s %s\n", shuntMac[0] ? shuntMac : "-", shuntKeyHex[0] ? "yes" : "no");
  for (int i = 0; i < 3; i++)
    Serial.printf("CFG tag %d %s %s\n", i + 1, tags[i].mac[0] ? tags[i].mac : "-", tags[i].name);
}

static void handleLine(char* line) {
  char* t[6];
  int n = 0;
  for (char* p = strtok(line, " \r\t"); p && n < 6; p = strtok(nullptr, " \r\t")) t[n++] = p;
  if (!n) return;

  if (!strcmp(t[0], "show")) {
    cmdShow();
  } else if (!strcmp(t[0], "scan")) {
    seenCount = 0;
    Serial.println("OK scanning");
  } else if (!strcmp(t[0], "reboot")) {
    Serial.println("OK rebooting");
    Serial.flush();
    delay(100);
    ESP.restart();
  } else if (!strcmp(t[0], "set") && n >= 3 && (!strcmp(t[1], "solar") || !strcmp(t[1], "shunt"))) {
    bool isSolar = !strcmp(t[1], "solar");
    char mac[18];
    if (!normMac(t[2], mac)) { Serial.println("ERR bad mac"); return; }
    if (n >= 4) {
      uint8_t tmp[16];
      if (!hexToBytes(t[3], tmp, 16)) { Serial.println("ERR key must be 32 hex characters"); return; }
      saveStr(isSolar ? "sKey" : "hKey", t[3]);
    }
    saveStr(isSolar ? "sMac" : "hMac", mac);
    Serial.println("OK saved");
  } else if (!strcmp(t[0], "set") && n >= 5 && !strcmp(t[1], "tag")) {
    int idx = atoi(t[2]) - 1;
    char mac[18];
    if (idx < 0 || idx > 2) { Serial.println("ERR tag must be 1-3"); return; }
    if (!normMac(t[3], mac)) { Serial.println("ERR bad mac"); return; }
    char name[12];
    strlcpy(name, t[4], sizeof name);
    char k[10];
    snprintf(k, sizeof k, "t%dMac", idx);  saveStr(k, mac);
    snprintf(k, sizeof k, "t%dName", idx); saveStr(k, name);
    Serial.println("OK saved");
  } else if (!strcmp(t[0], "clear") && n >= 2) {
    if (!strcmp(t[1], "solar")) { saveStr("sMac", ""); saveStr("sKey", ""); Serial.println("OK cleared"); }
    else if (!strcmp(t[1], "shunt")) { saveStr("hMac", ""); saveStr("hKey", ""); Serial.println("OK cleared"); }
    else if (!strcmp(t[1], "tag") && n >= 3) {
      int idx = atoi(t[2]) - 1;
      if (idx < 0 || idx > 2) { Serial.println("ERR tag must be 1-3"); return; }
      char k[10];
      snprintf(k, sizeof k, "t%dMac", idx); saveStr(k, "");
      Serial.println("OK cleared");
    } else Serial.println("ERR unknown clear target");
  } else {
    Serial.println("ERR unknown command");
  }
}

static void pollSerial() {
  static char buf[96];
  static size_t len = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (len) { buf[len] = 0; handleLine(buf); len = 0; }
    } else if (len < sizeof(buf) - 1) {
      buf[len++] = c;
    }
  }
}

// ---------- Victron decoding ----------
static void parseSolar(const uint8_t* d) {
  int p = 0;
  uint8_t state = getBits(d, p, 8);    p += 8;
  uint8_t err   = getBits(d, p, 8);    p += 8;
  int32_t v     = getSigned(d, p, 16); p += 16;
  int32_t i     = getSigned(d, p, 16); p += 16;
  uint32_t y    = getBits(d, p, 16);   p += 16;
  uint32_t pv   = getBits(d, p, 16);   p += 16;
  uint32_t load = getBits(d, p, 9);

  solar.state    = state;
  solar.error    = err;
  solar.vBat     = (v == 0x7FFF) ? NAN : v * 0.01f;
  solar.iBat     = (i == 0x7FFF) ? NAN : i * 0.1f;
  solar.yieldKWh = (y == 0xFFFF) ? NAN : y * 0.01f;
  solar.pvW      = (pv == 0xFFFF) ? NAN : (float)pv;
  solar.iLoad    = (load == 0x1FF) ? NAN : load * 0.1f;
  solar.last     = millis();
}

static void parseShunt(const uint8_t* d) {
  int p = 0;
  uint32_t ttg   = getBits(d, p, 16);   p += 16;
  int32_t v      = getSigned(d, p, 16); p += 16;
  uint32_t alrm  = getBits(d, p, 16);   p += 16;
  uint32_t aux   = getBits(d, p, 16);   p += 16;
  uint32_t auxIn = getBits(d, p, 2);    p += 2;
  int32_t cur    = getSigned(d, p, 22); p += 22;
  uint32_t cons  = getBits(d, p, 20);   p += 20;
  uint32_t soc   = getBits(d, p, 10);

  shunt.ttgMin     = (ttg == 0xFFFF) ? -1 : (int)ttg;
  shunt.v          = (v == 0x7FFF) ? NAN : v * 0.01f;
  shunt.alarm      = alrm;
  shunt.i          = cur * 0.001f;
  shunt.ahConsumed = (cons == 0xFFFFF) ? NAN : -(float)cons * 0.1f;  // negative = consumed
  shunt.soc        = (soc == 0x3FF) ? NAN : soc * 0.1f;
  shunt.auxV = NAN;
  shunt.tempC = NAN;
  if (auxIn == 0)      shunt.auxV  = (int16_t)aux * 0.01f;
  else if (auxIn == 2) shunt.tempC = aux * 0.01f - 273.15f;           // 0.01 K
  shunt.last = millis();
}

static void handleVictron(const std::string& mac, const uint8_t* m, size_t len) {
  // m[0..1]=0x02E1 company, m[2]=0x10, m[3..4]=model, m[5]=record type,
  // m[6..7]=nonce, m[8]=key check byte, m[9..]=encrypted payload
  if (seenNew(mac.c_str())) Serial.printf("SEEN victron %s %02x\n", mac.c_str(), m[5]);

  const uint8_t* key = nullptr;
  uint8_t expectType = 0;
  if (hasSolar && !strcasecmp(mac.c_str(), solarMac))      { key = solarKey; expectType = 0x01; }
  else if (hasShunt && !strcasecmp(mac.c_str(), shuntMac)) { key = shuntKey; expectType = 0x02; }
  else return;

  if (m[5] != expectType) return;
  if (m[8] != key[0]) {
    static uint32_t lastWarn = 0;
    if (millis() - lastWarn > 10000) {
      lastWarn = millis();
      Serial.printf("WARN key mismatch for %s - check the encryption key\n", mac.c_str());
    }
    return;
  }

  size_t encLen = len - 9;
  if (encLen > 16) encLen = 16;
  uint8_t out[16] = {0};

  uint8_t counter[16] = {0};
  counter[0] = m[6];
  counter[1] = m[7];
  uint8_t stream[16];
  size_t off = 0;

  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_enc(&ctx, key, 128);
  mbedtls_aes_crypt_ctr(&ctx, encLen, &off, counter, stream, m + 9, out);
  mbedtls_aes_free(&ctx);

  if (expectType == 0x01) parseSolar(out);
  else                    parseShunt(out);
}

// ---------- Ruuvi decoding (RAWv2 / data format 5) ----------
// m[0..1] = 0x0499 company (little-endian), m[2] = 0x05 format, then big-endian:
// temp int16 (0.005 C) @3, humidity u16 (0.0025 %) @5, pressure @7, accel @9..14,
// power info u16 @15 (battery mV = (v>>5)+1600).
static void handleRuuvi(const std::string& mac, const uint8_t* m, size_t len) {
  if (seenNew(mac.c_str())) Serial.printf("SEEN ruuvi %s\n", mac.c_str());

  int idx = -1;
  for (int i = 0; i < 3; i++) {
    if (tags[i].mac[0] && !strcasecmp(mac.c_str(), tags[i].mac)) { idx = i; break; }
  }
  if (idx < 0) return;

  if (len < 17 || m[2] != 0x05) {
    static uint32_t lastWarn = 0;
    if (millis() - lastWarn > 10000) {
      lastWarn = millis();
      Serial.printf("WARN Ruuvi data format %d not supported (need 5) - update the tag firmware\n", m[2]);
    }
    return;
  }

  RuuviTag& t = tags[idx];
  int16_t  rawT = (int16_t)((m[3] << 8) | m[4]);
  uint16_t rawH = (uint16_t)((m[5] << 8) | m[6]);
  uint16_t rawP = (uint16_t)((m[15] << 8) | m[16]);

  t.tempC = (rawT == (int16_t)0x8000) ? NAN : rawT * 0.005f;
  t.hum   = (rawH == 0xFFFF) ? NAN : rawH * 0.0025f;
  t.battV = (rawP == 0xFFFF) ? NAN : (((rawP >> 5) + 1600) / 1000.0f);
  t.last  = millis();
}

// ---------- BLE scan callback ----------
class AdvCB : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice* dev) override {
    std::string mfg = dev->getManufacturerData();
    if (mfg.size() < 3) return;
    const uint8_t* m = (const uint8_t*)mfg.data();
    std::string mac = dev->getAddress().toString();
    if (m[0] == 0xE1 && m[1] == 0x02 && m[2] == 0x10 && mfg.size() >= 10) {
      handleVictron(mac, m, mfg.size());
    } else if (m[0] == 0x99 && m[1] == 0x04) {          // Ruuvi Innovations, 0x0499
      handleRuuvi(mac, m, mfg.size());
    }
  }
} advCB;

// ---------- Display ----------
static const uint16_t HDR_COL = 0x0292;   // dark blue header

static void put(int x, int y, int w, int font, uint16_t col, const char* s) {
  tft.setTextColor(col, TFT_BLACK);
  tft.setTextPadding(w);
  tft.drawString(s, x, y, font);
}

static const char* solarStateName(uint8_t s) {
  switch (s) {
    case 0: return "Off";
    case 1: return "Low power";
    case 2: return "FAULT";
    case 3: return "Bulk";
    case 4: return "Absorb";
    case 5: return "Float";
    case 6: return "Storage";
    case 7: return "Equalize";
    case 245: return "Starting";
    case 246: return "Re-absorb";
    case 247: return "Auto eq.";
    case 248: return "BMS lock";
    case 252: return "External";
    default: return "?";
  }
}

static void drawHeaderLeft(bool alarm) {
  uint16_t bg = alarm ? TFT_RED : HDR_COL;
  tft.fillRect(0, 0, 160, 20, bg);
  tft.setTextColor(TFT_WHITE, bg);
  tft.setTextPadding(0);
  tft.drawString(alarm ? "BATTERY ALARM" : "CAMPER POWER", 6, 2, 2);
}

static void drawStatic() {
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, 320, 20, HDR_COL);
  drawHeaderLeft(false);

  tft.setTextPadding(0);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("BATTERY", 6, 24, 2);
  tft.drawString("SOLAR", 6, 124, 2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("%", 106, 62, 4);
  tft.drawString("W", 118, 160, 4);

  tft.drawFastHLine(0, 120, 320, TFT_DARKGREY);
  tft.drawFastHLine(0, 198, 320, TFT_DARKGREY);
  tft.drawFastVLine(146, 22, 96, TFT_DARKGREY);
  tft.drawFastVLine(146, 122, 74, TFT_DARKGREY);
  tft.drawFastVLine(106, 200, 40, TFT_DARKGREY);
  tft.drawFastVLine(213, 200, 40, TFT_DARKGREY);
}

static void drawTempCell(int x, const char* name, float tC, float hum, bool ok) {
  char buf[28];
  if (ok && !isnan(hum)) snprintf(buf, sizeof buf, "%s  %.0f%%RH", name, hum);
  else                   snprintf(buf, sizeof buf, "%s", name);
  put(x + 4, 201, 100, 1, TFT_DARKGREY, buf);
  if (ok && !isnan(tC)) {
    snprintf(buf, sizeof buf, "%.1f C", tC);
    uint16_t c = tC < 2 ? TFT_CYAN : (tC > 35 ? TFT_ORANGE : TFT_WHITE);
    put(x + 4, 212, 98, 4, c, buf);
  } else {
    put(x + 4, 212, 98, 4, TFT_DARKGREY, "--");
  }
}

static void drawDashboard() {
  char buf[48];
  bool shuntOk = fresh(shunt.last);
  bool solarOk = fresh(solar.last);

  // ----- Header -----
  static int8_t lastAlarm = -1;
  bool alarm = shuntOk && shunt.alarm;
  if (lastAlarm != (int8_t)alarm) { drawHeaderLeft(alarm); lastAlarm = alarm; }

  tft.setTextDatum(TR_DATUM);
  tft.setTextPadding(110);
  if (!hasSolar && !hasShunt) {
    tft.setTextColor(TFT_YELLOW, HDR_COL);
    tft.drawString("SETUP NEEDED", 314, 2, 2);
  } else {
    snprintf(buf, sizeof buf, "%s %s", shuntOk ? "SHUNT" : "shunt?", solarOk ? "MPPT" : "mppt?");
    tft.setTextColor((shuntOk && solarOk) ? TFT_GREEN : TFT_ORANGE, HDR_COL);
    tft.drawString(buf, 314, 2, 2);
  }
  tft.setTextDatum(TL_DATUM);

  // ----- Battery -----
  if (shuntOk && !isnan(shunt.soc)) {
    snprintf(buf, sizeof buf, "%d", (int)(shunt.soc + 0.5f));
    uint16_t c = shunt.soc > 50 ? TFT_GREEN : (shunt.soc > 20 ? TFT_YELLOW : TFT_RED);
    put(6, 40, 98, 6, c, buf);
  } else put(6, 40, 98, 6, TFT_DARKGREY, "--");

  if (shuntOk && !isnan(shunt.v)) {
    snprintf(buf, sizeof buf, "%.2f V", shunt.v);
    put(152, 26, 165, 4, TFT_WHITE, buf);
  } else put(152, 26, 165, 4, TFT_DARKGREY, "--");

  if (shuntOk) {
    snprintf(buf, sizeof buf, "%+.2f A", shunt.i);
    put(152, 54, 165, 4, shunt.i >= 0 ? TFT_GREEN : TFT_ORANGE, buf);
    snprintf(buf, sizeof buf, "%+.0f W", shunt.i * shunt.v);
    put(152, 82, 165, 4, shunt.i >= 0 ? TFT_GREEN : TFT_ORANGE, buf);
  } else {
    put(152, 54, 165, 4, TFT_DARKGREY, "--");
    put(152, 82, 165, 4, TFT_DARKGREY, "--");
  }

  char ttg[16] = "--";
  if (shuntOk && shunt.ttgMin >= 0) {
    int m = shunt.ttgMin;
    if (m >= 1440) snprintf(ttg, sizeof ttg, "%dd %dh", m / 1440, (m % 1440) / 60);
    else           snprintf(ttg, sizeof ttg, "%dh %02dm", m / 60, m % 60);
  }
  if (!hasShunt)
    snprintf(buf, sizeof buf, "Shunt not set up - use the setup page");
  else if (shuntOk && !isnan(shunt.ahConsumed))
    snprintf(buf, sizeof buf, "Used %.1fAh  Left %s", shunt.ahConsumed, ttg);
  else
    snprintf(buf, sizeof buf, "Used --  Left --");
  put(6, 102, 310, 2, TFT_LIGHTGREY, buf);

  // ----- Solar -----
  if (solarOk && !isnan(solar.pvW)) {
    snprintf(buf, sizeof buf, "%d", (int)solar.pvW);
    put(6, 136, 108, 6, solar.pvW > 0 ? TFT_YELLOW : TFT_DARKGREY, buf);
  } else put(6, 136, 108, 6, TFT_DARKGREY, "--");

  if (solarOk && !isnan(solar.vBat)) {
    snprintf(buf, sizeof buf, "%.1fV %.1fA", solar.vBat, solar.iBat);
    put(152, 126, 165, 4, TFT_WHITE, buf);
    uint16_t sc = (solar.state == 2 || solar.error) ? TFT_RED : TFT_CYAN;
    put(152, 154, 165, 4, sc, solarStateName(solar.state));
    snprintf(buf, sizeof buf, "Today %.2f kWh", solar.yieldKWh);
    put(152, 182, 165, 2, TFT_LIGHTGREY, buf);
  } else {
    put(152, 126, 165, 4, TFT_DARKGREY, "--");
    put(152, 154, 165, 4, TFT_DARKGREY, "--");
    put(152, 182, 165, 2, TFT_DARKGREY, "");
  }

  // ----- Temperatures: three Ruuvi tags -----
  for (int i = 0; i < 3; i++) {
    const char* nm = tags[i].mac[0] ? tags[i].name : "(unused)";
    drawTempCell(i * 107, nm, tags[i].tempC, tags[i].hum, fresh(tags[i].last));
  }
}

// ---------- Arduino ----------
void setup() {
  Serial.begin(115200);
  delay(50);

  loadConfig();

  tft.init();
  tft.setRotation(1);   // landscape; use 3 to flip 180 degrees
  drawStatic();

  NimBLEDevice::init("");
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(&advCB, true);   // keep duplicates so data keeps updating
  scan->setActiveScan(false);                         // passive is enough
  scan->setInterval(100);
  scan->setWindow(99);
  scan->setMaxResults(0);                             // don't store results (avoids heap growth)
  scan->start(0, nullptr, false);                     // scan forever

  Serial.println("READY");
}

void loop() {
  pollSerial();

  static uint32_t lastDraw = 0;
  if (millis() - lastDraw >= 1000) {
    lastDraw = millis();
    drawDashboard();
  }

  static uint32_t lastData = 0;
  if (millis() - lastData >= 5000) {
    lastData = millis();
    Serial.printf("DATA SOC %.1f%%  %.2fV  %.2fA | PV %.0fW %s | %s %.1fC  %s %.1fC  %s %.1fC\n",
                  shunt.soc, shunt.v, shunt.i, solar.pvW, solarStateName(solar.state),
                  tags[0].name, tags[0].tempC, tags[1].name, tags[1].tempC,
                  tags[2].name, tags[2].tempC);
  }

  // Restart scan if it ever stops
  NimBLEScan* scan = NimBLEDevice::getScan();
  if (!scan->isScanning()) scan->start(0, nullptr, false);

  delay(10);
}
