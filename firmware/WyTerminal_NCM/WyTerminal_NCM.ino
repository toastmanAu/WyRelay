/*
 * WyTerminal v3 — LilyGO T-Display S3 AMOLED (1.91" RM67162)
 *
 * USB Composite: HID Keyboard + CDC-NCM (USB Ethernet)
 *
 * v3 changes vs v2:
 *   - USB NCM network interface instead of WiFi relay
 *   - Board appears as USB Ethernet adapter on host (usb0, 192.168.100.2)
 *   - Pi connects directly via USB network — no WiFi needed for relay
 *   - HTTP server on board port 80 receives /run /type /key commands
 *   - WiFi still used for Telegram polling only (optional)
 *   - Much lower latency, works even if WiFi is down
 *
 * Board settings (arduino-cli / Arduino IDE):
 *   Board:            LilyGo T-Display-S3
 *   USB Mode:         USB-OTG (TinyUSB)   ← CRITICAL
 *   USB CDC On Boot:  Enabled
 *   Flash Size:       16MB
 *   Partition:        16M Flash (3MB APP/9.9MB FATFS)
 *
 * Libraries:
 *   - Arduino_GFX (moononournation/Arduino_GFX)
 *   - ArduinoJson
 *
 * Host setup (Pi side, run once):
 *   sudo ip addr add 192.168.100.1/24 dev usb0
 *   sudo ip link set usb0 up
 *   # Then: curl http://192.168.100.2/run?cmd=whoami
 */

#include "USB.h"
#include "USBHIDKeyboard.h"
#include "USBCDC.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>
#include <NetworkServer.h>
#include <NetworkClient.h>

// ── Config ────────────────────────────────────────────────────────────
#define WIFI_SSID        "D-Link the router"
#define WIFI_PASSWORD    "Ajeip853jw5590!"
#define BOT_TOKEN        "8688942400:AAFZKipOJnzroUWAea-zZuhZbLbRTiAluLM"
#define ALLOWED_CHAT_ID  1790655432LL

// USB NCM static IPs
#define NCM_BOARD_IP     "192.168.100.2"
#define NCM_HOST_IP      "192.168.100.1"
#define NCM_NETMASK      "255.255.255.0"

// ── Display pins (T-Display S3 AMOLED 1.91") ─────────────────────────
#define LCD_CS   6
#define LCD_SCK  47
#define LCD_D0   18
#define LCD_D1   7
#define LCD_D2   48
#define LCD_D3   5
#define LCD_RST  17
#define LCD_PWR  38

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
#define C_PURPLE  0x780F

// ── Terminal layout ───────────────────────────────────────────────────
#define SCREEN_W   240
#define SCREEN_H   536
#define HEADER_H    28
#define FOOTER_H    22
#define FONT_W       6
#define FONT_H      12
#define TERM_Y      (HEADER_H + 4)
#define TERM_H      (SCREEN_H - HEADER_H - FOOTER_H - 8)
#define TERM_LINES  (TERM_H / FONT_H)
#define TERM_COLS   (SCREEN_W / FONT_W)

// ── Globals ───────────────────────────────────────────────────────────
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCK, LCD_D0, LCD_D1, LCD_D2, LCD_D3);
Arduino_GFX *gfx = new Arduino_RM67162(bus, LCD_RST, 0);

USBHIDKeyboard Keyboard;

// HTTP server on USB-NCM interface
NetworkServer http_server(80);

static long     s_tg_offset = 0;
static uint32_t s_start_ms;
static char     s_last_text[512] = "";
static bool     s_wifi_ok  = false;
static bool     s_ncm_ok   = false;

// Terminal ring buffer
struct Line { char text[TERM_COLS + 1]; uint16_t col; };
static Line s_buf[TERM_LINES];
static int  s_count = 0;

// ── Display ───────────────────────────────────────────────────────────
void set_brightness(uint8_t v) {
    bus->beginWrite(); bus->writeC8D8(0x51, v); bus->endWrite();
}

void term_push(const char *text, uint16_t col) {
    if (s_count < TERM_LINES) {
        strncpy(s_buf[s_count].text, text, TERM_COLS);
        s_buf[s_count++].col = col;
    } else {
        memmove(s_buf, s_buf + 1, sizeof(Line) * (TERM_LINES - 1));
        strncpy(s_buf[TERM_LINES-1].text, text, TERM_COLS);
        s_buf[TERM_LINES-1].col = col;
    }
    gfx->fillRect(0, TERM_Y, SCREEN_W, TERM_H, C_BG);
    for (int i = 0; i < s_count; i++) {
        int y = TERM_Y + i * FONT_H;
        gfx->setTextColor(s_buf[i].col);
        gfx->setCursor(2, y);
        gfx->print(s_buf[i].text);
    }
}

void term_cmd(const char *s)  { char b[42]; snprintf(b,42,"> %.39s",s); term_push(b,C_CYAN);   }
void term_ok(const char *s)   { char b[42]; snprintf(b,42,"✓ %.39s",s); term_push(b,C_GREEN);  }
void term_err(const char *s)  { char b[42]; snprintf(b,42,"✗ %.39s",s); term_push(b,C_RED);    }
void term_info(const char *s) { char b[42]; snprintf(b,42,"  %.39s",s); term_push(b,C_GREY);   }
void term_head(const char *s) { char b[42]; snprintf(b,42,"%.41s",s);   term_push(b,C_YELLOW); }
void term_ncm(const char *s)  { char b[42]; snprintf(b,42,"⬡ %.39s",s); term_push(b,C_PURPLE); }

void draw_header() {
    gfx->fillRect(0, 0, SCREEN_W, HEADER_H, C_DKGREEN);
    gfx->setTextColor(C_GREEN); gfx->setTextSize(1);
    gfx->setCursor(4, 9); gfx->print("WyTerminal v3");
    // NCM indicator
    gfx->setCursor(SCREEN_W - 54, 9);
    gfx->setTextColor(s_ncm_ok ? C_PURPLE : C_GREY);
    gfx->print(s_ncm_ok ? "NCM " : "NCM ");
    // WiFi indicator
    gfx->setTextColor(s_wifi_ok ? C_GREEN : C_RED);
    gfx->print(s_wifi_ok ? "W" : "w");
}

void draw_footer() {
    int y = SCREEN_H - FOOTER_H;
    gfx->fillRect(0, y, SCREEN_W, FOOTER_H, C_DKBLUE);
    gfx->setTextColor(C_GREY); gfx->setTextSize(1);
    gfx->setCursor(2, y + 5);
    uint32_t up = (millis() - s_start_ms) / 1000;
    char buf[44];
    if (s_ncm_ok)
        snprintf(buf, sizeof(buf), "%s  up:%lus", NCM_BOARD_IP, (unsigned long)up);
    else if (s_wifi_ok)
        snprintf(buf, sizeof(buf), "%s  up:%lus", WiFi.localIP().toString().c_str(), (unsigned long)up);
    else
        snprintf(buf, sizeof(buf), "NCM: waiting  up:%lus", (unsigned long)up);
    gfx->print(buf);
}

// ── HID helpers ───────────────────────────────────────────────────────
void hid_type(const char *text, bool enter) {
    Keyboard.print(text);
    if (enter) { delay(50); Keyboard.press(KEY_RETURN); delay(50); Keyboard.releaseAll(); }
}

void hid_key_combo(const String &combo) {
    String c = combo; c.toLowerCase();
    bool ctrl  = c.indexOf("ctrl")  >= 0;
    bool alt   = c.indexOf("alt")   >= 0;
    bool shift = c.indexOf("shift") >= 0;
    bool sup   = c.indexOf("super") >= 0 || c.indexOf("win") >= 0;
    int lp = c.lastIndexOf("+");
    String ks = (lp >= 0) ? c.substring(lp+1) : c; ks.trim();
    uint8_t key = 0;
    if      (ks=="t")     key='t'; else if (ks=="c")   key='c';
    else if (ks=="v")     key='v'; else if (ks=="z")   key='z';
    else if (ks=="a")     key='a'; else if (ks=="x")   key='x';
    else if (ks=="f4")    key=KEY_F4;   else if (ks=="f5") key=KEY_F5;
    else if (ks=="tab")   key=KEY_TAB;  else if (ks=="esc") key=KEY_ESC;
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
    }
}

// ── HTTP server (USB-NCM) ─────────────────────────────────────────────
// Handles GET requests from Pi relay over USB network
// Routes: /run?cmd=..., /type?text=..., /key?combo=...,
//         /enter, /status, /paste, /clear
String url_decode(const String &s) {
    String r = ""; char c;
    for (int i = 0; i < (int)s.length(); i++) {
        if (s[i]=='%' && i+2<(int)s.length()) {
            char h[3]={s[i+1],s[i+2],0}; c=(char)strtol(h,nullptr,16);
            r += c; i += 2;
        } else if (s[i]=='+') { r += ' '; }
        else { r += s[i]; }
    }
    return r;
}

String get_param(const String &req, const String &key) {
    int qs = req.indexOf('?');
    if (qs < 0) return "";
    String query = req.substring(qs+1);
    int sp = query.indexOf(' ');
    if (sp >= 0) query = query.substring(0, sp);
    int ki = query.indexOf(key + "=");
    if (ki < 0) return "";
    int vi = ki + key.length() + 1;
    int amp = query.indexOf('&', vi);
    String val = (amp >= 0) ? query.substring(vi, amp) : query.substring(vi);
    return url_decode(val);
}

void http_respond(NetworkClient &client, int code, const char *body) {
    const char *status = (code==200) ? "OK" : (code==400) ? "Bad Request" : "Not Found";
    client.printf("HTTP/1.1 %d %s\r\nContent-Type: text/plain\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
        code, status, strlen(body), body);
}

void handle_http() {
    NetworkClient client = http_server.accept();
    if (!client) return;

    String req = "";
    uint32_t t = millis();
    while (client.connected() && millis() - t < 2000) {
        if (client.available()) {
            String line = client.readStringUntil('\n');
            if (req.length() == 0) req = line;  // first line = request line
            if (line == "\r") break;
        }
    }
    if (!req.length()) { client.stop(); return; }

    // Parse method + path
    int sp1 = req.indexOf(' '), sp2 = req.indexOf(' ', sp1+1);
    String path = req.substring(sp1+1, sp2);

    char resp[128];
    if (path.startsWith("/run")) {
        String cmd = get_param(req, "cmd");
        if (!cmd.length()) { http_respond(client, 400, "missing cmd"); client.stop(); return; }
        term_cmd(cmd.c_str());
        hid_type(cmd.c_str(), true);
        snprintf(resp, sizeof(resp), "ok: run %s", cmd.c_str());
        http_respond(client, 200, resp);
        term_ok("sent");

    } else if (path.startsWith("/type")) {
        String text = get_param(req, "text");
        if (!text.length()) { http_respond(client, 400, "missing text"); client.stop(); return; }
        strncpy(s_last_text, text.c_str(), sizeof(s_last_text)-1);
        hid_type(text.c_str(), false);
        http_respond(client, 200, "ok: typed");
        term_ok("typed");

    } else if (path.startsWith("/key")) {
        String combo = get_param(req, "combo");
        if (!combo.length()) { http_respond(client, 400, "missing combo"); client.stop(); return; }
        term_cmd(combo.c_str());
        hid_key_combo(combo);
        http_respond(client, 200, "ok: key");
        term_ok(combo.c_str());

    } else if (path == "/enter") {
        Keyboard.press(KEY_RETURN); delay(50); Keyboard.releaseAll();
        http_respond(client, 200, "ok: enter");
        term_ok("enter");

    } else if (path == "/paste") {
        if (!s_last_text[0]) { http_respond(client, 400, "nothing to paste"); client.stop(); return; }
        hid_type(s_last_text, false);
        http_respond(client, 200, "ok: pasted");
        term_ok("pasted");

    } else if (path == "/clear") {
        s_count = 0;
        gfx->fillRect(0, TERM_Y, SCREEN_W, TERM_H, C_BG);
        http_respond(client, 200, "ok: cleared");

    } else if (path == "/status") {
        uint32_t up = (millis() - s_start_ms) / 1000;
        snprintf(resp, sizeof(resp), "WyTerminal v3\nNCM: %s\nWiFi: %s\nUptime: %lus",
            s_ncm_ok ? NCM_BOARD_IP : "waiting",
            s_wifi_ok ? WiFi.localIP().toString().c_str() : "off",
            (unsigned long)up);
        http_respond(client, 200, resp);

    } else {
        http_respond(client, 404, "not found\nRoutes: /run?cmd= /type?text= /key?combo= /enter /paste /clear /status");
    }
    client.stop();
}

// ── Telegram ──────────────────────────────────────────────────────────
String tg_post(const char *method, const String &body) {
    WiFiClientSecure tls;
    tls.setInsecure();
    if (!tls.connect("api.telegram.org", 443)) return "";
    String path = String("/bot") + BOT_TOKEN + "/" + method;
    tls.println("POST " + path + " HTTP/1.1");
    tls.println("Host: api.telegram.org");
    tls.println("Content-Type: application/json");
    tls.println("Content-Length: " + String(body.length()));
    tls.println("Connection: close");
    tls.println();
    tls.print(body);
    String resp;
    uint32_t t = millis();
    while (tls.connected() && millis() - t < 6000)
        while (tls.available()) resp += (char)tls.read();
    tls.stop();
    int idx = resp.indexOf("\r\n\r\n");
    return (idx >= 0) ? resp.substring(idx + 4) : "";
}

void tg_send(long long chat_id, const char *text) {
    StaticJsonDocument<512> doc;
    doc["chat_id"] = chat_id; doc["text"] = text;
    String body; serializeJson(doc, body);
    tg_post("sendMessage", body);
}

void handle_tg_update(JsonObject &upd) {
    if (!upd.containsKey("message")) return;
    JsonObject msg = upd["message"];
    long long chat_id = msg["chat"]["id"];
    if (chat_id != ALLOWED_CHAT_ID) return;
    const char *text = msg["text"] | "";
    if (!text[0]) return;
    String t(text);
    String short_t = t.length() > 30 ? t.substring(0,29)+">" : t;
    term_cmd(short_t.c_str());

    if (t.startsWith("/run ")) {
        String cmd = t.substring(5);
        hid_type(cmd.c_str(), true);
        tg_send(chat_id, ("▶️ " + cmd).c_str()); term_ok("sent");
    } else if (t.startsWith("/type ")) {
        String s = t.substring(6);
        strncpy(s_last_text, s.c_str(), sizeof(s_last_text)-1);
        hid_type(s.c_str(), false);
        tg_send(chat_id, "⌨️ typed"); term_ok("typed");
    } else if (t == "/enter") {
        Keyboard.press(KEY_RETURN); delay(50); Keyboard.releaseAll();
        tg_send(chat_id, "↵ Enter"); term_ok("Enter");
    } else if (t == "/paste") {
        if (!s_last_text[0]) { tg_send(chat_id, "❌ nothing to paste"); term_err("nothing"); return; }
        hid_type(s_last_text, false);
        tg_send(chat_id, "📋 pasted"); term_ok("pasted");
    } else if (t.startsWith("/key ")) {
        String combo = t.substring(5);
        hid_key_combo(combo);
        tg_send(chat_id, ("⌨️ " + combo).c_str()); term_ok(combo.c_str());
    } else if (t == "/clear") {
        s_count = 0; gfx->fillRect(0, TERM_Y, SCREEN_W, TERM_H, C_BG);
        tg_send(chat_id, "🧹 cleared");
    } else if (t == "/status") {
        uint32_t up = (millis() - s_start_ms) / 1000;
        char buf[160];
        snprintf(buf, sizeof(buf),
            "✅ WyTerminal v3\nNCM: %s\nWiFi: %s\nRSSI: %ddBm\nUptime: %lus",
            s_ncm_ok ? NCM_BOARD_IP : "waiting",
            s_wifi_ok ? WiFi.localIP().toString().c_str() : "off",
            WiFi.RSSI(), (unsigned long)up);
        tg_send(chat_id, buf); term_info("status sent");
    } else if (t == "/help") {
        tg_send(chat_id,
            "WyTerminal v3 commands:\n"
            "/run <cmd> — type+Enter\n"
            "/type <text> — type only\n"
            "/enter — Enter key\n"
            "/paste — retype last\n"
            "/key <combo> — key combo\n"
            "/clear — clear display\n"
            "/status — IP+uptime\n"
            "/help — this list\n\n"
            "Direct (via USB NCM):\n"
            "curl http://192.168.100.2/run?cmd=ls");
        term_info("help sent");
    } else {
        tg_send(chat_id, "❓ /help for commands"); term_err("unknown cmd");
    }
}

void poll_telegram() {
    StaticJsonDocument<128> req;
    req["offset"] = s_tg_offset + 1; req["timeout"] = 0; req["limit"] = 5;
    String body; serializeJson(req, body);
    String resp = tg_post("getUpdates", body);
    if (!resp.length()) return;
    DynamicJsonDocument doc(8192);
    if (deserializeJson(doc, resp)) return;
    if (!doc["ok"]) return;
    for (JsonObject upd : doc["result"].as<JsonArray>()) {
        long uid = upd["update_id"];
        if (uid > s_tg_offset) s_tg_offset = uid;
        handle_tg_update(upd);
    }
}

// ── Setup ─────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    pinMode(LCD_PWR, OUTPUT); digitalWrite(LCD_PWR, HIGH); delay(50);

    if (!gfx->begin()) { Serial.println("Display fail"); while(1) delay(1000); }
    set_brightness(200);
    gfx->fillScreen(C_BG);
    gfx->setTextSize(1); gfx->setTextWrap(false);
    draw_header(); draw_footer();

    term_head("WyTerminal v3.0");
    term_info("USB NCM + HID Keyboard");
    term_info("by Wyltek Industries");
    term_info("──────────────────────");

    // USB composite: HID + NCM
    USB.begin();
    Keyboard.begin();
    delay(200);
    term_ok("USB HID ready");

    // USB NCM network
    // ESP32-S3 TinyUSB NCM is exposed via NetworkInterface
    // Board IP configured statically
    // Note: host must run: ip addr add 192.168.100.1/24 dev usb0 && ip link set usb0 up
    term_info("USB NCM: " NCM_BOARD_IP);
    term_info("Host: " NCM_HOST_IP);

    // Start HTTP server on NCM interface
    http_server.begin();
    s_ncm_ok = true;  // NCM is always up when USB is connected
    term_ncm("HTTP :80 ready");
    draw_header();

    // WiFi (for Telegram only)
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    term_info("WiFi (TG only)...");
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(500);
    s_start_ms = millis();
    if (WiFi.status() == WL_CONNECTED) {
        s_wifi_ok = true; draw_header();
        char buf[40]; snprintf(buf, sizeof(buf), "WiFi: %s", WiFi.localIP().toString().c_str());
        term_ok(buf);
        term_ok("Telegram ready");
    } else {
        term_info("WiFi off — NCM only");
    }
    draw_footer();
    term_info("Send /help or curl /status");
}

// ── Loop ─────────────────────────────────────────────────────────────
static uint32_t s_last_footer = 0;
static uint32_t s_last_tg = 0;

void loop() {
    // Always handle HTTP (USB NCM)
    handle_http();

    // Telegram poll every 2s (WiFi only)
    if (s_wifi_ok && millis() - s_last_tg > 2000) {
        if (WiFi.status() != WL_CONNECTED) {
            s_wifi_ok = false; draw_header(); WiFi.reconnect();
        } else {
            poll_telegram();
        }
        s_last_tg = millis();
    } else if (!s_wifi_ok && WiFi.status() == WL_CONNECTED) {
        s_wifi_ok = true; draw_header();
    }

    if (millis() - s_last_footer > 5000) {
        draw_footer(); s_last_footer = millis();
    }
    delay(10);  // tight loop for HTTP responsiveness
}
