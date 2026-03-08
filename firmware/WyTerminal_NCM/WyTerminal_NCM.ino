/*
 * WyTerminal v3 — LilyGO T-Display S3 AMOLED
 * USB HID Keyboard + USB CDC Serial (command channel)
 *
 * v3: CDC serial replaces WiFi relay — Pi sends commands directly
 * via /dev/ttyACM0, no WiFi needed for HID control.
 * WiFi + Telegram still work as before.
 *
 * Board: LilyGo T-Display-S3, USB-OTG TinyUSB, CDC On Boot: Enabled
 */

#include "USB.h"
#include "USBHIDKeyboard.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>

#define WIFI_SSID        "D-Link the router"
#define WIFI_PASSWORD    "Ajeip853jw5590!"
#define BOT_TOKEN        "8688942400:AAFZKipOJnzroUWAea-zZuhZbLbRTiAluLM"
#define ALLOWED_CHAT_ID  1790655432LL

#define LCD_CS  6
#define LCD_SCK 47
#define LCD_D0  18
#define LCD_D1   7
#define LCD_D2  48
#define LCD_D3   5
#define LCD_RST 17
#define LCD_PWR 38

#define C_BG      0x0000
#define C_GREEN   0x07E0
#define C_RED     0xF800
#define C_YELLOW  0xFFE0
#define C_CYAN    0x07FF
#define C_GREY    0x4208
#define C_DKGREEN 0x0200
#define C_DKBLUE  0x000A
#define C_PURPLE  0x780F

#define SCREEN_W  240
#define SCREEN_H  536
#define HEADER_H   28
#define FOOTER_H   22
#define FONT_H     12
#define TERM_Y    (HEADER_H + 4)
#define TERM_H    (SCREEN_H - HEADER_H - FOOTER_H - 8)
#define TERM_LINES (TERM_H / FONT_H)
#define TERM_COLS  40

Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS,LCD_SCK,LCD_D0,LCD_D1,LCD_D2,LCD_D3);
Arduino_GFX    *gfx = new Arduino_RM67162(bus, LCD_RST, 1);
USBHIDKeyboard  Keyboard;


static long     s_tg_offset = 0;
static uint32_t s_start_ms;
static char     s_last_text[512] = "";
static bool     s_wifi_ok = false;

struct Line { char text[TERM_COLS+1]; uint16_t col; };
static Line s_buf[TERM_LINES];
static int  s_count = 0;

void set_brightness(uint8_t v){ bus->beginWrite(); bus->writeC8D8(0x51,v); bus->endWrite(); }

void term_push(const char *text, uint16_t col){
    if(s_count < TERM_LINES){ strncpy(s_buf[s_count].text,text,TERM_COLS); s_buf[s_count++].col=col; }
    else{ memmove(s_buf,s_buf+1,sizeof(Line)*(TERM_LINES-1)); strncpy(s_buf[TERM_LINES-1].text,text,TERM_COLS); s_buf[TERM_LINES-1].col=col; }
    gfx->fillRect(0,TERM_Y,SCREEN_W,TERM_H,C_BG);
    for(int i=0;i<s_count;i++){ gfx->setTextColor(s_buf[i].col); gfx->setCursor(2,TERM_Y+i*FONT_H); gfx->print(s_buf[i].text); }
}
void term_cmd(const char*s){char b[42];snprintf(b,42,"> %.39s",s);term_push(b,C_CYAN);}
void term_ok(const char*s) {char b[42];snprintf(b,42,"+ %.39s",s);term_push(b,C_GREEN);}
void term_err(const char*s){char b[42];snprintf(b,42,"! %.39s",s);term_push(b,C_RED);}
void term_info(const char*s){char b[42];snprintf(b,42,"  %.39s",s);term_push(b,C_GREY);}
void term_head(const char*s){char b[42];snprintf(b,42,"%.41s",s);term_push(b,C_YELLOW);}
void term_sys(const char*s) {char b[42];snprintf(b,42,"* %.39s",s);term_push(b,C_PURPLE);}

void draw_header(){
    gfx->fillRect(0,0,SCREEN_W,HEADER_H,C_DKGREEN);
    gfx->setTextColor(C_GREEN); gfx->setTextSize(1);
    gfx->setCursor(4,9); gfx->print("WyTerminal v3");
    gfx->setCursor(SCREEN_W-34,9);
    gfx->setTextColor(s_wifi_ok?C_GREEN:C_GREY);
    gfx->print(s_wifi_ok?"WiFi":"wifi");
    gfx->setTextColor(C_PURPLE); gfx->print(" CDC");
}
void draw_footer(){
    int y=SCREEN_H-FOOTER_H;
    gfx->fillRect(0,y,SCREEN_W,FOOTER_H,C_DKBLUE);
    gfx->setTextColor(C_GREY); gfx->setTextSize(1); gfx->setCursor(2,y+5);
    uint32_t up=(millis()-s_start_ms)/1000; char buf[44];
    if(s_wifi_ok) snprintf(buf,44,"%s  up:%lus",WiFi.localIP().toString().c_str(),(unsigned long)up);
    else snprintf(buf,44,"CDC ready  up:%lus",(unsigned long)up);
    gfx->print(buf);
}

void hid_type(const char*text,bool enter){
    Keyboard.print(text);
    if(enter){delay(50);Keyboard.press(KEY_RETURN);delay(50);Keyboard.releaseAll();}
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

// Process a command (from CDC serial or Telegram)
// Returns response string
String handle_cmd(const String&t, bool send_tg, long long chat_id){
    String short_t=t.length()>30?t.substring(0,29)+">":t;
    term_cmd(short_t.c_str());
    if(t.startsWith("/run ")){
        String cmd=t.substring(5); hid_type(cmd.c_str(),true);
        if(send_tg){String m="▶️ "+cmd; tg_noop(chat_id,m.c_str());} term_ok("sent"); return "ok:run";
    } else if(t.startsWith("/type ")){
        String s=t.substring(6); strncpy(s_last_text,s.c_str(),sizeof(s_last_text)-1);
        hid_type(s.c_str(),false); term_ok("typed"); return "ok:typed";
    } else if(t=="/enter"){
        Keyboard.press(KEY_RETURN);delay(50);Keyboard.releaseAll(); term_ok("enter"); return "ok:enter";
    } else if(t=="/paste"){
        if(!s_last_text[0]){term_err("nothing");return "err:nothing";}
        hid_type(s_last_text,false); term_ok("pasted"); return "ok:pasted";
    } else if(t.startsWith("/key ")){
        String combo=t.substring(5); hid_key(combo); term_ok(combo.c_str()); return "ok:key";
    } else if(t=="/clear"){
        s_count=0; gfx->fillRect(0,TERM_Y,SCREEN_W,TERM_H,C_BG); return "ok:clear";
    } else if(t=="/status"){
        uint32_t up=(millis()-s_start_ms)/1000; char buf[160];
        snprintf(buf,160,"WyTerminal v3\nCDC: ready\nWiFi: %s\nUptime: %lus",
            s_wifi_ok?WiFi.localIP().toString().c_str():"off",(unsigned long)up);
        term_info("status"); return String(buf);
    } else if(t=="/help"){
        return "/run /type /enter /paste /key /clear /status\nCDC: echo cmd to /dev/ttyACM0";
    }
    term_err("unknown"); return "err:unknown";
}

// Telegram stubs (defined after handle_cmd)
String tg_post(const char*method, const String&body);
void tg_noop(long long chat_id, const char*text);

String tg_post(const char*method,const String&body){
    WiFiClientSecure tls; tls.setInsecure();
    if(!tls.connect("api.telegram.org",443))return "";
    String path=String("/bot")+BOT_TOKEN+"/"+method;
    tls.println("POST "+path+" HTTP/1.1"); tls.println("Host: api.telegram.org");
    tls.println("Content-Type: application/json");
    tls.println("Content-Length: "+String(body.length()));
    tls.println("Connection: close"); tls.println(); tls.print(body);
    String resp; uint32_t t=millis();
    while(tls.connected()&&millis()-t<6000) while(tls.available())resp+=(char)tls.read();
    tls.stop(); int idx=resp.indexOf("\r\n\r\n");
    return idx>=0?resp.substring(idx+4):"";
}
void tg_noop(long long chat_id,const char*text){
    StaticJsonDocument<512>doc; doc["chat_id"]=chat_id; doc["text"]=text;
    String body; serializeJson(doc,body); tg_post("sendMessage",body);
}

void poll_telegram(){
    StaticJsonDocument<128>req; req["offset"]=s_tg_offset+1; req["timeout"]=0; req["limit"]=5;
    String body; serializeJson(req,body);
    String resp=tg_post("getUpdates",body); if(!resp.length())return;
    DynamicJsonDocument doc(8192); if(deserializeJson(doc,resp))return;
    if(!doc["ok"])return;
    for(JsonObject upd:doc["result"].as<JsonArray>()){
        long uid=upd["update_id"]; if(uid>s_tg_offset)s_tg_offset=uid;
        if(!upd.containsKey("message"))continue;
        JsonObject msg=upd["message"];
        long long cid=msg["chat"]["id"]; if(cid!=ALLOWED_CHAT_ID)continue;
        const char*text=msg["text"]|""; if(!text[0])continue;
        String resp2=handle_cmd(String(text),true,cid);
        tg_noop(cid,resp2.c_str());
    }
}

// CDC serial command buffer
static String s_cdc_buf = "";
void handle_cdc(){
    while(Serial.available()){
        char c=Serial.read();
        if(c=='\n'||c=='\r'){
            s_cdc_buf.trim();
            if(s_cdc_buf.length()){
                term_sys(s_cdc_buf.c_str());
                String resp=handle_cmd(s_cdc_buf,false,0);
                Serial.println(resp);
            }
            s_cdc_buf="";
        } else { s_cdc_buf+=c; }
    }
}

void setup(){
    Serial.begin(115200);
    pinMode(LCD_PWR,OUTPUT); digitalWrite(LCD_PWR,HIGH); delay(50);
    if(!gfx->begin()){Serial.println("Display fail");while(1)delay(1000);}
    set_brightness(200);
    gfx->fillScreen(C_BG); gfx->setTextSize(1); gfx->setTextWrap(false);
    draw_header(); draw_footer();
    term_head("WyTerminal v3.0");
    term_sys("CDC serial commands");
    term_info("by Wyltek Industries");
    term_info("────────────────────");
    USB.begin(); Keyboard.begin();  delay(200);
    term_ok("USB HID + CDC ready");
    term_sys("Pi: echo /run ls > /dev/ttyACM0");
    WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID,WIFI_PASSWORD);
    term_info("WiFi connecting...");
    uint32_t t0=millis();
    while(WiFi.status()!=WL_CONNECTED&&millis()-t0<20000)delay(500);
    s_start_ms=millis();
    if(WiFi.status()==WL_CONNECTED){
        s_wifi_ok=true; draw_header();
        char buf[40]; snprintf(buf,40,"%s",WiFi.localIP().toString().c_str());
        term_ok(buf); term_ok("Telegram ready");
    } else { term_info("WiFi off — CDC only"); }
    draw_footer();
}

static uint32_t s_last_footer=0, s_last_tg=0;
void loop(){
    handle_cdc();
    if(s_wifi_ok&&millis()-s_last_tg>2000){
        if(WiFi.status()!=WL_CONNECTED){s_wifi_ok=false;draw_header();WiFi.reconnect();}
        else poll_telegram();
        s_last_tg=millis();
    } else if(!s_wifi_ok&&WiFi.status()==WL_CONNECTED){s_wifi_ok=true;draw_header();}
    if(millis()-s_last_footer>5000){draw_footer();s_last_footer=millis();}
    delay(50);
}
