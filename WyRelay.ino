/*
 * WyRelay — ESP32-S3 USB HID Keyboard Relay
 *
 * Receives text via Telegram bot, types it on the host machine via USB HID.
 * Plug the NATIVE USB port into the target Pi/PC.
 * Flash via the UART USB port.
 *
 * Board: ESP32-S3 DevKitC-1 (N16R8 or similar, two USB-C ports)
 * Framework: Arduino
 * Requires: USB HID (built-in), WiFiClientSecure, ArduinoJson
 *
 * Setup:
 *   1. Set BOT_TOKEN and CHAT_ID below
 *   2. Flash via UART port
 *   3. Plug NATIVE USB into target machine — appears as USB keyboard
 *   4. Send commands to your bot in Telegram
 *   5. Bot types them + presses Enter on the target machine
 *
 * Commands:
 *   /type <text>   — type text (no Enter)
 *   /run <command> — type command + press Enter
 *   /enter         — press Enter only
 *   /status        — reply with IP + uptime
 *   /paste         — type clipboard contents (last /type text, no Enter)
 *
 * Safety:
 *   Only responds to messages from ALLOWED_CHAT_ID.
 *   All other senders are silently ignored.
 */

#include "USB.h"
#include "USBHIDKeyboard.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

// ── Config ────────────────────────────────────────────────────────────
#define WIFI_SSID       "D-Link the router"
#define WIFI_PASSWORD   "Ajeip853jw5590!"
#define BOT_TOKEN       "8688942400:AAFZKipOJnzroUWAea-zZuhZbLbRTiAluLM"
#define ALLOWED_CHAT_ID  1790655432   // Phill's Telegram user ID — only he can send commands

// ── Telegram API ──────────────────────────────────────────────────────
#define TG_HOST         "api.telegram.org"
#define TG_PORT         443
#define POLL_INTERVAL   1000   // ms between polls

// ── USB HID Keyboard ─────────────────────────────────────────────────
USBHIDKeyboard Keyboard;

// ── State ─────────────────────────────────────────────────────────────
static long     s_last_update_id = 0;
static uint32_t s_start_ms;
static char     s_last_text[512] = "";  /* for /paste */
static const char *TAG = "wyrelay";

// ── Telegram HTTPS helper ─────────────────────────────────────────────
static WiFiClientSecure tls_client;

static String tg_post(const String &method, const String &body)
{
    if (!tls_client.connect(TG_HOST, TG_PORT)) {
        Serial.println("[tg] connect failed");
        return "";
    }
    String path = "/bot" + String(BOT_TOKEN) + "/" + method;
    tls_client.println("POST " + path + " HTTP/1.1");
    tls_client.println("Host: " + String(TG_HOST));
    tls_client.println("Content-Type: application/json");
    tls_client.println("Content-Length: " + String(body.length()));
    tls_client.println("Connection: close");
    tls_client.println();
    tls_client.print(body);

    String response = "";
    uint32_t t = millis();
    while (tls_client.connected() && millis() - t < 5000) {
        while (tls_client.available()) {
            response += (char)tls_client.read();
        }
    }
    tls_client.stop();
    // Strip HTTP headers
    int body_start = response.indexOf("\r\n\r\n");
    if (body_start >= 0) response = response.substring(body_start + 4);
    return response;
}

static void tg_send(long chat_id, const String &text)
{
    StaticJsonDocument<512> doc;
    doc["chat_id"] = chat_id;
    doc["text"] = text;
    String body;
    serializeJson(doc, body);
    tg_post("sendMessage", body);
}

// ── Type text via USB HID ─────────────────────────────────────────────
static void hid_type(const char *text, bool press_enter)
{
    Serial.printf("[hid] typing: %s\n", text);
    Keyboard.print(text);
    if (press_enter) {
        delay(50);
        Keyboard.press(KEY_RETURN);
        delay(50);
        Keyboard.releaseAll();
    }
}

// ── Process a single Telegram update ─────────────────────────────────
static void process_update(JsonObject &update)
{
    if (!update.containsKey("message")) return;
    JsonObject msg = update["message"];

    long chat_id = msg["chat"]["id"];
    if (chat_id != ALLOWED_CHAT_ID) {
        Serial.printf("[sec] blocked message from chat_id %ld\n", chat_id);
        return;
    }

    String text = msg["text"] | "";
    if (text.length() == 0) return;

    Serial.printf("[tg] message: %s\n", text.c_str());

    if (text.startsWith("/run ")) {
        String cmd = text.substring(5);
        tg_send(chat_id, "▶️ Running: " + cmd);
        hid_type(cmd.c_str(), true);

    } else if (text.startsWith("/type ")) {
        String t = text.substring(6);
        strncpy(s_last_text, t.c_str(), sizeof(s_last_text)-1);
        tg_send(chat_id, "⌨️ Typed: " + t);
        hid_type(t.c_str(), false);

    } else if (text == "/enter") {
        Keyboard.press(KEY_RETURN);
        delay(50);
        Keyboard.releaseAll();
        tg_send(chat_id, "↵ Enter pressed");

    } else if (text == "/paste") {
        if (strlen(s_last_text) == 0) {
            tg_send(chat_id, "❌ Nothing to paste (use /type first)");
        } else {
            tg_send(chat_id, "📋 Pasting: " + String(s_last_text));
            hid_type(s_last_text, false);
        }

    } else if (text == "/status") {
        uint32_t up = (millis() - s_start_ms) / 1000;
        String status = "✅ WyRelay online\n";
        status += "IP: " + WiFi.localIP().toString() + "\n";
        status += "Uptime: " + String(up) + "s\n";
        status += "Signal: " + String(WiFi.RSSI()) + " dBm";
        tg_send(chat_id, status);

    } else if (text.startsWith("/key ")) {
        String combo = text.substring(5);
        combo.toLowerCase();
        bool ctrl  = combo.indexOf("ctrl")  >= 0;
        bool alt   = combo.indexOf("alt")   >= 0;
        bool shift = combo.indexOf("shift") >= 0;
        bool super_ = combo.indexOf("super") >= 0 || combo.indexOf("win") >= 0 || combo.indexOf("cmd") >= 0;
        // Find the actual key (last token after +)
        int last_plus = combo.lastIndexOf("+");
        String key_str = (last_plus >= 0) ? combo.substring(last_plus + 1) : combo;
        key_str.trim();
        uint8_t key = 0;
        if      (key_str == "t")      key = 't';
        else if (key_str == "c")      key = 'c';
        else if (key_str == "v")      key = 'v';
        else if (key_str == "z")      key = 'z';
        else if (key_str == "a")      key = 'a';
        else if (key_str == "x")      key = 'x';
        else if (key_str == "f4")     key = KEY_F4;
        else if (key_str == "f5")     key = KEY_F5;
        else if (key_str == "tab")    key = KEY_TAB;
        else if (key_str == "esc")    key = KEY_ESC;
        else if (key_str == "space")  key = ' ';
        else if (key_str == "up")     key = KEY_UP_ARROW;
        else if (key_str == "down")   key = KEY_DOWN_ARROW;
        else if (key_str == "left")   key = KEY_LEFT_ARROW;
        else if (key_str == "right")  key = KEY_RIGHT_ARROW;
        else if (key_str.length()==1) key = key_str[0];
        if (key) {
            if (ctrl)   Keyboard.press(KEY_LEFT_CTRL);
            if (alt)    Keyboard.press(KEY_LEFT_ALT);
            if (shift)  Keyboard.press(KEY_LEFT_SHIFT);
            if (super_) Keyboard.press(KEY_LEFT_GUI);
            Keyboard.press(key);
            delay(100);
            Keyboard.releaseAll();
            tg_send(chat_id, "⌨️ Key sent: " + combo);
        } else {
            tg_send(chat_id, "❓ Unknown key: " + key_str);
        }

    } else if (text == "/help") {
        tg_send(chat_id,
            "WyRelay commands:\n"
            "/run <cmd> — type + Enter\n"
            "/type <text> — type only\n"
            "/enter — press Enter\n"
            "/paste — retype last /type\n"
            "/key <combo> — send key combo\n"
            "  e.g. /key ctrl+alt+t\n"
            "  e.g. /key ctrl+c\n"
            "  e.g. /key super\n"
            "/status — IP + uptime\n"
            "/help — this message"
        );

    } else {
        tg_send(chat_id, "❓ Unknown command. Try /help");
    }
}

// ── Poll Telegram for updates ─────────────────────────────────────────
static void poll_telegram()
{
    StaticJsonDocument<64> req;
    req["offset"]  = s_last_update_id + 1;
    req["timeout"] = 0;
    req["limit"]   = 5;
    String body;
    serializeJson(req, body);

    String resp = tg_post("getUpdates", body);
    if (resp.length() == 0) return;

    DynamicJsonDocument doc(8192);
    if (deserializeJson(doc, resp)) {
        Serial.println("[tg] JSON parse error");
        return;
    }
    if (!doc["ok"]) return;

    JsonArray results = doc["result"];
    for (JsonObject update : results) {
        long uid = update["update_id"];
        if (uid > s_last_update_id) s_last_update_id = uid;
        process_update(update);
    }
}

// ── Setup ─────────────────────────────────────────────────────────────
void setup()
{
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n=== WyRelay v1.0 ===");

    /* USB HID keyboard init */
    USB.begin();
    Keyboard.begin();
    delay(1000);
    Serial.println("[hid] USB keyboard ready");

    /* WiFi */
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("[wifi] connecting");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500); Serial.print(".");
    }
    Serial.println("\n[wifi] connected: " + WiFi.localIP().toString());

    /* TLS — skip cert verify for simplicity */
    tls_client.setInsecure();

    s_start_ms = millis();
    Serial.printf("[tg] polling bot, allowed chat: %d\n", ALLOWED_CHAT_ID);
}

// ── Loop ──────────────────────────────────────────────────────────────
void loop()
{
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[wifi] reconnecting...");
        WiFi.reconnect();
        delay(5000);
        return;
    }

    poll_telegram();
    delay(POLL_INTERVAL);
}
