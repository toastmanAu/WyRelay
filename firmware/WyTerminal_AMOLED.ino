/*
 * WyTerminal — LilyGO T-Display S3 AMOLED
 *
 * ESP32-S3 + RM67162 AMOLED (536x240, QSPI)
 * - USB HID keyboard relay (native USB port → target machine)
 * - Telegram bot command input
 * - AMOLED terminal display showing command log
 *
 * Board: LilyGO T-Display S3 AMOLED
 * USB Mode: USB-OTG (TinyUSB)
 * USB CDC On Boot: Enabled
 *
 * Libraries needed:
 *   - ArduinoJson
 *   - TFT_eSPI (with LilyGO AMOLED config)
 */

#include "USB.h"
#include "USBHIDKeyboard.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <SPI.h>

// ── Config ────────────────────────────────────────────────────────────
#define WIFI_SSID        "D-Link the router"
#define WIFI_PASSWORD    "Ajeip853jw5590!"
#define BOT_TOKEN        "8688942400:AAFZKipOJnzroUWAea-zZuhZbLbRTiAluLM"
#define ALLOWED_CHAT_ID   1790655432

// ── Display (RM67162 AMOLED via QSPI) ────────────────────────────────
#define TFT_WIDTH    240
#define TFT_HEIGHT   536

// ── Colours ───────────────────────────────────────────────────────────
#define C_BG         0x0000   // Pure black (AMOLED = true black = off pixels)
#define C_GREEN      0x07E0   // Terminal green
#define C_RED        0xF800
#define C_YELLOW     0xFFE0
#define C_CYAN       0x07FF
#define C_WHITE      0xFFFF
#define C_GREY       0x4208
#define C_DIM        0x2104

// ── Terminal config ───────────────────────────────────────────────────
#define TERM_LINES     18     // Visible lines on screen
#define TERM_COLS      30     // Chars per line (approx at font size 2)
#define FONT_H         14     // Pixel height per line
#define FONT_W          6     // Pixel width per char
#define HEADER_H       28     // Header bar height
#define STATUS_H       18     // Status bar height at bottom

// ── Globals ───────────────────────────────────────────────────────────
USBHIDKeyboard Keyboard;
TFT_eSPI tft = TFT_eSPI();

static long     s_last_update_id = 0;
static uint32_t s_start_ms;
static char     s_last_text[512] = "";
static bool     s_wifi_ok = false;
static WiFiClientSecure tls_client;

// Terminal line buffer
struct TermLine {
    char    text[64];
    uint16_t colour;
};
static TermLine s_lines[TERM_LINES];
static int s_line_count = 0;
static int s_scroll_top = 0;

// ── Terminal display ──────────────────────────────────────────────────
void term_init() {
    tft.init();
    tft.setRotation(0);      // Portrait — 240 wide, 536 tall
    tft.fillScreen(C_BG);
    tft.setTextWrap(false);

    // Header
    tft.fillRect(0, 0, TFT_WIDTH, HEADER_H, 0x0410); // dark green
    tft.setTextColor(C_GREEN, 0x0410);
    tft.setTextSize(2);
    tft.setCursor(6, 6);
    tft.print("WyTerminal");

    // WiFi icon placeholder
    tft.setTextSize(1);
    tft.setTextColor(C_GREY, 0x0410);
    tft.setCursor(TFT_WIDTH - 30, 10);
    tft.print("...");
}

void term_draw_status() {
    int y = TFT_HEIGHT - STATUS_H;
    tft.fillRect(0, y, TFT_WIDTH, STATUS_H, 0x0208);
    tft.setTextSize(1);
    tft.setTextColor(C_GREY, 0x0208);
    tft.setCursor(4, y + 4);

    uint32_t up = (millis() - s_start_ms) / 1000;
    char buf[40];
    if (s_wifi_ok) {
        snprintf(buf, sizeof(buf), "WiFi %ddBm  up %lus", WiFi.RSSI(), (unsigned long)up);
    } else {
        snprintf(buf, sizeof(buf), "WiFi: connecting...");
    }
    tft.print(buf);
}

void term_draw_wifi_icon() {
    tft.setTextSize(1);
    uint16_t c = s_wifi_ok ? C_GREEN : C_RED;
    tft.setTextColor(c, 0x0410);
    tft.setCursor(TFT_WIDTH - 30, 10);
    tft.print(s_wifi_ok ? " OK " : " -- ");
}

void term_redraw_lines() {
    int y_start = HEADER_H + 4;
    int max_lines = (TFT_HEIGHT - HEADER_H - STATUS_H - 4) / FONT_H;

    tft.fillRect(0, HEADER_H, TFT_WIDTH, TFT_HEIGHT - HEADER_H - STATUS_H, C_BG);

    int start = (s_line_count > max_lines) ? s_line_count - max_lines : 0;
    for (int i = start; i < s_line_count; i++) {
        int y = y_start + (i - start) * FONT_H;
        tft.setTextSize(1);
        tft.setTextColor(s_lines[i].colour, C_BG);
        tft.setCursor(4, y);
        // Truncate to fit width
        char buf[TERM_COLS + 1];
        strncpy(buf, s_lines[i].text, TERM_COLS);
        buf[TERM_COLS] = '\0';
        tft.print(buf);
    }
}

void term_add(const char* text, uint16_t colour = C_GREEN) {
    if (s_line_count < TERM_LINES) {
        strncpy(s_lines[s_line_count].text, text, 63);
        s_lines[s_line_count].colour = colour;
        s_line_count++;
    } else {
        // Scroll up
        for (int i = 0; i < TERM_LINES - 1; i++) {
            s_lines[i] = s_lines[i + 1];
        }
        strncpy(s_lines[TERM_LINES - 1].text, text, 63);
        s_lines[TERM_LINES - 1].colour = colour;
    }
    term_redraw_lines();
    term_draw_status();
}

void term_cmd(const char* cmd) {
    char buf[66];
    snprintf(buf, sizeof(buf), "> %s", cmd);
    term_add(buf, C_CYAN);
}

void term_ok(const char* msg) {
    char buf[66];
    snprintf(buf, sizeof(buf), "  %s", msg);
    term_add(buf, C_GREEN);
}

void term_err(const char* msg) {
    char buf[66];
    snprintf(buf, sizeof(buf), "  %s", msg);
    term_add(buf, C_RED);
}

void term_info(const char* msg) {
    char buf[66];
    snprintf(buf, sizeof(buf), "  %s", msg);
    term_add(buf, C_GREY);
}

// ── Telegram ──────────────────────────────────────────────────────────
static String tg_post(const String &method, const String &body) {
    if (!tls_client.connect("api.telegram.org", 443)) return "";
    String path = "/bot" + String(BOT_TOKEN) + "/" + method;
    tls_client.println("POST " + path + " HTTP/1.1");
    tls_client.println("Host: api.telegram.org");
    tls_client.println("Content-Type: application/json");
    tls_client.println("Content-Length: " + String(body.length()));
    tls_client.println("Connection: close");
    tls_client.println();
    tls_client.print(body);
    String response = "";
    uint32_t t = millis();
    while (tls_client.connected() && millis() - t < 5000) {
        while (tls_client.available()) response += (char)tls_client.read();
    }
    tls_client.stop();
    int idx = response.indexOf("\r\n\r\n");
    if (idx >= 0) response = response.substring(idx + 4);
    return response;
}

static void tg_send(long chat_id, const String &text) {
    StaticJsonDocument<512> doc;
    doc["chat_id"] = chat_id;
    doc["text"] = text;
    String body; serializeJson(doc, body);
    tg_post("sendMessage", body);
}

static void hid_type(const char *text, bool enter) {
    Keyboard.print(text);
    if (enter) { delay(50); Keyboard.press(KEY_RETURN); delay(50); Keyboard.releaseAll(); }
}

static void process_update(JsonObject &update) {
    if (!update.containsKey("message")) return;
    JsonObject msg = update["message"];
    long chat_id = msg["chat"]["id"];
    if (chat_id != ALLOWED_CHAT_ID) return;
    String text = msg["text"] | "";
    if (!text.length()) return;

    // Show command on display
    String display_cmd = text.length() > 28 ? text.substring(0, 28) + ".." : text;
    term_cmd(display_cmd.c_str());

    if (text.startsWith("/run ")) {
        String cmd = text.substring(5);
        hid_type(cmd.c_str(), true);
        tg_send(chat_id, "▶️ " + cmd);
        term_ok("sent + Enter");

    } else if (text.startsWith("/type ")) {
        String t = text.substring(6);
        strncpy(s_last_text, t.c_str(), sizeof(s_last_text)-1);
        hid_type(t.c_str(), false);
        tg_send(chat_id, "⌨️ typed");
        term_ok("typed");

    } else if (text == "/enter") {
        Keyboard.press(KEY_RETURN); delay(50); Keyboard.releaseAll();
        tg_send(chat_id, "↵");
        term_ok("Enter");

    } else if (text == "/paste") {
        if (!strlen(s_last_text)) { tg_send(chat_id, "❌ nothing to paste"); term_err("nothing"); return; }
        hid_type(s_last_text, false);
        tg_send(chat_id, "📋 pasted");
        term_ok("pasted");

    } else if (text.startsWith("/key ")) {
        String combo = text.substring(5); combo.toLowerCase();
        bool ctrl  = combo.indexOf("ctrl")  >= 0;
        bool alt   = combo.indexOf("alt")   >= 0;
        bool shift = combo.indexOf("shift") >= 0;
        bool super_ = combo.indexOf("super") >= 0 || combo.indexOf("win") >= 0;
        int lp = combo.lastIndexOf("+");
        String ks = (lp >= 0) ? combo.substring(lp+1) : combo; ks.trim();
        uint8_t key = 0;
        if      (ks=="t")     key='t'; else if (ks=="c") key='c';
        else if (ks=="v")     key='v'; else if (ks=="z") key='z';
        else if (ks=="a")     key='a'; else if (ks=="x") key='x';
        else if (ks=="f4")    key=KEY_F4; else if (ks=="f5") key=KEY_F5;
        else if (ks=="tab")   key=KEY_TAB; else if (ks=="esc") key=KEY_ESC;
        else if (ks=="space") key=' '; else if (ks=="up") key=KEY_UP_ARROW;
        else if (ks=="down")  key=KEY_DOWN_ARROW;
        else if (ks.length()==1) key=ks[0];
        if (key) {
            if (ctrl)  Keyboard.press(KEY_LEFT_CTRL);
            if (alt)   Keyboard.press(KEY_LEFT_ALT);
            if (shift) Keyboard.press(KEY_LEFT_SHIFT);
            if (super_) Keyboard.press(KEY_LEFT_GUI);
            Keyboard.press(key); delay(100); Keyboard.releaseAll();
            tg_send(chat_id, "⌨️ " + combo);
            term_ok(combo.c_str());
        } else {
            tg_send(chat_id, "❓ unknown key");
            term_err("unknown key");
        }

    } else if (text == "/status") {
        uint32_t up = (millis() - s_start_ms) / 1000;
        String s = "✅ WyTerminal\nIP: " + WiFi.localIP().toString() +
                   "\nSignal: " + String(WiFi.RSSI()) + "dBm\nUptime: " + String(up) + "s";
        tg_send(chat_id, s);
        term_info("status sent");

    } else if (text == "/clear") {
        s_line_count = 0;
        term_redraw_lines();
        tg_send(chat_id, "🧹 display cleared");

    } else if (text == "/help") {
        tg_send(chat_id,
            "WyTerminal commands:\n/run <cmd> — type+Enter\n/type <text> — type\n"
            "/enter — Enter key\n/paste — retype last\n/key <combo>\n"
            "/clear — clear display\n/status\n/help");
        term_info("help sent");
    } else {
        tg_send(chat_id, "❓ /help for commands");
        term_err("unknown cmd");
    }
}

static void poll_telegram() {
    StaticJsonDocument<64> req;
    req["offset"] = s_last_update_id + 1;
    req["timeout"] = 0; req["limit"] = 5;
    String body; serializeJson(req, body);
    String resp = tg_post("getUpdates", body);
    if (!resp.length()) return;
    DynamicJsonDocument doc(8192);
    if (deserializeJson(doc, resp)) return;
    if (!doc["ok"]) return;
    JsonArray results = doc["result"];
    for (JsonObject update : results) {
        long uid = update["update_id"];
        if (uid > s_last_update_id) s_last_update_id = uid;
        process_update(update);
    }
}

// ── WiFi event ────────────────────────────────────────────────────────
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_ok = false;
        term_draw_wifi_icon();
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_wifi_ok = true;
        term_draw_wifi_icon();
        ip_event_got_ip_t *ev = (ip_event_got_ip_t*)data;
        char buf[32];
        snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ev->ip_info.ip));
        term_ok(buf);
    }
}

// ── Setup ─────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);

    // Display first
    term_init();
    term_add("WyTerminal v2.0", C_YELLOW);
    term_info("by Wyltek Industries");
    term_info("Initialising...");

    // USB HID
    USB.begin();
    Keyboard.begin();
    delay(500);
    term_ok("USB HID ready");

    // WiFi
    WiFi.mode(WIFI_STA);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event, NULL);
    esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, &wifi_event, NULL);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    term_info("WiFi connecting...");

    tls_client.setInsecure();
    s_start_ms = millis();

    // Wait up to 30s for WiFi
    uint32_t t = millis();
    while (!s_wifi_ok && millis() - t < 30000) delay(500);

    if (s_wifi_ok) {
        term_ok("Telegram ready");
        term_add("Send /help to bot", C_CYAN);
    } else {
        term_err("WiFi failed!");
    }

    term_draw_status();
}

// ── Loop ──────────────────────────────────────────────────────────────
void loop() {
    if (s_wifi_ok) poll_telegram();
    term_draw_status();
    delay(1000);
}
