/*
 * WyTerminal v4.2 — LilyGO T-Display S3 AMOLED
 * USB HID Keyboard + WiFi Telegram + Onboard SSH
 *
 * v4.1 additions over v4:
 *   - Auto target discovery: board sends WYTERMINAL_HELLO on CDC connect;
 *     host udev script replies TARGET user@ip — board sets as active target
 *   - /screenshot — SSH to target, capture screen, send as Telegram photo
 *
 * Commands:
 *   /run <text>        — HID type + enter
 *   /type <text>       — HID type (no enter)
 *   /enter             — press Enter
 *   /paste             — retype last /type text
 *   /key <combo>       — key combos (ctrl+c, ctrl+alt+t, etc.)
 *   /clear             — clear display
 *   /shell <cmd>       — SSH exec on current target
 *   /screenshot        — capture target screen, send as photo
 *   /target user@host  — switch SSH target
 *   /ssh_pass <pass>   — set SSH password for current target
 *   /status            — show status
 *   /help              — command list
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
#include "libssh_esp32.h"
#include <libssh/libssh.h>
#include "SPIFFS.h"

// ─── Config ──────────────────────────────────────────────────────────────────
#define WIFI_SSID        "D-Link the router"
#define WIFI_PASSWORD    "Ajeip853jw5590!"
#define BOT_TOKEN        "8688942400:AAFZKipOJnzroUWAea-zZuhZbLbRTiAluLM"
#define ALLOWED_CHAT_ID  1790655432LL

// Fallback SSH target (overridden by auto-discovery or /target)
#define DEFAULT_SSH_USER "orangepi"
#define DEFAULT_SSH_HOST "192.168.68.87"
#define DEFAULT_SSH_PORT 22
#define DEFAULT_SSH_PASS ""

// Screenshot: command run on target to capture screen to a temp file
// Tries gnome-screenshot (Wayland/X), falls back to scrot (X11)
#define SHOT_CMD \
  "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$(id -u)/bus " \
  "gnome-screenshot -f /tmp/wyt-shot.png 2>/dev/null || " \
  "scrot /tmp/wyt-shot.png 2>/dev/null || " \
  "import -window root /tmp/wyt-shot.png 2>/dev/null; " \
  "echo SHOTDONE"
#define SHOT_PATH "/tmp/wyt-shot.png"
// Max screenshot bytes to buffer (board has ~200KB free heap for this)
#define SHOT_MAX_BYTES 180000

// SSH key paths on SPIFFS
#define SSH_KEY_PATH  "/spiffs/.ssh/id_ed25519"
#define SSH_PUBKEY_PATH "/spiffs/.ssh/id_ed25519.pub"

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
static bool     s_target_discovered = false;  // true once host sent TARGET
static char     s_pubkey_b64[512] = "";            // our ed25519 public key (base64 blob)

// SSH target config
static char s_ssh_host[64]  = DEFAULT_SSH_HOST;
static char s_ssh_user[64]  = DEFAULT_SSH_USER;
static char s_ssh_pass[128] = DEFAULT_SSH_PASS;
static int  s_ssh_port      = DEFAULT_SSH_PORT;

// SSH async job types
#define JOB_NONE       0
#define JOB_SHELL      1
#define JOB_SCREENSHOT 2

struct SshJob {
    int  type;
    char cmd[512];
    long long chat_id;
    volatile bool pending;
    volatile bool done;
    char  result[2048];      // text output (JOB_SHELL + errors)
    uint8_t *imgbuf;         // heap-allocated PNG bytes (JOB_SCREENSHOT)
    size_t   imglen;
};
static SshJob s_ssh_job = {0};
static SemaphoreHandle_t s_ssh_mutex;
static SemaphoreHandle_t s_disp_mutex;

// ─── Terminal buffer ──────────────────────────────────────────────────────────
struct Line { char text[TERM_COLS+1]; uint16_t col; };
static Line s_buf[TERM_LINES];
static int  s_count = 0;

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
    gfx->setCursor(4,9); gfx->print("WyTerminal v4.2");
    gfx->setCursor(SCREEN_W-52,9);
    gfx->setTextColor(s_wifi_ok?C_GREEN:C_GREY);   gfx->print(s_wifi_ok?"WiFi":"wifi");
    gfx->setTextColor(C_ORANGE);  gfx->print(" SSH");
    gfx->setTextColor(C_PURPLE);  gfx->print(" CDC");
    xSemaphoreGive(s_disp_mutex);
}

void draw_footer(){
    if(xSemaphoreTake(s_disp_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    int y=SCREEN_H-FOOTER_H;
    gfx->fillRect(0,y,SCREEN_W,FOOTER_H,C_DKBLUE);
    gfx->setTextColor(C_GREY); gfx->setTextSize(1); gfx->setCursor(2,y+5);
    uint32_t up=(millis()-s_start_ms)/1000; char buf[48];
    snprintf(buf,48,"%s@%.20s  %lus", s_ssh_user, s_ssh_host, (unsigned long)up);
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

// ─── SSH keypair management ───────────────────────────────────────────────────
// Generate ed25519 keypair on first boot, store in SPIFFS.
// Exports the public key blob into s_pubkey_b64.
static void init_ssh_keys(){
    if(!SPIFFS.begin(true)){
        term_err("SPIFFS mount fail"); return;
    }
    ssh_key privkey = NULL;
    bool have_key = SPIFFS.exists(SSH_KEY_PATH);
    if(have_key){
        int rc = ssh_pki_import_privkey_file(SSH_KEY_PATH, NULL, NULL, NULL, &privkey);
        if(rc != SSH_OK){ have_key = false; privkey = NULL; }
    }
    if(!privkey){
        term_info("generating ed25519 key...");
        int rc = ssh_pki_generate(SSH_KEYTYPE_ED25519, 0, &privkey);
        if(rc != SSH_OK || !privkey){ term_err("keygen failed"); return; }
        ssh_pki_export_privkey_file(privkey, NULL, NULL, NULL, SSH_KEY_PATH);
        term_ok("ed25519 key generated");
    }
    // Export public key as base64
    ssh_key pubkey = NULL;
    if(ssh_pki_export_privkey_to_pubkey(privkey, &pubkey) == SSH_OK){
        char *b64 = NULL;
        if(ssh_pki_export_pubkey_base64(pubkey, &b64) == SSH_OK && b64){
            snprintf(s_pubkey_b64, sizeof(s_pubkey_b64), "ssh-ed25519 %s WyTerminal", b64);
            SSH_STRING_FREE_CHAR(b64);
        }
        ssh_key_free(pubkey);
    }
    ssh_key_free(privkey);
    if(s_pubkey_b64[0]) term_ok("pubkey ready");
    else term_err("pubkey export failed");
}

// ─── SSH helpers ─────────────────────────────────────────────────────────────
static ssh_session ssh_open_session(){
    ssh_session session = ssh_new();
    if(!session) return NULL;
    int port    = s_ssh_port;
    int timeout = 20;
    int verb    = SSH_LOG_NOLOG;
    ssh_options_set(session, SSH_OPTIONS_HOST,          s_ssh_host);
    ssh_options_set(session, SSH_OPTIONS_USER,          s_ssh_user);
    ssh_options_set(session, SSH_OPTIONS_PORT,          &port);
    ssh_options_set(session, SSH_OPTIONS_TIMEOUT,       &timeout);
    ssh_options_set(session, SSH_OPTIONS_LOG_VERBOSITY, &verb);
    // TOFU — auto-accept unknown hosts
    ssh_options_set(session, SSH_OPTIONS_KNOWNHOSTS,    "/dev/null");

    if(ssh_connect(session) != SSH_OK){
        ssh_free(session); return NULL;
    }
    ssh_session_update_known_hosts(session);

    // Tell libssh where our private key lives on SPIFFS
    ssh_options_set(session, SSH_OPTIONS_IDENTITY, SSH_KEY_PATH);

    int rc;
    if(s_ssh_pass[0]){
        rc = ssh_userauth_password(session, NULL, s_ssh_pass);
    } else {
        // Try our generated key first, then auto, then empty password
        rc = ssh_userauth_publickey_auto(session, NULL, NULL);
        if(rc != SSH_AUTH_SUCCESS)
            rc = ssh_userauth_password(session, NULL, "");
    }
    if(rc != SSH_AUTH_SUCCESS){
        ssh_disconnect(session); ssh_free(session); return NULL;
    }
    return session;
}

// Run a command, return stdout+stderr as String
static String ssh_exec_cmd(ssh_session session, const char *cmd){
    ssh_channel ch = ssh_channel_new(session);
    if(!ch) return "err:channel";
    if(ssh_channel_open_session(ch) != SSH_OK){
        ssh_channel_free(ch); return "err:open";
    }
    if(ssh_channel_request_exec(ch, cmd) != SSH_OK){
        ssh_channel_close(ch); ssh_channel_free(ch); return "err:exec";
    }
    char buf[256]; String out=""; int n;
    while((n=ssh_channel_read_timeout(ch,buf,sizeof(buf)-1,0,15000))>0){
        buf[n]=0; out+=buf; if(out.length()>2000){out+="\n[truncated]";break;}
    }
    while((n=ssh_channel_read_timeout(ch,buf,sizeof(buf)-1,1,2000))>0){
        buf[n]=0; out+=buf; if(out.length()>2000){out+="\n[truncated]";break;}
    }
    ssh_channel_send_eof(ch); ssh_channel_close(ch); ssh_channel_free(ch);
    return out.length()?out:"(no output)";
}

// Read a file from target by base64-encoding it through SSH exec.
// Returns decoded byte count, or -1 on failure.
// Much simpler than SFTP — works with any SSH server, no extra protocol.
static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static int b64val(char c){
    if(c>='A'&&c<='Z') return c-'A';
    if(c>='a'&&c<='z') return c-'a'+26;
    if(c>='0'&&c<='9') return c-'0'+52;
    if(c=='+') return 62; if(c=='/') return 63;
    return -1;
}

static int ssh_read_file_b64(ssh_session session, const char *path,
                              uint8_t **outbuf, size_t maxbytes){
    // Run: base64 <file> on the target
    char cmd[256]; snprintf(cmd,sizeof(cmd),"base64 %s",path);
    ssh_channel ch = ssh_channel_new(session);
    if(!ch) return -1;
    if(ssh_channel_open_session(ch)!=SSH_OK){ ssh_channel_free(ch); return -1; }
    if(ssh_channel_request_exec(ch,cmd)!=SSH_OK){
        ssh_channel_close(ch); ssh_channel_free(ch); return -1;
    }
    // Read base64 text
    String b64=""; char buf[256]; int n;
    while((n=ssh_channel_read_timeout(ch,buf,sizeof(buf)-1,0,20000))>0){
        buf[n]=0; b64+=buf;
        if(b64.length()>maxbytes*2) break;  // sanity limit
    }
    ssh_channel_send_eof(ch); ssh_channel_close(ch); ssh_channel_free(ch);
    if(!b64.length()) return -1;

    // Decode base64
    size_t est = (b64.length()*3)/4 + 4;
    uint8_t *raw = (uint8_t*)malloc(min(est,maxbytes));
    if(!raw) return -1;
    size_t out=0; int i=0; int len=b64.length();
    while(i<len && out<maxbytes){
        // skip whitespace
        while(i<len && (b64[i]=='\n'||b64[i]=='\r'||b64[i]==' ')) i++;
        if(i+3>=len) break;
        int a=b64val(b64[i]),b=b64val(b64[i+1]),c=b64val(b64[i+2]),d=b64val(b64[i+3]);
        i+=4;
        if(a<0||b<0) break;
        raw[out++]=(a<<2)|(b>>4);
        if(c>=0&&out<maxbytes) raw[out++]=((b&0xF)<<4)|(c>>2);
        if(d>=0&&out<maxbytes) raw[out++]=((c&0x3)<<6)|d;
    }
    if(out==0){ free(raw); return -1; }
    *outbuf=raw; return (int)out;
}

// ─── SSH FreeRTOS task ────────────────────────────────────────────────────────
void ssh_task(void *pv){
    while(1){
        if(s_ssh_job.pending && !s_ssh_job.done){
            // Open session
            libssh_begin();
            ssh_session session = ssh_open_session();
            if(!session){
                if(xSemaphoreTake(s_ssh_mutex, portMAX_DELAY)==pdTRUE){
                    strncpy(s_ssh_job.result,"err:SSH connect failed",sizeof(s_ssh_job.result)-1);
                    s_ssh_job.done=true; s_ssh_job.pending=false;
                    xSemaphoreGive(s_ssh_mutex);
                }
                vTaskDelay(pdMS_TO_TICKS(100)); continue;
            }

            if(s_ssh_job.type == JOB_SHELL){
                String out = ssh_exec_cmd(session, s_ssh_job.cmd);
                ssh_disconnect(session); ssh_free(session);
                if(xSemaphoreTake(s_ssh_mutex, portMAX_DELAY)==pdTRUE){
                    strncpy(s_ssh_job.result, out.c_str(), sizeof(s_ssh_job.result)-1);
                    s_ssh_job.result[sizeof(s_ssh_job.result)-1]=0;
                    s_ssh_job.imgbuf=NULL; s_ssh_job.imglen=0;
                    s_ssh_job.done=true; s_ssh_job.pending=false;
                    xSemaphoreGive(s_ssh_mutex);
                }

            } else if(s_ssh_job.type == JOB_SCREENSHOT){
                // 1. Run screenshot command on target
                term_ssh("capturing...");
                String out = ssh_exec_cmd(session, SHOT_CMD);
                if(out.indexOf("SHOTDONE") < 0){
                    ssh_disconnect(session); ssh_free(session);
                    if(xSemaphoreTake(s_ssh_mutex, portMAX_DELAY)==pdTRUE){
                        strncpy(s_ssh_job.result,"err:screenshot cmd failed",sizeof(s_ssh_job.result)-1);
                        s_ssh_job.done=true; s_ssh_job.pending=false;
                        xSemaphoreGive(s_ssh_mutex);
                    }
                    vTaskDelay(pdMS_TO_TICKS(100)); continue;
                }
                // 2. SCP the PNG back
                term_ssh("fetching PNG...");
                uint8_t *imgbuf = NULL;
                int imglen = ssh_read_file_b64(session, SHOT_PATH, &imgbuf, SHOT_MAX_BYTES);
                // 3. Cleanup temp file on target
                ssh_exec_cmd(session, "rm -f " SHOT_PATH);
                ssh_disconnect(session); ssh_free(session);

                if(xSemaphoreTake(s_ssh_mutex, portMAX_DELAY)==pdTRUE){
                    if(imglen > 0){
                        s_ssh_job.imgbuf = imgbuf;
                        s_ssh_job.imglen = (size_t)imglen;
                        strncpy(s_ssh_job.result,"ok:screenshot",sizeof(s_ssh_job.result)-1);
                    } else {
                        strncpy(s_ssh_job.result,"err:could not fetch PNG (no display?)",sizeof(s_ssh_job.result)-1);
                        if(imgbuf) free(imgbuf);
                    }
                    s_ssh_job.done=true; s_ssh_job.pending=false;
                    xSemaphoreGive(s_ssh_mutex);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ─── Telegram helpers ─────────────────────────────────────────────────────────
String tg_post(const char*method, const String&body){
    WiFiClientSecure tls; tls.setInsecure();
    if(!tls.connect("api.telegram.org",443)) return "";
    String path=String("/bot")+BOT_TOKEN+"/"+method;
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
    return idx>=0?resp.substring(idx+4):"";
}

void tg_send(long long chat_id, const char*text){
    String full=String(text); int len=full.length(); int pos=0;
    while(pos<len){
        String chunk=full.substring(pos,min(pos+4000,len));
        StaticJsonDocument<512> doc; doc["chat_id"]=chat_id; doc["text"]=chunk;
        String body; serializeJson(doc,body); tg_post("sendMessage",body);
        pos+=4000; if(pos<len) delay(200);
    }
}

// Send PNG bytes as a Telegram photo via multipart/form-data
void tg_send_photo(long long chat_id, uint8_t *png, size_t len, const char *caption){
    WiFiClientSecure tls; tls.setInsecure();
    if(!tls.connect("api.telegram.org",443)){
        tg_send(chat_id,"err:TLS connect failed for photo"); return;
    }
    String boundary="----WyT4Boundary";
    // Build header part
    String hdr="";
    hdr+="--"+boundary+"\r\n";
    hdr+="Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n";
    hdr+=String((long long)chat_id)+"\r\n";
    hdr+="--"+boundary+"\r\n";
    hdr+="Content-Disposition: form-data; name=\"caption\"\r\n\r\n";
    hdr+=String(caption)+"\r\n";
    hdr+="--"+boundary+"\r\n";
    hdr+="Content-Disposition: form-data; name=\"photo\"; filename=\"screenshot.png\"\r\n";
    hdr+="Content-Type: image/png\r\n\r\n";
    String tail="\r\n--"+boundary+"--\r\n";
    size_t total=hdr.length()+len+tail.length();

    String path=String("/bot")+BOT_TOKEN+"/sendPhoto";
    tls.println("POST "+path+" HTTP/1.1");
    tls.println("Host: api.telegram.org");
    tls.println("Content-Type: multipart/form-data; boundary="+boundary);
    tls.println("Content-Length: "+String(total));
    tls.println("Connection: close"); tls.println();
    tls.print(hdr);
    // Send PNG in chunks
    size_t sent=0;
    while(sent<len){ size_t chunk=min((size_t)1024,len-sent); tls.write(png+sent,chunk); sent+=chunk; }
    tls.print(tail);
    // Drain response
    uint32_t t=millis();
    while(tls.connected()&&millis()-t<10000) while(tls.available()) tls.read();
    tls.stop();
}

// ─── SSH job dispatcher + result checker ──────────────────────────────────────
static long long s_pending_chat_id=0;
static bool      s_ssh_reply_pending=false;

void check_ssh_result(){
    if(!s_ssh_reply_pending) return;
    bool done=false; int type=JOB_NONE;
    char result[2048]={0}; uint8_t *imgbuf=NULL; size_t imglen=0;

    if(xSemaphoreTake(s_ssh_mutex, pdMS_TO_TICKS(50))==pdTRUE){
        if(s_ssh_job.done){
            done=true; type=s_ssh_job.type;
            strncpy(result,s_ssh_job.result,sizeof(result)-1);
            imgbuf=s_ssh_job.imgbuf; imglen=s_ssh_job.imglen;
            s_ssh_job.imgbuf=NULL; s_ssh_job.imglen=0;
            s_ssh_job.done=false;
        }
        xSemaphoreGive(s_ssh_mutex);
    }
    if(!done) return;
    s_ssh_reply_pending=false;

    if(type==JOB_SCREENSHOT && imgbuf && imglen>0){
        term_ok("sending photo...");
        char cap[80]; snprintf(cap,80,"📸 %s@%s",s_ssh_user,s_ssh_host);
        tg_send_photo(s_pending_chat_id, imgbuf, imglen, cap);
        free(imgbuf);
        term_ok("photo sent");
    } else {
        tg_send(s_pending_chat_id, result);
        term_ok("ssh done");
    }
}

void dispatch_ssh(int type, const char *cmd, long long chat_id){
    if(s_ssh_job.pending||s_ssh_reply_pending){
        tg_send(chat_id,"err:ssh busy — try again"); return;
    }
    s_ssh_job.type=type;
    strncpy(s_ssh_job.cmd, cmd?cmd:"", sizeof(s_ssh_job.cmd)-1);
    s_ssh_job.chat_id=chat_id;
    s_ssh_job.imgbuf=NULL; s_ssh_job.imglen=0;
    s_ssh_job.done=false; s_ssh_job.pending=true;
    s_pending_chat_id=chat_id; s_ssh_reply_pending=true;
    if(type==JOB_SCREENSHOT) tg_send(chat_id,"⏳ capturing screenshot...");
    else tg_send(chat_id,"⏳ running...");
}

// ─── Target auto-discovery ────────────────────────────────────────────────────
// Called when host sends: TARGET user@host[:port]
void apply_target(const String &arg){
    int at=arg.indexOf('@');
    if(at<0) return;
    String user=arg.substring(0,at);
    String hostport=arg.substring(at+1); hostport.trim();
    int colon=hostport.indexOf(':');
    String host=colon>=0?hostport.substring(0,colon):hostport;
    int port=colon>=0?hostport.substring(colon+1).toInt():22;
    user.trim(); host.trim();
    if(!user.length()||!host.length()) return;
    strncpy(s_ssh_user,user.c_str(),sizeof(s_ssh_user)-1);
    strncpy(s_ssh_host,host.c_str(),sizeof(s_ssh_host)-1);
    s_ssh_port=port; s_ssh_pass[0]=0;
    s_target_discovered=true;
    draw_footer();
    char msg[64]; snprintf(msg,64,"auto: %s@%s",s_ssh_user,s_ssh_host);
    term_ok(msg);
}

// ─── Command handler ──────────────────────────────────────────────────────────
String handle_cmd(const String&t, bool send_tg, long long chat_id){
    String short_t=t.length()>30?t.substring(0,29)+">":t;
    term_cmd(short_t.c_str());

    // HID
    if(t.startsWith("/run ")){
        String cmd=t.substring(5); hid_type(cmd.c_str(),true);
        if(send_tg){ String m="▶️ "+cmd; tg_send(chat_id,m.c_str()); }
        term_ok("sent"); return "";
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
        dispatch_ssh(JOB_SHELL, cmd.c_str(), chat_id);
        return "";
    }
    if(t=="/screenshot"){
        if(!s_wifi_ok){ term_err("no wifi"); return "err:no wifi"; }
        dispatch_ssh(JOB_SCREENSHOT, NULL, chat_id);
        return "";
    }
    if(t.startsWith("/target ")){
        String arg=t.substring(8); arg.trim();
        apply_target(arg);
        String resp="🎯 target: "+String(s_ssh_user)+"@"+String(s_ssh_host)+":"+String(s_ssh_port);
        return resp;
    }
    if(t.startsWith("/ssh_pass ")){
        String pass=t.substring(10);
        strncpy(s_ssh_pass,pass.c_str(),sizeof(s_ssh_pass)-1);
        term_ok("pass set"); return "ok:password stored";
    }

    // Show our public key
    if(t=="/pubkey"){
        if(s_pubkey_b64[0]) return String(s_pubkey_b64);
        return "err:no key generated yet";
    }

    // Info
    if(t=="/status"){
        uint32_t up=(millis()-s_start_ms)/1000; char buf[256];
        snprintf(buf,256,
            "WyTerminal v4.2\n"
            "WiFi: %s\n"
            "Target: %s@%s:%d\n"
            "Auth: %s\n"
            "Discovery: %s\n"
            "Uptime: %lus",
            s_wifi_ok?WiFi.localIP().toString().c_str():"off",
            s_ssh_user, s_ssh_host, s_ssh_port,
            s_ssh_pass[0]?"password":"publickey/none",
            s_target_discovered?"auto":"default",
            (unsigned long)up);
        term_info("status"); return String(buf);
    }
    if(t=="/help"){
        return
            "WyTerminal v4.2\n"
            "/run <text>       — HID type+enter\n"
            "/type <text>      — HID type\n"
            "/enter /paste     — enter / repeat\n"
            "/key <combo>      — ctrl+c, ctrl+alt+t…\n"
            "/clear            — clear display\n"
            "/shell <cmd>      — SSH exec on target\n"
            "/screenshot       — capture+send screen\n"
            "/target user@host — switch target\n"
            "/ssh_pass <pass>  — set SSH password\n"
            "/status           — show status";
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
    for(JsonObject upd:doc["result"].as<JsonArray>()){
        long uid=upd["update_id"]; if(uid>s_tg_offset) s_tg_offset=uid;
        if(!upd.containsKey("message")) continue;
        JsonObject msg=upd["message"];
        long long cid=msg["chat"]["id"]; if(cid!=ALLOWED_CHAT_ID) continue;
        const char*text=msg["text"]|""; if(!text[0]) continue;
        String tcmd=String(text);
        int at_idx=tcmd.indexOf('@'); if(at_idx>0) tcmd=tcmd.substring(0,at_idx);
        String r=handle_cmd(tcmd,true,cid);
        if(r.length()) tg_send(cid,r.c_str());
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
                // Auto-discovery: host sends "TARGET user@host"
                if(s_cdc_buf.startsWith("TARGET ")){
                    String arg=s_cdc_buf.substring(7);
                    apply_target(arg);
                    Serial.println("ok:target_set");
                    // Immediately broadcast our public key so host can install it
                    if(s_pubkey_b64[0]){
                        Serial.print("PUBKEY ");
                        Serial.println(s_pubkey_b64);
                    }
                } else {
                    term_sys(s_cdc_buf.c_str());
                    String resp=handle_cmd(s_cdc_buf,false,0);
                    if(resp.length()) Serial.println(resp);
                }
            }
            s_cdc_buf="";
        } else { s_cdc_buf+=c; }
    }
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup(){
    Serial.begin(115200);
    pinMode(LCD_PWR,OUTPUT); digitalWrite(LCD_PWR,HIGH); delay(50);
    if(!gfx->begin()){ Serial.println("Display fail"); while(1) delay(1000); }
    set_brightness(200);
    gfx->fillScreen(C_BG); gfx->setTextSize(1); gfx->setTextWrap(false);

    s_disp_mutex = xSemaphoreCreateMutex();
    s_ssh_mutex  = xSemaphoreCreateMutex();

    draw_header(); draw_footer();
    term_head("WyTerminal v4.2");
    term_sys("SSH + auto-discover");
    term_info("by Wyltek Industries");
    term_info("────────────────────");

    USB.begin(); Keyboard.begin(); delay(200);
    term_ok("USB HID + CDC ready");
    // Generate/load SSH keypair
    init_ssh_keys();
    // Signal host that we're ready for TARGET announcement
    Serial.println("WYTERMINAL_HELLO");
    term_info("waiting for TARGET...");

    WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID,WIFI_PASSWORD);
    term_info("WiFi connecting...");
    uint32_t t0=millis();
    while(WiFi.status()!=WL_CONNECTED && millis()-t0<20000) delay(500);
    s_start_ms=millis();
    if(WiFi.status()==WL_CONNECTED){
        s_wifi_ok=true; draw_header();
        char buf[48]; snprintf(buf,48,"%s",WiFi.localIP().toString().c_str());
        term_ok(buf);
        libssh_begin();
        term_ok("SSH engine ready");
        term_ok("Telegram ready");
    } else {
        term_info("WiFi off — CDC+HID only");
    }
    if(!s_target_discovered){
        char tgt[80]; snprintf(tgt,80,"default: %s@%s",s_ssh_user,s_ssh_host);
        term_ssh(tgt);
    }
    draw_footer();

    xTaskCreatePinnedToCore(ssh_task,"ssh",32768,NULL,(tskIDLE_PRIORITY+2),NULL,0);
}

// ─── Loop ─────────────────────────────────────────────────────────────────────
static uint32_t s_last_footer=0, s_last_tg=0;

void loop(){
    handle_cdc();
    if(s_wifi_ok){
        check_ssh_result();
        if(millis()-s_last_tg>2000){
            if(WiFi.status()!=WL_CONNECTED){ s_wifi_ok=false; draw_header(); WiFi.reconnect(); }
            else poll_telegram();
            s_last_tg=millis();
        }
    } else if(WiFi.status()==WL_CONNECTED){
        s_wifi_ok=true; draw_header();
    }
    if(millis()-s_last_footer>5000){ draw_footer(); s_last_footer=millis(); }
    delay(50);
}
