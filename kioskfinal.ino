/*
 * ============================================================
 *  DXB LAYOVER KIOSK  -  ESP32 (MYOSA) + SSD1306 OLED + Gemini
 * ============================================================
 *  Flow:
 *   1. Turn the pot to choose layover hours (2-12 h)
 *   2. Press the button -> ESP32 asks Gemini for a Dubai plan
 *   3. ESP32 hosts a cute "boarding pass" web page with
 *      Google Maps links for every stop
 *   4. QR code on the OLED opens that page on the visitor's phone
 *      (phone must be on the same Wi-Fi as the kiosk)
 *   5. While idle, the OLED briefly shows a Visa Guy ad + QR
 *
 *  LEDs:  YELLOW = thinking   GREEN = pass ready   RED = error
 *  RESET button (D19): tap = clear pass / new traveller, hold 3 s = restart
 *
 *  Libraries (Library Manager):
 *   - Adafruit SSD1306, Adafruit GFX Library, ArduinoJson (v7)
 *  QR uses the ESP32 core's built-in "qrcode.h" (esp_qrcode).
 * ============================================================
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "qrcode.h"

// ---------------- USER CONFIG ----------------
const char* WIFI_SSID     = "GM60";
const char* WIFI_PASS     = "Amrut1234";
const char* GEMINI_KEY    = "AQ.Ab8RN6KaxtSlMDYjqfOruTBGSKVO6oJ9Dfr5SmK7-3Vh8BUrNg";
const char* GEMINI_MODEL  = "gemini-3.8-flash";
const char* GEMINI_MODEL_2 = "";        // optional 2nd Gemini model, e.g. a Flash-Lite name from AI Studio

// Backup AIs (free). Leave the key "" to skip that AI.
const char* GROQ_KEY         = "";       // from console.groq.com -> API Keys
const char* GROQ_MODEL       = "llama-3.3-70b-versatile";
const char* OPENROUTER_KEY   = "";       // from openrouter.ai -> Keys
const char* OPENROUTER_MODEL = "";       // pick any model marked ":free" on openrouter.ai
const char* AIRPORT_NAME  = "Dubai International Airport (DXB)";
const char* AIRPORT_CODE  = "DXB";

// ---------------- AI PROVIDER TYPE ----------------
// (must be up here so Arduino IDE knows it before any function)
enum ApiType { API_GEMINI, API_OPENAI };
struct AIProvider {
  const char* name;
  ApiType     type;
  const char* url;      // only for OpenAI-style APIs
  const char* key;
  const char* model;
  uint16_t    timeoutMs;
};

// ---------------- AD CONFIG ----------------
const char* AD_URL        = "https://visaguy.ae";
const unsigned long AD_EVERY_MS = 20000;   // show ad every 20 s while idle
const unsigned long AD_SHOW_MS  = 5000;    // ad stays for 5 s

// ---------------- PINS (MYOSA board) ----------------
#define POT_PIN       34   // pot middle leg
#define BUTTON_PIN    18   // button -> GND (internal pull-up)
#define BOOT_BTN_PIN   0   // on-board BOOT button also works
#define RESET_BTN_PIN 19   // reset button -> GND (short press = new customer, hold 3 s = restart)
#define LED_GREEN     25   // pass ready
#define LED_RED       26   // error
#define LED_YELLOW    33   // thinking
#define BUZZER_PIN    12   // buzzer (passive buzzer gives the best chime)
// OLED: SDA / SCL pins on the MYOSA bottom header (GPIO 21 / 22)

// ---------------- OLED ----------------
#define SCREEN_W 128
#define SCREEN_H 64
#define OLED_ADDR 0x3C
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);

// ---------------- GLOBALS ----------------
WebServer server(80);
String passHtml = "";
int    layoverHours = 4;
int    lastShownHours = -1;
bool   showingQR = false;
bool   showingAd = false;
unsigned long qrShownAt = 0;
unsigned long lastAdEnd = 0;
unsigned long adStartedAt = 0;
const unsigned long QR_TIMEOUT_MS = 120000;  // back to menu after 2 min
String qrCaption[4];                          // text shown next to a QR
void connectWiFi();                          // defined further down

// ============================================================
//  Small helpers
// ============================================================
void beep(int ms) {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(ms);
  digitalWrite(BUZZER_PIN, LOW);
}

void setLeds(bool green, bool red, bool yellow) {
  digitalWrite(LED_GREEN,  green  ? HIGH : LOW);
  digitalWrite(LED_RED,    red    ? HIGH : LOW);
  digitalWrite(LED_YELLOW, yellow ? HIGH : LOW);
}

void showMessage(const String& l1, const String& l2 = "", const String& l3 = "") {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);   display.println(l1);
  display.setCursor(0, 20);  display.println(l2);
  display.setCursor(0, 40);  display.println(l3);
  display.display();
}

void showMenu() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("DXB LAYOVER KIOSK");
  display.drawLine(0, 10, 127, 10, SSD1306_WHITE);
  display.setCursor(0, 16);
  display.println("Layover time:");
  display.setTextSize(2);
  display.setCursor(30, 30);
  display.print(layoverHours);
  display.print(" hrs");
  display.setTextSize(1);
  display.setCursor(0, 54);
  display.println("Press button to plan");
  display.display();
}

int readLayoverHours() {
  int raw = analogRead(POT_PIN);             // 0..4095
  return map(raw, 0, 4095, 2, 12);
}

String htmlEscape(const String& s) {
  String o;
  o.reserve(s.length() + 16);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;";
    else if (c == '\'') o += "&#39;";
    else if (c == '\n') o += "<br>";
    else o += c;
  }
  return o;
}

String urlEncode(const String& s) {
  const char* hex = "0123456789ABCDEF";
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
    else if (c == ' ') o += '+';
    else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
  }
  return o;
}

String mapsLink(const String& place, const String& area) {
  String q = place;
  if (area.length()) q += ", " + area;
  q += ", Dubai";
  return "https://www.google.com/maps/search/?api=1&query=" + urlEncode(q);
}

// ============================================================
//  QR CODE (ESP32 core's esp_qrcode)
// ============================================================
static void drawQR(esp_qrcode_handle_t qr) {
  int size  = esp_qrcode_get_size(qr);
  int scale = SCREEN_H / size;
  if (scale < 1) scale = 1;
  int qrPx = size * scale;
  int ox = 5;
  int oy = (SCREEN_H - qrPx) / 2;

  display.clearDisplay();
  display.fillRect(ox - 3, 0, qrPx + 6, SCREEN_H, SSD1306_WHITE);  // quiet zone
  for (int y = 0; y < size; y++)
    for (int x = 0; x < size; x++)
      if (esp_qrcode_get_module(qr, x, y))
        display.fillRect(ox + x * scale, oy + y * scale, scale, scale, SSD1306_BLACK);

  int tx = ox + qrPx + 6;
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  for (int i = 0; i < 4; i++) {
    display.setCursor(tx, 4 + i * 14);
    display.println(qrCaption[i]);
  }
  display.display();
}

void showQR(const String& url, const String& c1, const String& c2,
            const String& c3, const String& c4) {
  qrCaption[0] = c1; qrCaption[1] = c2; qrCaption[2] = c3; qrCaption[3] = c4;
  esp_qrcode_config_t cfg;
  cfg.display_func       = drawQR;
  cfg.max_qrcode_version = 4;
  cfg.qrcode_ecc_level   = ESP_QRCODE_ECC_LOW;
  if (esp_qrcode_generate(&cfg, url.c_str()) != ESP_OK) {
    Serial.println("QR failed - URL too long?");
    showMessage("Open in browser:", url);
  }
}

// ============================================================
//  VISA GUY AD (OLED)
// ============================================================
void showAd() {
  showQR(AD_URL, "UAE VISA?", "Leave DXB", "worry-free", "visaguy.ae");
}

// ============================================================
//  AI PROVIDERS  - tried in order until one answers
// ============================================================

AIProvider PROVIDERS[] = {
  // name           type        url                                                  key             model              timeout
  { "Gemini",       API_GEMINI, "",                                                  GEMINI_KEY,     GEMINI_MODEL,      45000 },
  { "Gemini Lite",  API_GEMINI, "",                                                  GEMINI_KEY,     GEMINI_MODEL_2,    30000 },
  { "Groq",         API_OPENAI, "https://api.groq.com/openai/v1/chat/completions",   GROQ_KEY,       GROQ_MODEL,        25000 },
  { "OpenRouter",   API_OPENAI, "https://openrouter.ai/api/v1/chat/completions",     OPENROUTER_KEY, OPENROUTER_MODEL,  30000 },
};
const int NUM_PROVIDERS = sizeof(PROVIDERS) / sizeof(PROVIDERS[0]);
String plannedBy = "";

// Builds the prompt (same for every AI)
String buildPrompt(int hours) {
  String prompt =
    "You are a cheerful, funny travel buddy at a kiosk inside " + String(AIRPORT_NAME) +
    ". A traveller has a " + String(hours) + " hour layover. Plan it.\n"
    "Rules:\n"
    "- Keep at least 2.5 hours buffer for getting back, security and boarding.\n"
    "- If the layover is under 6 hours, suggest ONLY things inside DXB airport "
    "(lounges, gardens, food, shopping, sleep pods, etc.).\n"
    "- If 6 hours or more, suggest real places in Dubai reachable from DXB "
    "(Dubai Metro Red Line runs from Terminals 1 and 3; taxis also work), "
    "and remind them some passports need a UAE visa to leave the airport.\n"
    "- Exactly 3 stops, in a sensible order. Keep every field short.\n"
    "- Use the exact real names of places so they can be found on Google Maps.\n"
    "Reply ONLY with JSON in exactly this shape:\n"
    "{\"title\":\"short fun trip name\","
    "\"vibe\":\"one funny cute sentence\","
    "\"stops\":[{\"name\":\"exact place name\",\"area\":\"area or terminal\","
    "\"emoji\":\"one emoji\",\"travel\":\"how to get there + time\","
    "\"spend\":\"time to spend\",\"tip\":\"short funny tip\"}],"
    "\"back_by\":\"when to be back at DXB, friendly\"}";

  return prompt;
}

// Sends one request. Returns HTTP code; fills 'text' with the AI's answer.
int callProvider(const AIProvider& p, const String& prompt, String& text) {
  String url, body;
  JsonDocument req;

  if (p.type == API_GEMINI) {
    url = "https://generativelanguage.googleapis.com/v1beta/models/" + String(p.model) + ":generateContent";
    req["contents"][0]["parts"][0]["text"] = prompt;
    req["generationConfig"]["responseMimeType"] = "application/json";
    req["generationConfig"]["temperature"] = 0.9;
    if (String(p.model).startsWith("gemini-3"))
      req["generationConfig"]["thinkingConfig"]["thinkingLevel"] = "low";
  } else {
    url = p.url;
    req["model"] = p.model;
    req["messages"][0]["role"] = "user";
    req["messages"][0]["content"] = prompt;
    req["temperature"] = 0.9;
  }
  serializeJson(req, body);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, url)) return -1;
  http.addHeader("Content-Type", "application/json");
  if (p.type == API_GEMINI) http.addHeader("x-goog-api-key", p.key);
  else                      http.addHeader("Authorization", "Bearer " + String(p.key));
  http.setTimeout(p.timeoutMs);

  unsigned long t0 = millis();
  int code = http.POST(body);
  String resp = (code > 0) ? http.getString() : "";
  http.end();
  Serial.printf("  %s -> HTTP %d (%lu s)\n", p.name, code, (millis() - t0) / 1000);

  if (code < 0) {
    Serial.printf("  Connection problem: %s\n", HTTPClient::errorToString(code).c_str());
    return code;
  }
  if (code != 200) {
    Serial.println(resp.substring(0, 400));
    return code;
  }

  JsonDocument doc;
  if (deserializeJson(doc, resp)) { Serial.println("  Bad JSON from AI"); return 0; }

  text = "";
  if (p.type == API_GEMINI) {
    for (JsonObject part : doc["candidates"][0]["content"]["parts"].as<JsonArray>()) {
      if (part["thought"] | false) continue;
      const char* t = part["text"];
      if (t) text += t;
    }
  } else {
    const char* t = doc["choices"][0]["message"]["content"];
    if (t) text = t;
  }
  if (text.indexOf('{') < 0) { Serial.println("  No usable plan in reply"); return 0; }
  return 200;
}

// Built-in plan so the kiosk ALWAYS gives a result, even with no AI
String offlinePlan(int hours) {
  if (hours < 6) {
    return F("{\"title\":\"DXB Airport Explorer\","
             "\"vibe\":\"The AIs are napping, so I planned this one myself!\","
             "\"stops\":["
             "{\"name\":\"Dubai Duty Free\",\"area\":\"Terminal 3, Dubai International Airport\",\"emoji\":\"🛍️\","
             "\"travel\":\"Walk from your gate\",\"spend\":\"45 min\",\"tip\":\"Window shopping is free cardio.\"},"
             "{\"name\":\"Sleep 'n Fly Sleep Lounge\",\"area\":\"Dubai International Airport\",\"emoji\":\"😴\","
             "\"travel\":\"Follow the signs\",\"spend\":\"1 hr\",\"tip\":\"A power nap beats a power bank.\"},"
             "{\"name\":\"Food court\",\"area\":\"Terminal 3, Dubai International Airport\",\"emoji\":\"🍔\","
             "\"travel\":\"Near the central concourse\",\"spend\":\"30 min\",\"tip\":\"Calories eaten in transit don't count.\"}],"
             "\"back_by\":\"Stay airside and head to your gate when boarding opens!\"}");
  }
  return F("{\"title\":\"Dubai Speed Run\","
           "\"vibe\":\"The AIs are busy, so the kiosk picked its all-time favourites!\","
           "\"stops\":["
           "{\"name\":\"Burj Khalifa\",\"area\":\"Downtown Dubai\",\"emoji\":\"🏙️\","
           "\"travel\":\"Metro Red Line to Burj Khalifa/Dubai Mall\",\"spend\":\"1 hr\",\"tip\":\"Look up. Keep looking. Still going.\"},"
           "{\"name\":\"The Dubai Mall\",\"area\":\"Downtown Dubai\",\"emoji\":\"🛍️\","
           "\"travel\":\"Connected to Burj Khalifa\",\"spend\":\"1 hr\",\"tip\":\"Visit the aquarium, wave at a shark.\"},"
           "{\"name\":\"Al Fahidi Historical Neighbourhood\",\"area\":\"Bur Dubai\",\"emoji\":\"🕌\","
           "\"travel\":\"About 15 min by taxi\",\"spend\":\"45 min\",\"tip\":\"Then hop on an abra across the creek!\"}],"
           "\"back_by\":\"Be back at DXB at least 2.5 hours before your flight.\"}");
}

// Tries every configured AI in order; falls back to the built-in plan.
String askAI(int hours) {
  String prompt = buildPrompt(hours);
  for (int i = 0; i < NUM_PROVIDERS; i++) {
    const AIProvider& p = PROVIDERS[i];
    if (strlen(p.key) < 10 || strlen(p.model) == 0) continue;   // not set up - skip
    if (WiFi.status() != WL_CONNECTED) connectWiFi();

    showMessage("Planning your", String(hours) + " hr DXB layover...", String("Asking ") + p.name + "...");
    Serial.printf("Trying %s (%s)...\n", p.name, p.model);

    String text;
    int code = callProvider(p, prompt, text);
    if (code == 200) {
      plannedBy = p.name;
      return text;
    }
    // 429 = limit reached, 503 = busy, 404 = model gone, <0 = network: move on
  }
  Serial.println("All AIs failed - using built-in plan");
  plannedBy = "the kiosk (offline)";
  return offlinePlan(hours);
}
// ============================================================
//  CUTE BOARDING PASS PAGE
// ============================================================
const char PASS_HEAD[] = R"HTML(<!DOCTYPE html><html><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Your DXB Layover Pass</title>
<link href='https://fonts.googleapis.com/css2?family=Baloo+2:wght@500;700;800&display=swap' rel='stylesheet'>
<style>
*{box-sizing:border-box}
body{margin:0;padding:18px 12px 40px;font-family:'Baloo 2','Trebuchet MS',sans-serif;color:#3b2f4a;
background:linear-gradient(165deg,#ffe3ef 0%,#dff1ff 55%,#fff4d6 100%);min-height:100vh}
.sky{text-align:center;font-size:26px;letter-spacing:18px;opacity:.7;margin-bottom:6px}
.pass{max-width:440px;margin:auto;background:#fff;border-radius:26px;overflow:hidden;
box-shadow:0 12px 30px rgba(120,80,160,.25)}
.top{background:linear-gradient(120deg,#ff7eb3,#ff9e7a 45%,#7ac6ff);color:#fff;padding:18px 20px 22px;position:relative}
.brand{font-size:13px;letter-spacing:2px;opacity:.9}
.title{font-size:26px;font-weight:800;line-height:1.1;margin:4px 0 0}
.plane{position:absolute;right:16px;top:12px;font-size:40px;animation:fly 3s ease-in-out infinite}
@keyframes fly{0%,100%{transform:translateY(0) rotate(-8deg)}50%{transform:translateY(-8px) rotate(4deg)}}
.route{display:flex;align-items:center;justify-content:space-between;padding:16px 22px 6px}
.code{font-size:34px;font-weight:800;color:#ff5f9e}
.code small{display:block;font-size:11px;color:#9a8cab;font-weight:500;margin-top:-6px}
.dash{flex:1;border-top:3px dotted #ffb3d1;margin:0 10px;position:relative}
.dash span{position:absolute;left:50%;top:-17px;transform:translateX(-50%);font-size:22px;background:#fff;padding:0 4px}
.grid{display:grid;grid-template-columns:1fr 1fr 1fr;gap:8px;padding:8px 18px 14px}
.cell{background:#fff6fa;border-radius:12px;padding:6px 8px}
.cell b{display:block;font-size:10px;letter-spacing:1px;color:#b49cc6}
.cell span{font-size:15px;font-weight:700}
.tear{position:relative;border-top:3px dashed #f0dce8;margin:0 14px}
.tear:before,.tear:after{content:'';position:absolute;top:-14px;width:26px;height:26px;border-radius:50%;
background:#f3e8f7}
.tear:before{left:-28px}.tear:after{right:-28px}
.vibe{margin:16px 18px 6px;background:#fff1d6;border-radius:16px;padding:10px 14px;font-size:15px;position:relative}
.vibe:before{content:'\1F4AC';position:absolute;left:-6px;top:-12px;font-size:22px}
.stop{display:flex;gap:12px;margin:12px 16px;padding:12px;border-radius:18px;background:#f4f9ff;
border:2px solid #e2f0ff}
.stop:nth-child(even){background:#fff5fa;border-color:#ffe2ee}
.emo{font-size:34px;width:48px;height:48px;display:flex;align-items:center;justify-content:center;
background:#fff;border-radius:14px;box-shadow:0 3px 8px rgba(0,0,0,.06);flex-shrink:0}
.num{font-size:11px;color:#b49cc6;letter-spacing:1px}
.name{font-size:18px;font-weight:800;line-height:1.15}
.area{font-size:12px;color:#8d7f9e}
.chips{margin:6px 0 4px}
.chip{display:inline-block;font-size:12px;background:#fff;border-radius:20px;padding:2px 9px;margin:2px 4px 2px 0;
border:1px solid #eadff2}
.tip{font-size:13px;font-style:italic;color:#6d5f7d}
.map{display:inline-block;margin-top:6px;font-size:13px;font-weight:700;text-decoration:none;color:#fff;
background:linear-gradient(90deg,#34c38f,#2aa4f4);padding:6px 12px;border-radius:20px}
.back{margin:14px 18px;background:#e9fff4;border:2px dashed #8fe0b8;border-radius:16px;padding:10px 14px;font-weight:700}
.ad{display:block;margin:16px;border-radius:18px;padding:14px 16px;text-decoration:none;color:#3b2a00;
background:linear-gradient(120deg,#ffd86b,#ffb347);box-shadow:0 6px 14px rgba(255,170,60,.35)}
.ad b{font-size:17px;display:block}.ad span{font-size:13px}
.ad i{display:inline-block;margin-top:6px;background:#3b2a00;color:#ffd86b;font-style:normal;
padding:4px 12px;border-radius:14px;font-size:13px;font-weight:700}
.barcode{height:46px;margin:6px 22px 4px;background:repeating-linear-gradient(90deg,#3b2f4a 0 2px,#fff 2px 4px,#3b2f4a 4px 5px,#fff 5px 8px,#3b2f4a 8px 11px,#fff 11px 13px)}
.foot{text-align:center;font-size:12px;color:#a596b5;padding:4px 10px 16px}
.save{display:block;margin:14px auto 0;border:0;border-radius:20px;padding:10px 20px;font-family:inherit;
font-size:15px;font-weight:700;color:#fff;background:#ff7eb3;box-shadow:0 4px 10px rgba(255,126,179,.4)}
@media print{.save{display:none}body{background:#fff}}
</style></head><body>
<div class='sky'>&#9729;&#65039; &#9729;&#65039; &#9729;&#65039;</div>
<div class='pass'>)HTML";

String buildPassHtml(int hours, const String& aiJson) {
  // Extract the {...} part in case the model wrapped it
  int a = aiJson.indexOf('{');
  int b = aiJson.lastIndexOf('}');
  String clean = (a >= 0 && b > a) ? aiJson.substring(a, b + 1) : aiJson;

  JsonDocument plan;
  bool ok = !deserializeJson(plan, clean);

  String title = ok ? String((const char*)(plan["title"] | "Dubai Adventure Mode")) : "Dubai Adventure Mode";
  String vibe  = ok ? String((const char*)(plan["vibe"]  | "")) : "";
  String back  = ok ? String((const char*)(plan["back_by"] | "")) : "";

  String h;
  h.reserve(9000);
  h += PASS_HEAD;

  // Header
  h += "<div class='top'><div class='brand'>&#10024; LAYOVER AIRLINES &#10024;</div>";
  h += "<div class='title'>" + htmlEscape(title) + "</div>";
  h += "<div class='plane'>&#9992;&#65039;</div></div>";

  // Route
  h += "<div class='route'><div class='code'>" + String(AIRPORT_CODE) +
       "<small>you are here</small></div><div class='dash'><span>&#129321;</span></div>"
       "<div class='code'>FUN<small>destination</small></div>"
       "<div class='dash'><span>&#128747;</span></div><div class='code'>" + String(AIRPORT_CODE) +
       "<small>back in time</small></div></div>";

  // Info grid
  h += "<div class='grid'>"
       "<div class='cell'><b>PASSENGER</b><span>Explorer</span></div>"
       "<div class='cell'><b>LAYOVER</b><span>" + String(hours) + " hrs</span></div>"
       "<div class='cell'><b>GATE</b><span>&#127775; Fun</span></div>"
       "<div class='cell'><b>SEAT</b><span>Window</span></div>"
       "<div class='cell'><b>CLASS</b><span>VIP</span></div>"
       "<div class='cell'><b>SNACKS</b><span>Yes pls</span></div>"
       "</div><div class='tear'></div>";

  if (vibe.length()) h += "<div class='vibe'>" + htmlEscape(vibe) + "</div>";

  // Stops
  h += "<div class='stops'>";
  if (ok && plan["stops"].is<JsonArray>()) {
    int n = 1;
    for (JsonObject s : plan["stops"].as<JsonArray>()) {
      String name   = s["name"]   | "Mystery stop";
      String area   = s["area"]   | "";
      String emoji  = s["emoji"]  | "📍";
      String travel = s["travel"] | "";
      String spend  = s["spend"]  | "";
      String tip    = s["tip"]    | "";

      h += "<div class='stop'><div class='emo'>" + htmlEscape(emoji) + "</div><div>";
      h += "<div class='num'>STOP " + String(n++) + "</div>";
      h += "<div class='name'>" + htmlEscape(name) + "</div>";
      if (area.length()) h += "<div class='area'>" + htmlEscape(area) + "</div>";
      h += "<div class='chips'>";
      if (travel.length()) h += "<span class='chip'>&#128647; " + htmlEscape(travel) + "</span>";
      if (spend.length())  h += "<span class='chip'>&#9201;&#65039; " + htmlEscape(spend) + "</span>";
      h += "</div>";
      if (tip.length()) h += "<div class='tip'>" + htmlEscape(tip) + "</div>";
      h += "<a class='map' target='_blank' href='" + htmlEscape(mapsLink(name, area)) +
           "'>&#128205; Open in Google Maps</a>";
      h += "</div></div>";
    }
  } else {
    // Fallback: show whatever text the AI sent
    h += "<div class='stop'><div class='emo'>&#129302;</div><div class='tip'>" +
         htmlEscape(aiJson) + "</div></div>";
  }
  h += "</div>";

  if (back.length()) h += "<div class='back'>&#9200; " + htmlEscape(back) + "</div>";

  // Visa Guy ad banner
  h += "<a class='ad' target='_blank' href='" + String(AD_URL) + "'>"
       "<b>&#128706; Want to step out of DXB?</b>"
       "<span>Some passports need a UAE visa to leave the airport. Get yours sorted with Visa Guy.</span><br>"
       "<i>visaguy.ae &#8594;</i></a>";

  h += "<div class='barcode'></div>";
  h += "<button class='save' onclick='window.print()'>&#128190; Save my pass</button>";
  h += "<div class='foot'>Planned by " + htmlEscape(plannedBy) + " &middot; Made with &#128150; by the DXB Layover Kiosk &middot; Don't miss your flight!</div>";
  h += "</div></body></html>";
  return h;
}

const char WAIT_PAGE[] = R"HTML(<!DOCTYPE html><html><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<meta http-equiv='refresh' content='5'>
<style>body{margin:0;font-family:'Trebuchet MS',sans-serif;display:flex;align-items:center;justify-content:center;
height:100vh;background:linear-gradient(165deg,#ffe3ef,#dff1ff 55%,#fff4d6)}
.card{background:#fff;border-radius:24px;padding:28px;text-align:center;max-width:320px;
box-shadow:0 10px 26px rgba(120,80,160,.2)}
.p{font-size:48px;animation:f 2.5s ease-in-out infinite}@keyframes f{50%{transform:translateY(-8px)}}
h1{color:#ff5f9e;margin:6px 0}p{color:#6d5f7d}</style></head><body>
<div class='card'><div class='p'>&#9992;&#65039;</div><h1>DXB Layover Kiosk</h1>
<p>No pass yet! Turn the knob, press the button, and this page will update by itself.</p></div>
</body></html>)HTML";

void handlePass() {
  if (passHtml.length() == 0) server.send(200, "text/html", WAIT_PAGE);
  else                        server.send(200, "text/html", passHtml);
}

// ============================================================
//  MAIN ACTION
// ============================================================
void generatePass() {
  showingAd = false;
  setLeds(false, false, true);            // yellow = thinking
  beep(80);
  showMessage("Planning your", String(layoverHours) + " hr DXB layover...", "Please wait :)");

  String plan = askAI(layoverHours);

  if (plan.length() == 0) {
    setLeds(false, true, false);          // red = error
    beep(400);
    showMessage("AI request failed", "Check WiFi / API key", "Try again");
    delay(3000);
    setLeds(false, false, false);
    lastShownHours = -1;
    lastAdEnd = millis();
    return;
  }

  Serial.println("---- PLAN JSON ----");
  Serial.println(plan);

  passHtml = buildPassHtml(layoverHours, plan);

  String url = "http://" + WiFi.localIP().toString() + "/p";
  Serial.println("Pass URL: " + url);

  setLeds(true, false, false);            // green = ready
  beep(60); delay(80); beep(60);
  showQR(url, "SCAN", "for your", "boarding", "pass!");
  showingQR = true;
  qrShownAt = millis();
}

// ============================================================
//  WIFI
// ============================================================
bool scanForMySSID() {
  Serial.println("\nScanning for Wi-Fi networks...");
  showMessage("Scanning WiFi...");
  int n = WiFi.scanNetworks();
  bool found = false;
  for (int i = 0; i < n; i++) {
    Serial.printf("  %2d: %-25s  signal %d dBm\n", i + 1, WiFi.SSID(i).c_str(), WiFi.RSSI(i));
    if (WiFi.SSID(i) == WIFI_SSID) found = true;
  }
  WiFi.scanDelete();
  Serial.printf(found ? "OK: '%s' is visible.\n" : "PROBLEM: '%s' NOT found (2.4 GHz?)\n", WIFI_SSID);
  return found;
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.disconnect(true);
  delay(200);

  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED) {
    attempt++;
    if (!scanForMySSID()) {
      showMessage("WiFi not found:", WIFI_SSID, "Use 2.4GHz network");
      setLeds(false, true, false);
      delay(5000);
      continue;
    }
    Serial.printf("Connecting to '%s' (attempt %d)", WIFI_SSID, attempt);
    showMessage("Connecting WiFi...", WIFI_SSID, "Attempt " + String(attempt));
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
      delay(500);
      Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.printf("FAILED (status %d) - check password / WPA2\n", (int)WiFi.status());
      showMessage("WiFi failed", "Retrying...");
      setLeds(false, true, false);
      WiFi.disconnect(true);
      delay(3000);
    }
  }
  setLeds(false, false, false);
  Serial.println("WiFi connected!");
  Serial.println("IP: " + WiFi.localIP().toString());
}

// ============================================================
//  SETUP / LOOP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== DXB Layover Kiosk booting ===");

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(BOOT_BTN_PIN, INPUT_PULLUP);
  pinMode(RESET_BTN_PIN, INPUT_PULLUP);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_YELLOW, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  analogReadResolution(12);

  // LED self-test: Y -> G -> R
  setLeds(false, false, true);  delay(250);
  setLeds(true, false, false);  delay(250);
  setLeds(false, true, false);  delay(250);
  setLeds(false, false, false);

  Wire.begin(21, 22);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED not found! Check SDA/SCL/3V3/GND");
  }

  connectWiFi();

  server.on("/", handlePass);
  server.on("/p", handlePass);
  server.begin();

  showMessage("WiFi OK", WiFi.localIP().toString());
  beep(100);
  delay(1500);
  lastAdEnd = millis();
}

// ============================================================
//  AIRPORT CHIME + LED SHOW (played on reset)
// ============================================================
// Plays one note by toggling the pin (works on any ESP32 core).
// Passive buzzer -> real musical notes. Active buzzer -> buzzy but still a chime.
void playNote(int freq, int ms) {
  if (freq <= 0) { delay(ms); return; }
  long halfPeriod = 500000L / freq;            // microseconds
  long cycles = (long)freq * ms / 1000;
  for (long i = 0; i < cycles; i++) {
    digitalWrite(BUZZER_PIN, HIGH); delayMicroseconds(halfPeriod);
    digitalWrite(BUZZER_PIN, LOW);  delayMicroseconds(halfPeriod);
  }
}

void airportChime() {
  // Classic "ding-dong-ding-dong" airport announcement chime
  const int notes[] = { 784, 659, 523, 392 };   // G5  E5  C5  G4
  const int durs[]  = { 380, 380, 380, 650 };
  // each note lights one LED: yellow -> green -> red -> all
  for (int i = 0; i < 4; i++) {
    if (i == 0) setLeds(false, false, true);
    if (i == 1) setLeds(true,  false, false);
    if (i == 2) setLeds(false, true,  false);
    if (i == 3) setLeds(true,  true,  true);
    playNote(notes[i], durs[i]);
    setLeds(false, false, false);
    delay(60);
  }

  // LED show: runway lights chase (Y -> G -> R -> G -> Y) x2
  const int chase[] = { 0, 1, 2, 1, 0, 1, 2, 1, 0 };
  for (int i = 0; i < 9; i++) {
    setLeds(chase[i] == 1, chase[i] == 2, chase[i] == 0);
    delay(90);
  }
  // all LEDs blink 3 times
  for (int i = 0; i < 3; i++) {
    setLeds(true, true, true);    delay(150);
    setLeds(false, false, false); delay(150);
  }
}

// Clears the old pass and goes back to the start screen (for the next traveller)
void resetKiosk() {
  Serial.println(">>> Kiosk reset");
  passHtml = "";                 // phone page goes back to "No pass yet"
  plannedBy = "";
  showingQR = false;
  showingAd = false;
  setLeds(false, false, false);
  showMessage("  * DING DONG *", "Kiosk reset!", "Ready for next traveller");
  airportChime();
  delay(500);
  lastShownHours = -1;           // redraw menu
  lastAdEnd = millis();
}

// Reset button: short press = reset kiosk, hold 3 s = full restart
void checkResetButton() {
  if (digitalRead(RESET_BTN_PIN) != LOW) return;
  delay(30);                                 // debounce
  if (digitalRead(RESET_BTN_PIN) != LOW) return;

  unsigned long t0 = millis();
  bool restartShown = false;
  while (digitalRead(RESET_BTN_PIN) == LOW) {
    if (!restartShown && millis() - t0 > 3000) {
      restartShown = true;
      setLeds(true, true, true);
      showMessage("Restarting...", "", "Release button");
      beep(300);
    }
    server.handleClient();
    delay(10);
  }
  if (restartShown) {
    Serial.println(">>> Full restart");
    delay(300);
    ESP.restart();
  } else {
    resetKiosk();
  }
}

bool buttonDown() {
  return digitalRead(BUTTON_PIN) == LOW || digitalRead(BOOT_BTN_PIN) == LOW;
}

void loop() {
  server.handleClient();
  checkResetButton();

  // ---- Button (falling edge + debounce) ----
  static bool lastBtn = false;
  bool btn = buttonDown();
  if (btn && !lastBtn) {
    delay(30);
    if (buttonDown()) {
      Serial.println(">>> Button pressed!");
      if (showingQR) {                 // press while QR shown -> back to menu
        showingQR = false;
        setLeds(false, false, false);
        lastShownHours = -1;
        lastAdEnd = millis();
      } else {
        generatePass();
      }
    }
  }
  lastBtn = btn;

  // ---- QR timeout ----
  if (showingQR && millis() - qrShownAt > QR_TIMEOUT_MS) {
    showingQR = false;
    setLeds(false, false, false);
    lastShownHours = -1;
    lastAdEnd = millis();
  }

  if (!showingQR) {
    int hrs = readLayoverHours();

    // ---- Ad rotation while idle ----
    if (showingAd) {
      bool knobMoved = (hrs != layoverHours);
      if (millis() - adStartedAt > AD_SHOW_MS || knobMoved) {
        showingAd = false;
        lastAdEnd = millis();
        lastShownHours = -1;           // redraw menu
      }
    } else if (millis() - lastAdEnd > AD_EVERY_MS) {
      showingAd = true;
      adStartedAt = millis();
      showAd();
    }

    // ---- Menu ----
    if (!showingAd) {
      layoverHours = hrs;
      if (layoverHours != lastShownHours) {
        lastShownHours = layoverHours;
        showMenu();
      }
    }
  }

  delay(20);
}