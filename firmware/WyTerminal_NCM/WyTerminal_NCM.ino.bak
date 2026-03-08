/*
 * WyTerminal — LilyGO T-Display S3 AMOLED (1.91" RM67162)
 *
 * USB HID keyboard relay + Telegram bot + live terminal display
 *
 * Board settings (Arduino IDE):
 *   Board:          LilyGo T-Display-S3
 *   USB Mode:       USB-OTG (TinyUSB)   ← CRITICAL
 *   USB CDC On Boot: Enabled
 *   Flash Size:     16MB
 *   Partition:      16M Flash (3MB APP/9.9MB FATFS)
 *
 * Libraries:
 *   - Arduino_GFX (github.com/moononournation/Arduino_GFX)
 *   - ArduinoJson
 */

#include "USB.h"
#include "USBHIDKeyboard.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>

// ── Config ────────────────────────────────────────────────────────────
#define WIFI_SSID        "D-Link the router"
#define WIFI_PASSWORD    "Ajeip853jw5590!"
#define BOT_TOKEN        "8688942400:AAFZKipOJnzroUWAea-zZuhZbLbRTiAluLM"
#define ALLOWED_CHAT_ID  1790655432LL

// ── Display pins (T-Display S3 AMOLED 1.91") ─────────────────────────
#define LCD_CS   6
#define LCD_SCK  47
#define LCD_D0   18
#define LCD_D1   7
#define LCD_D2   48
#define LCD_D3   5
#define LCD_RST  17
#define LCD_PWR  38   // display power enable (touch version)

// ── Colours ───────────────────────────────────────────────────────────
#define C_BG      0x0000
#define C_GREEN   0x07E0
#define C_RED     0xF800
#define C_YELLOW  0xFFE0
#define C_CYAN    0x07FF
#define C_WHITE   0xFFFF
#define C_GREY    0x4208
#define C_DKGREEN 0x0200
#define C_DKBLUE  0x000A

// ── Terminal layout ───────────────────────────────────────────────────
// AMOLED is portrait 240x536
#define SCREEN_W   240
#define SCREEN_H   536
#define HEADER_H    26
#define FOOTER_H    20
#define FONT_W       6
#define FONT_H      12
#define TERM_Y      (HEADER_H + 4)
#define TERM_H      (SCREEN_H - HEADER_H - FOOTER_H - 8)
#define TERM_LINES  (TERM_H / FONT_H)    // ~40 lines
#define TERM_COLS   (SCREEN_W / FONT_W)  // 40 chars

// ── Globals ───────────────────────────────────────────────────────────
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCK, LCD_D0, LCD_D1, LCD_D2, LCD_D3);
Arduino_GFX *gfx = new Arduino_RM67162(bus, LCD_RST, 0 /* rotation */);

USBHIDKeyboard Keyboard;
WiFiClientSecure tls_client;

static long     s_offset = 0;
static uint32_t s_start_ms;
static char     s_last_text[512] = "";
static bool     s_wifi_ok = false;

// Terminal ring buffer
struct Line { char text[TERM_COLS + 1]; uint16_t col; };
static Line  s_buf[TERM_LINES];
static int   s_count = 0;

// ── Brightness ────────────────────────────────────────────────────────
void set_brightness(uint8_t v) {
    bus->beginWrite();
    bus->writeC8D8(0x51, v);
    bus->endWrite();
}

// ── Terminal helpers ──────────────────────────────────────────────────
void term_push(const char *text, uint16_t col) {
    if (s_count < TERM_LINES) {
        strncpy(s_buf[s_count].text, text, TERM_COLS);
        s_buf[s_count].col = col;
        s_count++;
    } else {
        memmove(s_buf, s_buf + 1, sizeof(Line) * (TERM_LINES - 1));
        strncpy(s_buf[TERM_LINES-1].text, text, TERM_COLS);
        s_buf[TERM_LINES-1].col = col;
    }
    // Redraw terminal area
    gfx->fillRect(0, TERM_Y, SCREEN_W, TERM_H, C_BG);
    int start = (s_count > TERM_LINES) ? s_count - TERM_LINES : 0;
    for (int i = start; i < s_count; i++) {
        int y = TERM_Y + (i - start) * FONT_H;
        gfx->setTextColor(s_buf[i].col);
        gfx->setCursor(2, y);
        gfx->print(s_buf[i].text);
    }
}

void term_cmd(const char *s)  { char b[42]; snprintf(b,42,"> %.39s",s); term_push(b, C_CYAN);   }
void term_ok(const char *s)   { char b[42]; snprintf(b,42,"  %.39s",s); term_push(b, C_GREEN);  }
void term_err(const char *s)  { char b[42]; snprintf(b,42,"  %.39s",s); term_push(b, C_RED);    }
void term_info(const char *s) { char b[42]; snprintf(b,42,"  %.39s",s); term_push(b, C_GREY);   }
void term_head(const char *s) { char b[42]; snprintf(b,42,"%.41s",s);   term_push(b, C_YELLOW); }

void draw_header() {
    gfx->fillRect(0, 0, SCREEN_W, HEADER_H, C_DKGREEN);
    gfx->setTextColor(C_GREEN);
    gfx->setTextSize(1);
    gfx->setCursor(4, 8);
    gfx->print("WyTerminal");
    // Wifi indicator
    gfx->setCursor(SCREEN_W - 28, 8);
    gfx->setTextColor(s_wifi_ok ? C_GREEN : C_RED);
    gfx->print(s_wifi_ok ? " LIVE" : " WAIT");
}

void draw_footer() {
    int y = SCREEN_H - FOOTER_H;
    gfx->fillRect(0, y, SCREEN_W, FOOTER_H, C_DKBLUE);
    gfx->setTextColor(C_GREY);
    gfx->setTextSize(1);
    gfx->setCursor(2, y + 4);
    uint32_t up = (millis() - s_start_ms) / 1000;
    char buf[40];
    if (s_wifi_ok)
        snprintf(buf, sizeof(buf), "%s  %ddBm  %lus", WiFi.localIP().toString().c_str(), WiFi.RSSI(), (unsigned long)up);
    else
        snprintf(buf, sizeof(buf), "Connecting...  %lus", (unsigned long)up);
    gfx->print(buf);
}

// ── Telegram ──────────────────────────────────────────────────────────
String tg_post(const char *method, const String &body) {
    if (!tls_client.connect("api.telegram.org", 443)) return "";
    String path = String("/bot") + BOT_TOKEN + "/" + method;
    tls_client.println("POST " + path + " HTTP/1.1");
    tls_client.println("Host: api.telegram.org");
    tls_client.println("Content-Type: application/json");
    tls_client.println("Content-Length: " + String(body.length()));
    tls_client.println("Connection: close");
    tls_client.println();
    tls_client.print(body);
    String resp;
    uint32_t t = millis();
    while (tls_client.connected() && millis() - t < 6000) {
        while (tls_client.available()) resp += (char)tls_client.read();
    }
    tls_client.stop();
    int idx = resp.indexOf("\r\n\r\n");
    return (idx >= 0) ? resp.substring(idx + 4) : "";
}

void tg_send(long long chat_id, const char *text) {
    StaticJsonDocument<512> doc;
    doc["chat_id"] = chat_id;
    doc["text"]    = text;
    String body; serializeJson(doc, body);
    tg_post("sendMessage", body);
}

// ── HID helpers ───────────────────────────────────────────────────────
void hid_type(const char *text, bool enter) {
    Keyboard.print(text);
    if (enter) { delay(50); Keyboard.press(KEY_RETURN); delay(50); Keyboard.releaseAll(); }
}

// ── Process one Telegram update ───────────────────────────────────────
void handle_update(JsonObject &upd) {
    if (!upd.containsKey("message")) return;
    JsonObject msg = upd["message"];
    long long chat_id = msg["chat"]["id"];
    if (chat_id != ALLOWED_CHAT_ID) return;
    const char *text = msg["text"] | "";
    if (!text[0]) return;

    String t(text);
    String short_t = t.length() > 30 ? t.substring(0, 29) + ">" : t;
    term_cmd(short_t.c_str());
    draw_footer();

    if (t.startsWith("/run ")) {
        String cmd = t.substring(5);
        hid_type(cmd.c_str(), true);
        tg_send(chat_id, ("▶️ " + cmd).c_str());
        term_ok("sent + Enter");

    } else if (t.startsWith("/type ")) {
        String s = t.substring(6);
        strncpy(s_last_text, s.c_str(), sizeof(s_last_text)-1);
        hid_type(s.c_str(), false);
        tg_send(chat_id, "⌨️ typed");
        term_ok("typed");

    } else if (t == "/enter") {
        Keyboard.press(KEY_RETURN); delay(50); Keyboard.releaseAll();
        tg_send(chat_id, "↵ Enter");
        term_ok("Enter");

    } else if (t == "/paste") {
        if (!s_last_text[0]) { tg_send(chat_id, "❌ nothing to paste"); term_err("nothing"); return; }
        hid_type(s_last_text, false);
        tg_send(chat_id, "📋 pasted");
        term_ok("pasted");

    } else if (t.startsWith("/key ")) {
        String combo = t.substring(5); combo.toLowerCase();
        bool ctrl  = combo.indexOf("ctrl")  >= 0;
        bool alt   = combo.indexOf("alt")   >= 0;
        bool shift = combo.indexOf("shift") >= 0;
        bool sup   = combo.indexOf("super") >= 0 || combo.indexOf("win") >= 0;
        int lp = combo.lastIndexOf("+");
        String ks  = (lp >= 0) ? combo.substring(lp+1) : combo; ks.trim();
        uint8_t key = 0;
        if      (ks=="t")     key='t'; else if (ks=="c")  key='c';
        else if (ks=="v")     key='v'; else if (ks=="z")  key='z';
        else if (ks=="a")     key='a'; else if (ks=="x")  key='x';
        else if (ks=="f4")    key=KEY_F4; else if (ks=="f5") key=KEY_F5;
        else if (ks=="tab")   key=KEY_TAB; else if (ks=="esc") key=KEY_ESC;
        else if (ks=="space") key=' ';
        else if (ks=="up")    key=KEY_UP_ARROW;
        else if (ks=="down")  key=KEY_DOWN_ARROW;
        else if (ks=="left")  key=KEY_LEFT_ARROW;
        else if (ks=="right") key=KEY_RIGHT_ARROW;
        else if (ks.length()==1) key=(uint8_t)ks[0];
        if (key) {
            if (ctrl)  Keyboard.press(KEY_LEFT_CTRL);
            if (alt)   Keyboard.press(KEY_LEFT_ALT);
            if (shift) Keyboard.press(KEY_LEFT_SHIFT);
            if (sup)   Keyboard.press(KEY_LEFT_GUI);
            Keyboard.press(key); delay(100); Keyboard.releaseAll();
            tg_send(chat_id, ("⌨️ " + combo).c_str());
            term_ok(combo.c_str());
        } else {
            tg_send(chat_id, "❓ unknown key");
            term_err("unknown key");
        }

    } else if (t == "/clear") {
        s_count = 0;
        gfx->fillRect(0, TERM_Y, SCREEN_W, TERM_H, C_BG);
        tg_send(chat_id, "🧹 cleared");

    } else if (t == "/status") {
        uint32_t up = (millis() - s_start_ms) / 1000;
        char buf[128];
        snprintf(buf, sizeof(buf), "✅ WyTerminal\nIP: %s\nRSSI: %ddBm\nUptime: %lus",
            WiFi.localIP().toString().c_str(), WiFi.RSSI(), (unsigned long)up);
        tg_send(chat_id, buf);
        term_info("status sent");

    } else if (t == "/help") {
        tg_send(chat_id,
            "WyTerminal commands:\n"
            "/run <cmd> — type+Enter\n"
            "/type <text> — type only\n"
            "/enter — Enter key\n"
            "/paste — retype last\n"
            "/key <combo> — key combo\n"
            "  ctrl+alt+t, ctrl+c, super\n"
            "/clear — clear display\n"
            "/status — IP+uptime\n"
            "/help — this list");
        term_info("help sent");
    } else {
        tg_send(chat_id, "❓ /help for commands");
        term_err("unknown cmd");
    }
}

void poll_telegram() {
    StaticJsonDocument<128> req;
    req["offset"]  = s_offset + 1;
    req["timeout"] = 0;
    req["limit"]   = 5;
    String body; serializeJson(req, body);
    String resp = tg_post("getUpdates", body);
    if (!resp.length()) return;
    DynamicJsonDocument doc(8192);
    if (deserializeJson(doc, resp)) return;
    if (!doc["ok"]) return;
    for (JsonObject upd : doc["result"].as<JsonArray>()) {
        long uid = upd["update_id"];
        if (uid > s_offset) s_offset = uid;
        handle_update(upd);
    }
}

// ── Setup ─────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);

    // Power on display (touch version)
    pinMode(LCD_PWR, OUTPUT);
    digitalWrite(LCD_PWR, HIGH);
    delay(50);

    // Init display
    if (!gfx->begin()) {
        Serial.println("Display init failed!");
        while(1) delay(1000);
    }
    set_brightness(200);

    gfx->fillScreen(C_BG);
    gfx->setTextSize(1);
    gfx->setTextWrap(false);

    draw_header();
    draw_footer();

    term_head("WyTerminal v2.0");
    term_info("by Wyltek Industries");
    term_info("──────────────────");

    // USB HID
    USB.begin();
    Keyboard.begin();
    delay(200);
    term_ok("USB HID ready");

    // WiFi
    tls_client.setInsecure();
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    term_info("WiFi connecting...");
    draw_header();

    uint32_t t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 30000) {
        delay(500);
    }

    s_start_ms = millis();

    if (WiFi.status() == WL_CONNECTED) {
        s_wifi_ok = true;
        draw_header();
        char buf[40];
        snprintf(buf, sizeof(buf), "%s", WiFi.localIP().toString().c_str());
        term_ok(buf);
        term_ok("Telegram ready");
        term_info("Send /help to bot");
    } else {
        term_err("WiFi failed!");
    }
    draw_footer();
}

// ── Loop ──────────────────────────────────────────────────────────────
static uint32_t s_last_footer = 0;
void loop() {
    if (s_wifi_ok) {
        // Check WiFi reconnect
        if (WiFi.status() != WL_CONNECTED) {
            s_wifi_ok = false;
            draw_header();
            WiFi.reconnect();
        }
        poll_telegram();
    } else if (WiFi.status() == WL_CONNECTED) {
        s_wifi_ok = true;
        draw_header();
    }

    // Update footer every 5s
    if (millis() - s_last_footer > 5000) {
        draw_footer();
        s_last_footer = millis();
    }
    delay(1000);
}
