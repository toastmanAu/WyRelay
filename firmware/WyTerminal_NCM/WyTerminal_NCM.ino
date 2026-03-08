/*
 * WyTerminal v4 — LilyGO T-Display S3 AMOLED
 * USB HID Keyboard + WiFi Telegram + Onboard SSH
 *
 * v4: /shell <cmd> executes on any SSH target directly from the board.
 *     No Pi relay needed. Board SSHes to target over WiFi.
 *     /target user@host[:port] — switch SSH target
 *     /ssh_pass <password>     — set SSH password for current target
 *     /shell <cmd>             — run command on target, reply via Telegram
 *
 * Board: LilyGo T-Display-S3, USB-OTG TinyUSB, CDC On Boot: Enabled
 * FQBN:  esp32:esp32:lilygo_t_display_s3:USBMode=default,CDCOnBoot=cdc
 */

#include "USB.h"
#include "USBHIDKeyboard.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>

// LibSSH-ESP32
#include "libssh_esp32.h"
#include <libssh/libssh.h>

// ─── Config ──────────────────────────────────────────────────────────────────
#define WIFI_SSID        "D-Link the router"
#define WIFI_PASSWORD    "Ajeip853jw5590!"
#define BOT_TOKEN        "8688942400:AAFZKipOJnzroUWAea-zZuhZbLbRTiAluLM"
#define ALLOWED_CHAT_ID  1790655432LL

// Default SSH target — override with /target
#define DEFAULT_SSH_USER "orangepi"
#define DEFAULT_SSH_HOST "192.168.68.87"
#define DEFAULT_SSH_PORT 22
#define DEFAULT_SSH_PASS ""   // blank = try publickey then ask

// ─── Display pins ─────────────────────────────────────────────────────────────
#define LCD_CS  6
#define LCD_SCK 47
#define LCD_D0  18
#define LCD_D1   7
#define LCD_D2  48
#define LCD_D3   5
#define LCD_RST 17
#define LCD_PWR 38

// ─── Colours ──────────────────────────────────────────────────────────────────
#define C_BG      0x0000
#define C_GREEN   0x07E0
#define C_RED     0xF800
#define C_YELLOW  0xFFE0
#define C_CYAN    0x07FF
#define C_GREY    0x4208
#define C_DKGREEN 0x0200
#define C_DKBLUE  0x000A
#define C_PURPLE  0x780F
#define C_ORANGE  0xFD20

// ─── Display layout ───────────────────────────────────────────────────────────
#define SCREEN_W   240
#define SCREEN_H   536
#define HEADER_H    28
#define FOOTER_H    22
#define FONT_H      12
#define TERM_Y     (HEADER_H + 4)
#define TERM_H     (SCREEN_H - HEADER_H - FOOTER_H - 8)
#define TERM_LINES (TERM_H / FONT_H)
#define TERM_COLS   40

Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS,LCD_SCK,LCD_D0,LCD_D1,LCD_D2,LCD_D3);
Arduino_GFX    *gfx = new Arduino_RM67162(bus, LCD_RST, 0);
USBHIDKeyboard  Keyboard;

// ─── State ────────────────────────────────────────────────────────────────────
static long     s_tg_offset = 0;
static uint32_t s_start_ms;
static char     s_last_text[512] = "";
static bool     s_wifi_ok = false;

// SSH target config
static char s_ssh_host[64]  = DEFAULT_SSH_HOST;
static char s_ssh_user[64]  = DEFAULT_SSH_USER;
static char s_ssh_pass[128] = DEFAULT_SSH_PASS;
static int  s_ssh_port      = DEFAULT_SSH_PORT;

// SSH async job
struct SshJob {
    char cmd[512];
    long long chat_id;
    volatile bool pending;
    volatile bool done;
    char result[2048];
};
static SshJob s_ssh_job = {0};
static SemaphoreHandle_t s_ssh_mutex;

// ─── Terminal buffer ──────────────────────────────────────────────────────────
struct Line { char text[TERM_COLS+1]; uint16_t col; };
static Line s_buf[TERM_LINES];
static int  s_count = 0;
static SemaphoreHandle_t s_disp_mutex;

void set_brightness(uint8_t v){ bus->beginWrite(); bus->writeC8D8(0x51,v); bus->endWrite(); }

void term_push(const char *text, uint16_t col){
    if(xSemaphoreTake(s_disp_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    if(s_count < TERM_LINES){
        strncpy(s_buf[s_count].text, text, TERM_COLS);
        s_buf[s_count].text[TERM_COLS] = 0;
        s_buf[s_count++].col = col;
    } else {
        memmove(s_buf, s_buf+1, sizeof(Line)*(TERM_LINES-1));
        strncpy(s_buf[TERM_LINES-1].text, text, TERM_COLS);
        s_buf[TERM_LINES-1].text[TERM_COLS] = 0;
        s_buf[TERM_LINES-1].col = col;
    }
    gfx->fillRect(0, TERM_Y, SCREEN_W, TERM_H, C_BG);
    for(int i=0; i<s_count; i++){
        gfx->setTextColor(s_buf[i].col);
        gfx->setCursor(2, TERM_Y + i*FONT_H);
        gfx->print(s_buf[i].text);
    }
    xSemaphoreGive(s_disp_mutex);
}

void term_cmd(const char*s) { char b[42]; snprintf(b,42,"> %.39s",s); term_push(b,C_CYAN);   }
void term_ok(const char*s)  { char b[42]; snprintf(b,42,"+ %.39s",s); term_push(b,C_GREEN);  }
void term_err(const char*s) { char b[42]; snprintf(b,42,"! %.39s",s); term_push(b,C_RED);    }
void term_info(const char*s){ char b[42]; snprintf(b,42,"  %.39s",s); term_push(b,C_GREY);   }
void term_head(const char*s){ char b[42]; snprintf(b,42,"%.41s",s);   term_push(b,C_YELLOW); }
void term_sys(const char*s) { char b[42]; snprintf(b,42,"* %.39s",s); term_push(b,C_PURPLE); }
void term_ssh(const char*s) { char b[42]; snprintf(b,42,"$ %.39s",s); term_push(b,C_ORANGE); }

// ─── Header / Footer ──────────────────────────────────────────────────────────
void draw_header(){
    if(xSemaphoreTake(s_disp_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    gfx->fillRect(0,0,SCREEN_W,HEADER_H,C_DKGREEN);
    gfx->setTextColor(C_GREEN); gfx->setTextSize(1);
    gfx->setCursor(4,9); gfx->print("WyTerminal v4");
    gfx->setCursor(SCREEN_W-52,9);
    gfx->setTextColor(s_wifi_ok?C_GREEN:C_GREY);
    gfx->print(s_wifi_ok?"WiFi":"wifi");
    gfx->setTextColor(C_ORANGE); gfx->print(" SSH");
    gfx->setTextColor(C_PURPLE); gfx->print(" CDC");
    xSemaphoreGive(s_disp_mutex);
}

void draw_footer(){
    if(xSemaphoreTake(s_disp_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    int y=SCREEN_H-FOOTER_H;
    gfx->fillRect(0,y,SCREEN_W,FOOTER_H,C_DKBLUE);
    gfx->setTextColor(C_GREY); gfx->setTextSize(1); gfx->setCursor(2,y+5);
    uint32_t up=(millis()-s_start_ms)/1000; char buf[48];
    snprintf(buf,48,"%s@%s  up:%lus",
        s_ssh_user, s_ssh_host, (unsigned long)up);
    gfx->print(buf);
    xSemaphoreGive(s_disp_mutex);
}

// ─── HID ──────────────────────────────────────────────────────────────────────
void hid_type(const char*text, bool enter){
    Keyboard.print(text);
    if(enter){ delay(50); Keyboard.press(KEY_RETURN); delay(50); Keyboard.releaseAll(); }
}

void hid_key(const String&combo){
    String c=combo; c.toLowerCase();
    bool ctrl=c.indexOf("ctrl")>=0, alt=c.indexOf("alt")>=0;
    bool shift=c.indexOf("shift")>=0, sup=c.indexOf("super")>=0||c.indexOf("win")>=0;
    int lp=c.lastIndexOf("+"); String ks=(lp>=0)?c.substring(lp+1):c; ks.trim();
    uint8_t key=0;
    if(ks=="t")key='t'; else if(ks=="c")key='c'; else if(ks=="v")key='v';
    else if(ks=="z")key='z'; else if(ks=="a")key='a'; else if(ks=="x")key='x';
    else if(ks=="f4")key=KEY_F4; else if(ks=="f5")key=KEY_F5;
    else if(ks=="tab")key=KEY_TAB; else if(ks=="esc")key=KEY_ESC;
    else if(ks=="space")key=' '; else if(ks=="up")key=KEY_UP_ARROW;
    else if(ks=="down")key=KEY_DOWN_ARROW; else if(ks=="left")key=KEY_LEFT_ARROW;
    else if(ks=="right")key=KEY_RIGHT_ARROW;
    else if(ks.length()==1)key=(uint8_t)ks[0];
    if(key){
        if(ctrl)Keyboard.press(KEY_LEFT_CTRL); if(alt)Keyboard.press(KEY_LEFT_ALT);
        if(shift)Keyboard.press(KEY_LEFT_SHIFT); if(sup)Keyboard.press(KEY_LEFT_GUI);
        Keyboard.press(key); delay(100); Keyboard.releaseAll();
    }
}

// ─── SSH execution (runs on ssh_task, not main loop) ─────────────────────────
// Returns output string (truncated to 2000 chars). Called from ssh_task only.
static String ssh_exec(const char *cmd){
    // Init libssh
    libssh_begin();
    ssh_session session = ssh_new();
    if(!session) return "err:ssh_new failed";

    int port = s_ssh_port;
    int timeout = 15;
    int verbosity = SSH_LOG_NOLOG;
    ssh_options_set(session, SSH_OPTIONS_HOST,            s_ssh_host);
    ssh_options_set(session, SSH_OPTIONS_USER,            s_ssh_user);
    ssh_options_set(session, SSH_OPTIONS_PORT,            &port);
    ssh_options_set(session, SSH_OPTIONS_TIMEOUT,         &timeout);
    ssh_options_set(session, SSH_OPTIONS_LOG_VERBOSITY,   &verbosity);
    // Disable strict host key checking (auto-accept new hosts)
    ssh_options_set(session, SSH_OPTIONS_KNOWNHOSTS,      "/dev/null");

    int rc = ssh_connect(session);
    if(rc != SSH_OK){
        String e = String("err:connect ") + ssh_get_error(session);
        ssh_free(session);
        return e;
    }

    // Auto-accept host key (TOFU — Trust On First Use)
    ssh_session_update_known_hosts(session);

    // Auth: try password if set, else try auto publickey
    if(s_ssh_pass[0]){
        rc = ssh_userauth_password(session, NULL, s_ssh_pass);
    } else {
        rc = ssh_userauth_publickey_auto(session, NULL, NULL);
    }
    if(rc != SSH_AUTH_SUCCESS){
        // Try password with empty string as last resort
        rc = ssh_userauth_password(session, NULL, "");
        if(rc != SSH_AUTH_SUCCESS){
            String e = String("err:auth ") + ssh_get_error(session);
            ssh_disconnect(session);
            ssh_free(session);
            return e;
        }
    }

    // Open channel and exec
    ssh_channel channel = ssh_channel_new(session);
    if(!channel){
        ssh_disconnect(session); ssh_free(session);
        return "err:channel_new";
    }
    rc = ssh_channel_open_session(channel);
    if(rc != SSH_OK){
        ssh_channel_free(channel); ssh_disconnect(session); ssh_free(session);
        return "err:channel_open";
    }
    rc = ssh_channel_request_exec(channel, cmd);
    if(rc != SSH_OK){
        ssh_channel_close(channel); ssh_channel_free(channel);
        ssh_disconnect(session); ssh_free(session);
        return "err:exec";
    }

    // Read output (stdout + stderr)
    char buf[256];
    String output = "";
    int nbytes;
    while((nbytes = ssh_channel_read_timeout(channel, buf, sizeof(buf)-1, 0, 10000)) > 0){
        buf[nbytes] = 0;
        output += buf;
        if(output.length() > 2000){ output += "\n[truncated]"; break; }
    }
    // Also read stderr
    while((nbytes = ssh_channel_read_timeout(channel, buf, sizeof(buf)-1, 1, 1000)) > 0){
        buf[nbytes] = 0;
        output += buf;
        if(output.length() > 2000){ output += "\n[truncated]"; break; }
    }

    ssh_channel_send_eof(channel);
    ssh_channel_close(channel);
    ssh_channel_free(channel);
    ssh_disconnect(session);
    ssh_free(session);

    if(output.length() == 0) output = "(no output)";
    return output;
}

// ─── SSH FreeRTOS task ────────────────────────────────────────────────────────
// Runs at high priority, waits for jobs, executes SSH, stores result
void ssh_task(void *pv){
    while(1){
        if(s_ssh_job.pending && !s_ssh_job.done){
            term_ssh(s_ssh_job.cmd);
            String out = ssh_exec(s_ssh_job.cmd);
            // Store result
            if(xSemaphoreTake(s_ssh_mutex, portMAX_DELAY) == pdTRUE){
                strncpy(s_ssh_job.result, out.c_str(), sizeof(s_ssh_job.result)-1);
                s_ssh_job.result[sizeof(s_ssh_job.result)-1] = 0;
                s_ssh_job.done = true;
                s_ssh_job.pending = false;
                xSemaphoreGive(s_ssh_mutex);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ─── Telegram helpers ─────────────────────────────────────────────────────────
String tg_post(const char*method, const String&body){
    WiFiClientSecure tls; tls.setInsecure();
    if(!tls.connect("api.telegram.org",443)) return "";
    String path = String("/bot") + BOT_TOKEN + "/" + method;
    tls.println("POST "+path+" HTTP/1.1");
    tls.println("Host: api.telegram.org");
    tls.println("Content-Type: application/json");
    tls.println("Content-Length: "+String(body.length()));
    tls.println("Connection: close"); tls.println();
    tls.print(body);
    String resp; uint32_t t=millis();
    while(tls.connected()&&millis()-t<8000) while(tls.available()) resp+=(char)tls.read();
    tls.stop();
    int idx=resp.indexOf("\r\n\r\n");
    return idx>=0 ? resp.substring(idx+4) : "";
}

void tg_send(long long chat_id, const char*text){
    // Split into ≤4096 char chunks if needed
    String full = String(text);
    int len = full.length();
    int pos = 0;
    while(pos < len){
        String chunk = full.substring(pos, min(pos+4000, len));
        StaticJsonDocument<512> doc;
        doc["chat_id"] = chat_id;
        doc["text"] = chunk;
        String body; serializeJson(doc, body);
        tg_post("sendMessage", body);
        pos += 4000;
        if(pos < len) delay(200);
    }
}

// ─── SSH job dispatcher (called from main loop to check completed jobs) ───────
static long long s_pending_chat_id = 0;
static bool      s_ssh_reply_pending = false;

void check_ssh_result(){
    if(!s_ssh_reply_pending) return;
    bool done = false;
    char result[2048] = {0};
    if(xSemaphoreTake(s_ssh_mutex, pdMS_TO_TICKS(50)) == pdTRUE){
        if(s_ssh_job.done){
            done = true;
            strncpy(result, s_ssh_job.result, sizeof(result)-1);
            s_ssh_job.done = false;
        }
        xSemaphoreGive(s_ssh_mutex);
    }
    if(done){
        s_ssh_reply_pending = false;
        term_ok("ssh done");
        tg_send(s_pending_chat_id, result);
    }
}

void dispatch_ssh(const char *cmd, long long chat_id){
    if(s_ssh_job.pending || s_ssh_reply_pending){
        tg_send(chat_id, "err:ssh busy — try again");
        return;
    }
    strncpy(s_ssh_job.cmd, cmd, sizeof(s_ssh_job.cmd)-1);
    s_ssh_job.cmd[sizeof(s_ssh_job.cmd)-1] = 0;
    s_ssh_job.chat_id = chat_id;
    s_ssh_job.done = false;
    s_ssh_job.pending = true;
    s_pending_chat_id = chat_id;
    s_ssh_reply_pending = true;
    char disp[40]; snprintf(disp,40,"ssh: %.36s",cmd);
    term_ssh(disp);
    tg_send(chat_id, "⏳ running...");
}

// ─── Command handler ──────────────────────────────────────────────────────────
// Returns response string. Async commands (SSH) return immediately; result sent later.
String handle_cmd(const String&t, bool send_tg, long long chat_id){
    String short_t = t.length()>30 ? t.substring(0,29)+">" : t;
    term_cmd(short_t.c_str());

    // HID commands
    if(t.startsWith("/run ")){
        String cmd=t.substring(5); hid_type(cmd.c_str(),true);
        if(send_tg){ String m="▶️ "+cmd; tg_send(chat_id,m.c_str()); }
        term_ok("sent"); return "ok:run";
    }
    if(t.startsWith("/type ")){
        String s=t.substring(6);
        strncpy(s_last_text,s.c_str(),sizeof(s_last_text)-1);
        hid_type(s.c_str(),false); term_ok("typed"); return "ok:typed";
    }
    if(t=="/enter"){
        Keyboard.press(KEY_RETURN); delay(50); Keyboard.releaseAll();
        term_ok("enter"); return "ok:enter";
    }
    if(t=="/paste"){
        if(!s_last_text[0]){ term_err("nothing"); return "err:nothing"; }
        hid_type(s_last_text,false); term_ok("pasted"); return "ok:pasted";
    }
    if(t.startsWith("/key ")){
        String combo=t.substring(5); hid_key(combo); term_ok(combo.c_str()); return "ok:key";
    }
    if(t=="/clear"){
        s_count=0; gfx->fillRect(0,TERM_Y,SCREEN_W,TERM_H,C_BG); return "ok:clear";
    }

    // SSH commands
    if(t.startsWith("/shell ")){
        if(!s_wifi_ok){ term_err("no wifi"); return "err:no wifi"; }
        String cmd=t.substring(7); cmd.trim();
        dispatch_ssh(cmd.c_str(), chat_id);
        return "";  // response sent async
    }
    if(t.startsWith("/target ")){
        // /target user@host[:port]
        String arg=t.substring(8); arg.trim();
        int at=arg.indexOf('@');
        if(at<0){ term_err("fmt: user@host"); return "err:format — use user@host"; }
        String user=arg.substring(0,at);
        String hostport=arg.substring(at+1);
        int colon=hostport.indexOf(':');
        String host=colon>=0?hostport.substring(0,colon):hostport;
        int port=colon>=0?hostport.substring(colon+1).toInt():22;
        strncpy(s_ssh_user, user.c_str(), sizeof(s_ssh_user)-1);
        strncpy(s_ssh_host, host.c_str(), sizeof(s_ssh_host)-1);
        s_ssh_port=port; s_ssh_pass[0]=0;
        draw_footer();
        String resp="🎯 target: "+user+"@"+host+":"+String(port);
        term_ok(resp.c_str());
        return resp;
    }
    if(t.startsWith("/ssh_pass ")){
        String pass=t.substring(10);
        strncpy(s_ssh_pass, pass.c_str(), sizeof(s_ssh_pass)-1);
        term_ok("pass set");
        return "ok:password stored";
    }

    // Info
    if(t=="/status"){
        uint32_t up=(millis()-s_start_ms)/1000; char buf[256];
        snprintf(buf,256,
            "WyTerminal v4\n"
            "WiFi: %s\n"
            "Target: %s@%s:%d\n"
            "Auth: %s\n"
            "Uptime: %lus",
            s_wifi_ok?WiFi.localIP().toString().c_str():"off",
            s_ssh_user, s_ssh_host, s_ssh_port,
            s_ssh_pass[0]?"password":"publickey/none",
            (unsigned long)up);
        term_info("status"); return String(buf);
    }
    if(t=="/help"){
        return
            "WyTerminal v4 commands:\n"
            "/run <text>         — HID type+enter\n"
            "/type <text>        — HID type (no enter)\n"
            "/enter              — press Enter\n"
            "/paste              — retype last /type text\n"
            "/key <combo>        — e.g. ctrl+c, ctrl+alt+t\n"
            "/clear              — clear display\n"
            "/shell <cmd>        — SSH to target, run cmd\n"
            "/target user@host   — switch SSH target\n"
            "/ssh_pass <pass>    — set SSH password\n"
            "/status             — show status\n"
            "/help               — this message";
    }

    term_err("unknown"); return "err:unknown";
}

// ─── Telegram poller ──────────────────────────────────────────────────────────
void poll_telegram(){
    StaticJsonDocument<128> req;
    req["offset"]=s_tg_offset+1; req["timeout"]=0; req["limit"]=5;
    String body; serializeJson(req,body);
    String resp=tg_post("getUpdates",body);
    if(!resp.length()) return;
    DynamicJsonDocument doc(8192);
    if(deserializeJson(doc,resp)) return;
    if(!doc["ok"]) return;
    for(JsonObject upd : doc["result"].as<JsonArray>()){
        long uid=upd["update_id"]; if(uid>s_tg_offset) s_tg_offset=uid;
        if(!upd.containsKey("message")) continue;
        JsonObject msg=upd["message"];
        long long cid=msg["chat"]["id"]; if(cid!=ALLOWED_CHAT_ID) continue;
        const char*text=msg["text"]|""; if(!text[0]) continue;
        // Strip @BotUsername suffix from commands
        String tcmd=String(text);
        int at_idx=tcmd.indexOf('@'); if(at_idx>0) tcmd=tcmd.substring(0,at_idx);
        String r=handle_cmd(tcmd,true,cid);
        if(r.length()) tg_send(cid, r.c_str());
    }
}

// ─── CDC serial ───────────────────────────────────────────────────────────────
static String s_cdc_buf="";
void handle_cdc(){
    while(Serial.available()){
        char c=Serial.read();
        if(c=='\n'||c=='\r'){
            s_cdc_buf.trim();
            if(s_cdc_buf.length()){
                term_sys(s_cdc_buf.c_str());
                String resp=handle_cmd(s_cdc_buf,false,0);
                if(resp.length()) Serial.println(resp);
            }
            s_cdc_buf="";
        } else { s_cdc_buf+=c; }
    }
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup(){
    Serial.begin(115200);

    // Display
    pinMode(LCD_PWR,OUTPUT); digitalWrite(LCD_PWR,HIGH); delay(50);
    if(!gfx->begin()){ Serial.println("Display fail"); while(1) delay(1000); }
    set_brightness(200);
    gfx->fillScreen(C_BG);
    gfx->setTextSize(1);
    gfx->setTextWrap(false);

    // Mutexes
    s_disp_mutex = xSemaphoreCreateMutex();
    s_ssh_mutex  = xSemaphoreCreateMutex();

    draw_header(); draw_footer();
    term_head("WyTerminal v4.0");
    term_sys("Onboard SSH — no relay");
    term_info("by Wyltek Industries");
    term_info("────────────────────");

    // USB HID + CDC
    USB.begin(); Keyboard.begin(); delay(200);
    term_ok("USB HID + CDC ready");

    // WiFi
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    term_info("WiFi connecting...");
    uint32_t t0=millis();
    while(WiFi.status()!=WL_CONNECTED && millis()-t0<20000) delay(500);
    s_start_ms=millis();
    if(WiFi.status()==WL_CONNECTED){
        s_wifi_ok=true; draw_header();
        char buf[48]; snprintf(buf,48,"%s",WiFi.localIP().toString().c_str());
        term_ok(buf);
        // Init libssh (must happen after WiFi)
        libssh_begin();
        term_ok("SSH engine ready");
        char tgt[80]; snprintf(tgt,80,"tgt: %s@%s",s_ssh_user,s_ssh_host);
        term_ssh(tgt);
        term_ok("Telegram ready");
    } else {
        term_info("WiFi off — CDC+HID only");
    }
    draw_footer();

    // SSH FreeRTOS task (high stack — libssh needs ~32KB)
    xTaskCreatePinnedToCore(ssh_task, "ssh", 32768, NULL,
        (tskIDLE_PRIORITY + 2), NULL, 0);
}

// ─── Loop ─────────────────────────────────────────────────────────────────────
static uint32_t s_last_footer=0, s_last_tg=0;

void loop(){
    handle_cdc();

    if(s_wifi_ok){
        // Check for completed SSH jobs
        check_ssh_result();

        // Poll Telegram every 2s
        if(millis()-s_last_tg > 2000){
            if(WiFi.status()!=WL_CONNECTED){
                s_wifi_ok=false; draw_header(); WiFi.reconnect();
            } else {
                poll_telegram();
            }
            s_last_tg=millis();
        }
    } else if(WiFi.status()==WL_CONNECTED){
        s_wifi_ok=true; draw_header();
    }

    if(millis()-s_last_footer > 5000){ draw_footer(); s_last_footer=millis(); }
    delay(50);
}