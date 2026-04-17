#include <Arduino.h>
#include <U8g2lib.h>
#include <SPI.h>   // keep for SPI option
#include <Wire.h>  // for 4-pin I2C OLED
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <DNSServer.h>
#include <ArduinoJson.h>

// ─── OLED — 4-pin I2C (VCC, GND, SDA, SCL) ─────────────────────────────
// SSD1306 (most common blue/white OLED):
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
// ESP32-S3-Zero default I2C: SDA=GPIO8, SCL=GPIO9
// For SH1106: U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
// Old 5-wire SPI: U8G2_SH1106_128X64_NONAME_F_4W_SW_SPI u8g2(U8G2_R0,1,2,5,4,3);

// ─── Network ─────────────────────────────────────────────────────────────
const char* AP_SSID = "IBMOVS-Chat";
const char* AP_PASS = "";
const IPAddress AP_IP(192, 168, 4, 1);
const IPAddress AP_SUBNET(255, 255, 255, 0);

WebServer        httpServer(80);
WebSocketsServer wsServer(81);
DNSServer        dnsServer;

// ─── State ───────────────────────────────────────────────────────────────
unsigned long startTime;
int  totalMsgs  = 0;
bool clients[10] = {false};
String clientNames[10];

// ─── Log buffer ──────────────────────────────────────────────────────────
#define MAX_LOGS 40
String logBuf[MAX_LOGS];
int logHead = 0, logCount = 0;

void addLog(String msg) {
  Serial.println(msg);
  logBuf[logHead] = msg;
  logHead = (logHead + 1) % MAX_LOGS;
  if (logCount < MAX_LOGS) logCount++;
}

String getLogs() {
  String out = "";
  int s = (logCount < MAX_LOGS) ? 0 : logHead;
  for (int i = 0; i < logCount; i++)
    out += logBuf[(s + i) % MAX_LOGS] + "\n";
  return out;
}

// ─── OLED 3-screen cycling ───────────────────────────────────────────────
// Screen 0 = Main (WiFi/IP/Users/Msgs)
// Screen 1 = Activity (last sender + message)
// Screen 2 = Uptime clock
#define SCR_MAIN 0
#define SCR_ACT  1
#define SCR_TIME 2

int           oledScr  = SCR_MAIN;
unsigned long oledNext = 0;
String        actSndr  = "";   // last message sender
String        actMsg   = "";   // last message text
bool          actNew   = false; // jump to activity screen on new msg

// Truncate string to max chars (with ".." suffix)
String trOled(String s, int n) {
  if ((int)s.length() <= n) return s;
  return s.substring(0, n - 2) + "..";
}

void oledMain() {
  int c = WiFi.softAPgetStationNum();
  char cu[20], cm[20];
  sprintf(cu, "Users : %d", c);
  sprintf(cm, "Msgs  : %d", totalMsgs);
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  int tw = u8g2.getStrWidth("IBMOVS CHAT");
  u8g2.drawStr((128 - tw) / 2, 10, "IBMOVS CHAT");
  u8g2.drawHLine(0, 12, 128);
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(0, 22, "WiFi: IBMOVS-Chat");
  u8g2.drawStr(0, 31, "IP  : 192.168.4.1");
  u8g2.drawStr(0, 40, cu);
  u8g2.drawStr(0, 49, cm);
  u8g2.drawHLine(0, 52, 128);
  u8g2.setFont(u8g2_font_4x6_tf);
  u8g2.drawStr(0, 62, "Open: chat.ibmovs.com");
  u8g2.sendBuffer();
}

void oledActivity() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 10, "LAST MSG");
  u8g2.drawHLine(0, 12, 128);
  u8g2.setFont(u8g2_font_5x7_tf);
  if (actSndr.length() == 0) {
    u8g2.drawStr(8, 34, "No messages yet..");
  } else {
    // Sender name line
    u8g2.drawStr(0, 23, trOled(actSndr, 21).c_str());
    // Message - up to 2 lines of 21 chars each
    String m1 = trOled(actMsg, 21);
    u8g2.drawStr(0, 33, m1.c_str());
    if ((int)actMsg.length() > 21) {
      u8g2.drawStr(0, 42, trOled(actMsg.substring(21), 21).c_str());
    }
  }
  int c = WiFi.softAPgetStationNum();
  char s[26]; sprintf(s, "%d online | %d msgs", c, totalMsgs);
  u8g2.drawHLine(0, 52, 128);
  u8g2.setFont(u8g2_font_4x6_tf);
  u8g2.drawStr(0, 62, s);
  u8g2.sendBuffer();
}

void oledUptime() {
  unsigned long e = (millis() - startTime) / 1000;
  char tb[10]; sprintf(tb, "%02lu:%02lu:%02lu", e / 3600, (e / 60) % 60, e % 60);
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tf);
  int tw = u8g2.getStrWidth("-- UPTIME --");
  u8g2.drawStr((128 - tw) / 2, 9, "-- UPTIME --");
  u8g2.drawHLine(0, 11, 128);
  u8g2.setFont(u8g2_font_logisoso16_tf);
  tw = u8g2.getStrWidth(tb);
  u8g2.drawStr((128 - tw) / 2, 38, tb);
  u8g2.drawHLine(0, 46, 128);
  int c = WiFi.softAPgetStationNum();
  char s[26]; sprintf(s, "Users:%d  Msgs:%d", c, totalMsgs);
  u8g2.setFont(u8g2_font_4x6_tf);
  u8g2.drawStr(0, 57, s);
  u8g2.sendBuffer();
}

void oledDraw() {
  unsigned long now = millis();
  // New message? Jump to activity screen for 5s
  if (actNew) {
    oledScr  = SCR_ACT;
    oledNext = now + 5000;
    actNew   = false;
  } else if (now >= oledNext) {
    oledScr  = (oledScr + 1) % 3;
    // Main=4s, Activity=4s, Uptime=3s
    unsigned long dur = (oledScr == SCR_TIME) ? 3000 : 4000;
    oledNext = now + dur;
  }
  switch (oledScr) {
    case SCR_MAIN: oledMain();     break;
    case SCR_ACT:  oledActivity(); break;
    case SCR_TIME: oledUptime();   break;
  }
}

// ─── Broadcast helpers ────────────────────────────────────────────────────
void broadcastExcept(uint8_t skip, String p) {
  for (int i = 0; i < 10; i++)
    if (clients[i] && i != skip) wsServer.sendTXT(i, p);
}
void broadcastAll(String p) {
  for (int i = 0; i < 10; i++)
    if (clients[i]) wsServer.sendTXT(i, p);
}

String buildOnlineList() {
  StaticJsonDocument<512> doc;
  doc["type"] = "online";
  JsonArray arr = doc.createNestedArray("users");
  for (int i = 0; i < 10; i++)
    if (clients[i] && clientNames[i].length() > 0) arr.add(clientNames[i]);
  String out; serializeJson(doc, out); return out;
}

// ─── WebSocket events ─────────────────────────────────────────────────────
void wsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {

    case WStype_CONNECTED:
      clients[num] = true; clientNames[num] = "";
      addLog("[WS] #" + String(num) + " connected");
      wsServer.setAutoPing(false);
      break;

    case WStype_DISCONNECTED: {
      String name = clientNames[num];
      clients[num] = false; clientNames[num] = "";
      addLog("[WS] #" + String(num) + " (" + name + ") left");
      if (name.length()) {
        StaticJsonDocument<128> d;
        d["type"] = "system"; d["text"] = name + " left the chat";
        String o; serializeJson(d, o); broadcastAll(o);
        broadcastAll(buildOnlineList());
      }
      break;
    }

    case WStype_TEXT: {
      String msg = String((char*)payload);

      // Fast path: peek type field only
      StaticJsonDocument<256> hdr;
      DeserializationError err = deserializeJson(hdr, msg.substring(0, min((int)msg.length(), 200)));

      if (!err) {
        String t = hdr["type"].as<String>();

        // ── Image chunks — relay as-is ──
        if (t == "img_start" || t == "img_chunk" || t == "img_end") {
          broadcastExcept(num, msg);
          if (t == "img_end") {
            totalMsgs++;
            actSndr = clientNames[num]; actMsg = "[Image]"; actNew = true;
            addLog("[IMG] " + clientNames[num] + " sent image");
          }
          return;
        }

        // ── Voice chunks — relay as-is (NEW) ──
        if (t == "voice_start" || t == "voice_chunk" || t == "voice_end") {
          broadcastExcept(num, msg);
          if (t == "voice_end") {
            totalMsgs++;
            actSndr = clientNames[num]; actMsg = "[Voice msg]"; actNew = true;
            addLog("[VOICE] " + clientNames[num] + " sent voice");
          }
          return;
        }

        // ── Typing / reaction ──
        if (t == "typing")   { broadcastExcept(num, msg); return; }
        if (t == "reaction") { broadcastAll(msg); return; }
      }

      // Full parse for join/chat
      StaticJsonDocument<1024> doc;
      if (deserializeJson(doc, msg)) { addLog("[WS] JSON err"); return; }
      String msgType = doc["type"].as<String>();

      if (msgType == "join") {
        String name = doc["name"].as<String>();
        clientNames[num] = name;
        addLog("[JOIN] " + name);
        StaticJsonDocument<128> sys;
        sys["type"] = "system";
        sys["text"] = name + " joined the chat \xF0\x9F\x91\x8B";
        String so; serializeJson(sys, so); broadcastExcept(num, so);
        broadcastAll(buildOnlineList());
        StaticJsonDocument<64> ack; ack["type"] = "joined";
        String ao; serializeJson(ack, ao); wsServer.sendTXT(num, ao);
        return;
      }

      if (msgType == "chat") {
        totalMsgs++;
        String txt = doc["text"].as<String>();
        addLog("[MSG] " + clientNames[num] + ": " + txt);
        actSndr = clientNames[num]; actMsg = txt; actNew = true;
        broadcastAll(msg);
        return;
      }

      break;
    }
  }
}

// ─── Handlers ─────────────────────────────────────────────────────────────
void handleRoot()  { httpServer.send_P(200, "text/html", HTML); }
void handleLogs()  { httpServer.send(200, "text/plain", getLogs()); }

// Return 204 to prevent captive portal auto-popup.
// DNS still resolves chat.ibmovs.com → 192.168.4.1, so manual navigation works.
void handleNotFound() { httpServer.send(204, "text/plain", ""); }

// ─── HTML ─────────────────────────────────────────────────────────────────
const char HTML[] PROGMEM = R"rawhtml(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<title>IBMOVS Chat</title>
<style>
*{box-sizing:border-box;margin:0;padding:0;-webkit-tap-highlight-color:transparent}
:root{
  --bg:#0a0a14;--bg2:#111122;--bg3:#1a1a2e;--bg4:#16213e;
  --accent:#4f8ef7;--green:#2ecc71;--text:#e8e8f0;--text2:#9090b0;
  --border:#2a2a4a;--r:18px;
}
html,body{height:100%;height:100dvh;overflow:hidden;background:var(--bg);color:var(--text);font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif}
#app{display:flex;flex-direction:column;height:100%;height:100dvh}

/* ── Header with shimmer ── */
#hdr{background:linear-gradient(120deg,var(--bg4) 0%,#1c2e5e 50%,var(--bg4) 100%);
  background-size:200% 100%;animation:hdrShimmer 6s linear infinite;
  padding:10px 14px;display:flex;align-items:center;gap:10px;
  border-bottom:1px solid var(--border);flex-shrink:0}
@keyframes hdrShimmer{0%{background-position:0% 50%}100%{background-position:200% 50%}}
.av{width:38px;height:38px;border-radius:50%;
  background:linear-gradient(135deg,#4f8ef7,#6c63ff);
  display:flex;align-items:center;justify-content:center;
  font-weight:800;font-size:15px;flex-shrink:0;
  animation:avPulse 3s ease-in-out infinite}
@keyframes avPulse{0%,100%{box-shadow:0 0 0 0 #4f8ef733}50%{box-shadow:0 0 0 8px #4f8ef711}}
.info{flex:1;min-width:0}
.info h1{font-size:15px;font-weight:700;color:var(--accent)}
.info .sub{font-size:11px;color:var(--text2)}
.btns{display:flex;gap:6px}
.btns button{background:var(--bg3);border:1px solid var(--border);color:var(--text);
  border-radius:8px;padding:5px 9px;font-size:12px;cursor:pointer;transition:all .15s}
.btns button:active{transform:scale(.88)}

/* ── Online bar ── */
#onBar{background:var(--bg2);padding:5px 14px;font-size:11px;
  border-bottom:1px solid var(--border);flex-shrink:0;
  display:flex;align-items:center;gap:6px;overflow-x:auto;white-space:nowrap}
.dot{width:7px;height:7px;background:var(--green);border-radius:50%;
  flex-shrink:0;animation:dotGlow 2s ease-in-out infinite}
@keyframes dotGlow{0%,100%{box-shadow:0 0 4px var(--green)}50%{box-shadow:0 0 14px var(--green),0 0 22px var(--green)}}
.chip{background:var(--bg3);border:1px solid var(--border);border-radius:20px;
  padding:2px 8px;font-size:10px;color:var(--text2);flex-shrink:0;transition:all .2s}
.chip.me{border-color:var(--accent);color:var(--accent)}
.chip.istyping{border-color:#ffd43b88;color:#ffd43b;background:#ffd43b11}
.tyiDots{display:inline-flex;gap:2px;vertical-align:middle;margin-left:2px}
.tyiDots span{width:3px;height:3px;background:#ffd43b;border-radius:50%;animation:tdf .7s infinite}
.tyiDots span:nth-child(2){animation-delay:.15s}.tyiDots span:nth-child(3){animation-delay:.3s}
@keyframes tdf{0%,80%,100%{opacity:.2}40%{opacity:1}}

/* ── Messages ── */
#msgs{flex:1;overflow-y:auto;padding:12px 14px;display:flex;flex-direction:column;gap:6px;scroll-behavior:smooth}
#msgs::-webkit-scrollbar{width:3px}
#msgs::-webkit-scrollbar-thumb{background:var(--border);border-radius:2px}
.bw{display:flex;flex-direction:column;max-width:78%}
.bw.mine{align-self:flex-end;align-items:flex-end}
.bw.other{align-self:flex-start;align-items:flex-start}
/* Slide-up + pop for new messages */
.bw.new{animation:bubIn .25s cubic-bezier(.34,1.56,.64,1) both}
@keyframes bubIn{from{opacity:0;transform:translateY(12px) scale(.93)}to{opacity:1;transform:none}}
.meta{font-size:10px;color:var(--text2);margin-bottom:2px;padding:0 4px}
.nm{font-weight:600}
.bubble{padding:9px 13px;border-radius:var(--r);font-size:14px;line-height:1.45;
  word-break:break-word;position:relative;cursor:pointer;transition:opacity .1s}
.bubble:active{opacity:.75}
.mine .bubble{background:linear-gradient(135deg,#4f8ef7,#6c63ff);border-bottom-right-radius:4px;color:#fff}
.other .bubble{background:var(--bg3);border-bottom-left-radius:4px;border:1px solid var(--border)}

/* ── Image bubble ── */
.imgWrap{position:relative;min-width:80px;min-height:60px}
.imgWrap img{max-width:100%;max-height:220px;border-radius:10px;display:block}
.imgProg{position:absolute;inset:0;background:#0008;display:flex;flex-direction:column;
  align-items:center;justify-content:center;border-radius:10px;gap:6px}
.imgProg .pbar{width:80%;height:4px;background:#fff3;border-radius:2px;overflow:hidden}
.imgProg .pfill{height:100%;background:var(--accent);transition:width .2s;border-radius:2px}
.imgProg .ptxt{font-size:11px;color:#fff}

/* ── Voice bubble ── */
.voiceBub{display:flex;align-items:center;gap:8px;min-width:155px;padding:2px 0}
.vPlayBtn{width:33px;height:33px;border-radius:50%;border:none;cursor:pointer;
  display:flex;align-items:center;justify-content:center;font-size:13px;flex-shrink:0;
  transition:transform .15s;background:rgba(255,255,255,.25);color:#fff}
.other .vPlayBtn{background:var(--accent);color:#fff}
.vPlayBtn:active{transform:scale(.82)}
.vBars{display:flex;align-items:center;gap:2px;flex:1;height:26px}
.vBar{width:3px;border-radius:2px;background:rgba(255,255,255,.45);transition:height .15s}
.other .vBar{background:var(--accent)}
.vBar:nth-child(1){height:6px} .vBar:nth-child(2){height:13px} .vBar:nth-child(3){height:20px}
.vBar:nth-child(4){height:16px} .vBar:nth-child(5){height:10px} .vBar:nth-child(6){height:17px}
.vBar:nth-child(7){height:8px}
.vBars.playing .vBar{animation:vbWave .5s ease-in-out infinite}
.vBars.playing .vBar:nth-child(1){animation-delay:.00s} .vBars.playing .vBar:nth-child(2){animation-delay:.07s}
.vBars.playing .vBar:nth-child(3){animation-delay:.14s} .vBars.playing .vBar:nth-child(4){animation-delay:.21s}
.vBars.playing .vBar:nth-child(5){animation-delay:.28s} .vBars.playing .vBar:nth-child(6){animation-delay:.35s}
.vBars.playing .vBar:nth-child(7){animation-delay:.42s}
@keyframes vbWave{0%,100%{height:4px}50%{height:22px}}
.vDur{font-size:11px;opacity:.75;flex-shrink:0;font-variant-numeric:tabular-nums;min-width:28px}

/* ── Timestamps ── */
.ts{font-size:10px;opacity:.5;margin-top:2px;padding:0 4px}
.sys{align-self:center;background:#1118;border:1px solid var(--border);
  border-radius:20px;padding:3px 12px;font-size:11px;color:var(--text2);
  margin:2px 0;animation:sysIn .3s ease}
@keyframes sysIn{from{opacity:0;transform:scale(.88)}to{opacity:1;transform:none}}

/* ── Reactions ── */
.rxPicker{display:none;position:absolute;bottom:calc(100% + 6px);left:0;
  background:var(--bg3);border:1px solid var(--border);border-radius:12px;
  padding:6px 8px;gap:6px;flex-wrap:wrap;width:180px;z-index:10;box-shadow:0 4px 20px #0008}
.rxPicker.show{display:flex}
.rxPicker span{font-size:22px;cursor:pointer}
.rxRow{display:flex;gap:3px;margin-top:3px;flex-wrap:wrap}
.rxChip{background:var(--bg3);border:1px solid var(--border);border-radius:20px;padding:2px 7px;font-size:13px;cursor:pointer}

/* ── Typing bar ── */
#typBar{padding:3px 16px;min-height:20px;font-size:11px;color:var(--text2);flex-shrink:0;font-style:italic}
.df{display:inline-flex;gap:3px;vertical-align:middle}
.df span{width:4px;height:4px;background:var(--text2);border-radius:50%;animation:df .9s infinite}
.df span:nth-child(2){animation-delay:.2s}.df span:nth-child(3){animation-delay:.4s}
@keyframes df{0%,80%,100%{opacity:.2}40%{opacity:1}}

/* ── Input bar ── */
#inp{padding:8px 12px;background:var(--bg2);border-top:1px solid var(--border);
  display:flex;gap:8px;align-items:flex-end;flex-shrink:0}
#txtInp{flex:1;background:var(--bg3);border:1px solid var(--border);border-radius:22px;
  padding:10px 16px;color:var(--text);font-size:14px;outline:none;resize:none;
  max-height:90px;min-height:40px;overflow-y:auto;line-height:1.4;transition:border-color .2s}
#txtInp:focus{border-color:var(--accent)}
#txtInp:empty:before{content:attr(placeholder);color:var(--text2);pointer-events:none}
.ibtn{width:40px;height:40px;border-radius:50%;border:none;cursor:pointer;
  display:flex;align-items:center;justify-content:center;font-size:17px;flex-shrink:0;transition:all .15s}
.ibtn:active{transform:scale(.82)}
#imgBtn{background:var(--bg3);border:1px solid var(--border);color:var(--text2)}
#micBtn{background:var(--bg3);border:1px solid var(--border);color:var(--text2)}
#micBtn.rec{background:#e74c3c!important;border-color:#e74c3c!important;color:#fff!important;
  animation:recPulse .7s ease-in-out infinite}
@keyframes recPulse{0%,100%{box-shadow:0 0 0 0 #e74c3c55}50%{box-shadow:0 0 0 9px #e74c3c00}}
#sndBtn{background:linear-gradient(135deg,#4f8ef7,#6c63ff);color:#fff;box-shadow:0 2px 10px #4f8ef755}

/* ── Modals ── */
.modal{display:none;position:fixed;inset:0;background:#000a;z-index:200;align-items:center;justify-content:center}
.modal.show{display:flex}
.mbox{background:var(--bg3);border:1px solid var(--border);border-radius:20px;
  padding:28px 24px;width:min(320px,90vw);text-align:center;
  animation:mboxIn .28s cubic-bezier(.34,1.56,.64,1) both}
@keyframes mboxIn{from{opacity:0;transform:scale(.8) translateY(20px)}to{opacity:1;transform:none}}
.mbox h2{color:var(--accent);margin-bottom:6px;font-size:19px}
.mbox p{color:var(--text2);font-size:12px;margin-bottom:18px;line-height:1.5}
.mbox input{width:100%;background:var(--bg);border:1px solid var(--border);
  border-radius:10px;padding:11px 14px;color:var(--text);font-size:15px;
  outline:none;margin-bottom:12px;transition:border-color .2s}
.mbox input:focus{border-color:var(--accent)}
.pri{width:100%;background:linear-gradient(135deg,#4f8ef7,#6c63ff);border:none;
  border-radius:10px;padding:12px;color:#fff;font-size:15px;font-weight:700;
  cursor:pointer;transition:opacity .15s}
.pri:active{opacity:.8}
#logBox{background:var(--bg);border:1px solid var(--border);border-radius:16px;
  padding:16px;width:min(400px,95vw);max-height:72vh;display:flex;flex-direction:column;gap:8px}
#logBox h3{color:var(--accent);font-size:14px}
#logContent{flex:1;overflow-y:auto;font-family:monospace;font-size:11px;color:#7f8;
  background:#050505;border-radius:8px;padding:10px;white-space:pre-wrap;word-break:break-all;min-height:80px}
#logBox .lbtn{background:var(--bg3);border:1px solid var(--border);color:var(--text);border-radius:8px;padding:7px;cursor:pointer;font-size:13px}
#imgView{display:none;position:fixed;inset:0;background:#000d;z-index:300;align-items:center;justify-content:center}
#imgView.show{display:flex}
#imgView img{max-width:95vw;max-height:90vh;border-radius:12px}
#imgView .cls{position:absolute;top:16px;right:20px;color:#fff;font-size:28px;cursor:pointer}
</style>
</head>
<body>
<div id="app">
  <div id="hdr">
    <div class="av">IB</div>
    <div class="info">
      <h1>IBMOVS Chat</h1>
      <div class="sub" id="subTxt">Connecting...</div>
    </div>
    <div class="btns">
      <button onclick="showLogs()">&#128203; Logs</button>
      <button onclick="clearChat()">&#128465;</button>
    </div>
  </div>
  <div id="onBar"><div class="dot"></div><span id="onList">No one online</span></div>
  <div id="msgs"></div>
  <div id="typBar"></div>
  <div id="inp">
    <button class="ibtn" id="imgBtn" onclick="document.getElementById('imgInp').click()" title="Send image">&#128247;</button>
    <input type="file" id="imgInp" accept="image/*" style="display:none" onchange="sendImage(this)">
    <button class="ibtn" id="micBtn"
      onpointerdown="startVoice(event)"
      onpointerup="stopVoice()"
      onpointerleave="stopVoice()"
      title="Hold to record voice">&#127908;</button>
    <div id="txtInp" contenteditable="true" placeholder="Message..."
         oninput="onType()"
         onkeydown="if(event.key==='Enter'&&!event.shiftKey){event.preventDefault();sendMsg()}"></div>
    <button class="ibtn" id="sndBtn" onclick="sendMsg()">&#10148;</button>
  </div>
</div>

<!-- Name modal -->
<div class="modal show" id="nameModal">
  <div class="mbox">
    <div style="font-size:48px;margin-bottom:10px">&#128172;</div>
    <h2>IBMOVS Chat</h2>
    <p>Hosted on ESP32-S3-Zero<br>Local WiFi &#8226; No internet needed<br>
    <span style="color:var(--accent);font-size:11px">chat.ibmovs.com &bull; 192.168.4.1</span></p>
    <input id="nameInp" type="text" placeholder="Your name..." maxlength="20" autocomplete="off"
           onkeydown="if(event.key==='Enter')join()">
    <button class="pri" onclick="join()">Join Chat &#128640;</button>
  </div>
</div>

<!-- Log modal -->
<div class="modal" id="logModal">
  <div id="logBox">
    <h3>&#128203; ESP32 Logs</h3>
    <div id="logContent">Loading...</div>
    <button class="lbtn" onclick="fetchLogs()">&#128260; Refresh</button>
    <button class="lbtn" onclick="document.getElementById('logModal').classList.remove('show')">Close</button>
  </div>
</div>

<!-- Image viewer -->
<div id="imgView" onclick="this.classList.remove('show')">
  <div class="cls">&#10005;</div>
  <img id="viewImg" src="">
</div>

<script>
// ── Constants ──
const SK         = 'ibmovs_v5';
const RX         = ['&#128077;','&#10084;&#65039;','&#128514;','&#128558;','&#128546;','&#128293;','&#128079;','&#128175;'];
const COLS       = ['#4f8ef7','#ff6b6b','#51cf66','#ffd43b','#cc5de8','#ff922b','#20c997','#f06595'];
const CHUNK_SIZE = 3000;

// ── State ──
let ws, myName;
let typUsers     = {}, typTimer;
let msgMap       = {};
let imgAssembly  = {};
let voiceAssembly= {};
let voiceMap     = {};  // voiceId -> dataURL, in-memory only (not persisted)
let onlineUsers  = [];
let mediaRecorder= null;
let audioChunks  = [];
let isRecording  = false;
let currentAudio = null;
let openRx       = null;

// ── Helpers ──
function nc(n){let h=0;for(let c of n)h=(h*31+c.charCodeAt(0))&0xffff;return COLS[h%COLS.length]}
function ft(t){const d=new Date(t);return d.getHours().toString().padStart(2,'0')+':'+d.getMinutes().toString().padStart(2,'0')}
function esc(t){return String(t).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;')}
function uid(){return Math.random().toString(36).slice(2,10)}
function lm(){try{return JSON.parse(localStorage.getItem(SK)||'[]')}catch{return[]}}
function sm(m){if(m.length>200)m=m.slice(-200);localStorage.setItem(SK,JSON.stringify(m))}

// ── Join ──
function join(){
  const n=document.getElementById('nameInp').value.trim();
  if(!n)return document.getElementById('nameInp').focus();
  myName=n;
  document.getElementById('nameModal').classList.remove('show');
  connectWS();
  // Check mic availability (needs HTTPS or local HTTP — may not work in Chrome on plain HTTP)
  if(!window.MediaRecorder || !navigator.mediaDevices){
    const btn=document.getElementById('micBtn');
    btn.style.opacity='0.35';
    btn.title='Voice not supported (needs Firefox or HTTPS)';
  }
}

// ── WebSocket ──
function connectWS(){
  document.getElementById('subTxt').textContent='Connecting...';
  ws=new WebSocket('ws://192.168.4.1:81');
  ws.onopen=()=>{
    document.getElementById('subTxt').textContent='ESP32-S3 \u2022 Connected \u{1F7E2}';
    ws.send(JSON.stringify({type:'join',name:myName}));
    renderAll();
    document.getElementById('txtInp').focus();
  };
  ws.onclose=()=>{
    document.getElementById('subTxt').textContent='Reconnecting...';
    setTimeout(connectWS,2000);
  };
  ws.onmessage=e=>{try{handle(JSON.parse(e.data))}catch(err){console.error(err)}};
  ws.onerror=e=>console.error('[WS]',e);
}

// ── Handle incoming messages ──
function handle(m){
  if(m.type==='online'){
    onlineUsers=m.users||[];
    renderOnline(onlineUsers);
    return;
  }
  if(m.type==='system'){appendSys(m.text);return}
  if(m.type==='joined')return;
  if(m.type==='typing'){
    if(m.name!==myName){
      typUsers[m.name]=Date.now();
      renderTyp();
      renderOnline(onlineUsers); // refresh chips to show (typing...)
    }
    return;
  }
  if(m.type==='reaction'){applyRx(m.msgId,m.emoji,m.name);return}

  if(m.type==='chat'){
    const msgs=lm();
    if(!msgs.find(x=>x.id===m.id)){msgs.push(m);sm(msgs);appendBub(m,true)}
    return;
  }

  // ── Image chunks ──
  if(m.type==='img_start'){
    imgAssembly[m.imgId]={chunks:[],total:m.total,name:m.name,ts:m.ts,id:m.imgId};
    showImgProgress(m.imgId,m.name,m.ts,0,m.total);
    return;
  }
  if(m.type==='img_chunk'){
    const a=imgAssembly[m.imgId];if(!a)return;
    a.chunks[m.idx]=m.data;
    updateImgProgress(m.imgId,a.chunks.filter(Boolean).length,a.total);
    return;
  }
  if(m.type==='img_end'){
    const a=imgAssembly[m.imgId];if(!a)return;
    const fullData=a.chunks.join('');
    delete imgAssembly[m.imgId];
    const msg={type:'image',id:a.id,name:a.name,data:fullData,ts:a.ts};
    const msgs=lm();msgs.push(msg);sm(msgs);
    finalizeImgBubble(msg);
    return;
  }

  // ── Voice chunks ──
  if(m.type==='voice_start'){
    voiceAssembly[m.voiceId]={chunks:[],total:m.total,name:m.name,ts:m.ts,id:m.voiceId};
    showVoiceProgress(m.voiceId,m.name,m.ts);
    return;
  }
  if(m.type==='voice_chunk'){
    const a=voiceAssembly[m.voiceId];if(!a)return;
    a.chunks[m.idx]=m.data;
    return;
  }
  if(m.type==='voice_end'){
    const a=voiceAssembly[m.voiceId];if(!a)return;
    const fullData=a.chunks.join('');
    delete voiceAssembly[m.voiceId];
    voiceMap[a.id]=fullData;
    finalizeVoiceBubble({id:a.id,name:a.name,ts:a.ts});
    return;
  }
}

// ── Render all from localStorage ──
function renderAll(){
  const box=document.getElementById('msgs');
  box.innerHTML='';msgMap={};
  lm().forEach(m=>appendBub(m,false));
  box.scrollTop=box.scrollHeight;
}

// ── Voice bubble HTML helper ──
function voiceBubHTML(id){
  return '<div class="voiceBub">'+
    '<button class="vPlayBtn" id="vpb_'+id+'" onclick="playVoice(\''+id+'\',event)">&#9654;</button>'+
    '<div class="vBars" id="vw_'+id+'">'+
      '<div class="vBar"></div>'.repeat(7)+
    '</div>'+
    '<span class="vDur" id="vd_'+id+'">0:00</span>'+
  '</div>';
}

// ── Append bubble ──
function appendBub(m,scroll){
  const mine=m.name===myName;
  const col=nc(m.name);
  const w=document.createElement('div');
  w.className='bw '+(mine?'mine':'other')+(scroll?' new':'');
  w.id='imgbub_'+m.id;

  let content='';
  if(m.type==='image'){
    content='<div class="imgWrap"><img src="'+m.data+'" onclick="viewImg(this.src)" loading="lazy"></div>';
  } else if(m.type==='voice'){
    if(m.data)voiceMap[m.id]=m.data; // own voice bubbles pass data directly
    content=voiceBubHTML(m.id);
  } else {
    content=esc(m.text).replace(/\n/g,'<br>');
  }

  w.innerHTML=
    '<div class="meta"><span class="nm" style="color:'+col+'">'+esc(m.name)+'</span></div>'+
    '<div class="bubble" onclick="toggleRx(event,\''+m.id+'\')">'+
      content+
      '<div class="rxPicker" id="rx_'+m.id+'">'+
        RX.map(r=>'<span onclick="sendRxD(event,\''+m.id+'\',\''+r+'\')">'+r+'</span>').join('')+
      '</div>'+
    '</div>'+
    '<div class="rxRow" id="rxrow_'+m.id+'"></div>'+
    '<div class="ts">'+ft(m.ts)+'</div>';

  document.getElementById('msgs').appendChild(w);
  msgMap[m.id]=w;
  if(scroll)w.scrollIntoView({behavior:'smooth',block:'end'});
}

// ── Voice playback ──
function playVoice(id,e){
  if(e)e.stopPropagation();
  const src=voiceMap[id];
  if(!src){return;}

  // If already playing this track — pause it
  if(currentAudio&&currentAudio._id===id&&!currentAudio.paused){
    currentAudio.pause();
    resetVoiceUI(id);
    currentAudio=null;
    return;
  }
  // Stop any other playing audio
  if(currentAudio&&!currentAudio.paused){
    currentAudio.pause();
    if(currentAudio._id)resetVoiceUI(currentAudio._id);
    currentAudio=null;
  }

  const audio=new Audio(src);
  audio._id=id;
  currentAudio=audio;

  const btn=document.getElementById('vpb_'+id);
  const wave=document.getElementById('vw_'+id);
  const dur=document.getElementById('vd_'+id);
  if(btn)btn.innerHTML='&#9646;&#9646;';
  if(wave)wave.classList.add('playing');

  audio.ontimeupdate=()=>{
    if(!dur)return;
    const t=Math.floor(audio.currentTime);
    dur.textContent=Math.floor(t/60)+':'+(t%60).toString().padStart(2,'0');
  };
  audio.onended=()=>{resetVoiceUI(id);currentAudio=null;};
  audio.onerror=()=>{resetVoiceUI(id);currentAudio=null;};
  audio.play().catch(()=>{resetVoiceUI(id);});
}

function resetVoiceUI(id){
  const btn=document.getElementById('vpb_'+id);
  const wave=document.getElementById('vw_'+id);
  const dur=document.getElementById('vd_'+id);
  if(btn)btn.innerHTML='&#9654;';
  if(wave)wave.classList.remove('playing');
  if(dur)dur.textContent='0:00';
}

function appendSys(text){
  const d=document.createElement('div');d.className='sys';d.textContent=text;
  document.getElementById('msgs').appendChild(d);
  d.scrollIntoView({behavior:'smooth',block:'end'});
}

// ── Image progress bubble ──
function showImgProgress(imgId,name,ts,got,total){
  if(name===myName)return;
  const col=nc(name);
  const w=document.createElement('div');
  w.className='bw other new';w.id='imgbub_'+imgId;
  const pct=total>0?Math.round(got/total*100):0;
  w.innerHTML=
    '<div class="meta"><span class="nm" style="color:'+col+'">'+esc(name)+'</span></div>'+
    '<div class="bubble"><div class="imgWrap">'+
      '<div class="imgProg" id="iprog_'+imgId+'">'+
        '<div class="pbar"><div class="pfill" id="pfill_'+imgId+'" style="width:'+pct+'%"></div></div>'+
        '<div class="ptxt" id="ptxt_'+imgId+'">'+pct+'%</div>'+
      '</div></div></div>'+
    '<div class="ts">'+ft(ts)+'</div>';
  document.getElementById('msgs').appendChild(w);
  w.scrollIntoView({behavior:'smooth',block:'end'});
}
function updateImgProgress(imgId,got,total){
  const fill=document.getElementById('pfill_'+imgId);
  const txt=document.getElementById('ptxt_'+imgId);
  if(!fill)return;
  const pct=Math.round(got/total*100);
  fill.style.width=pct+'%';
  if(txt)txt.textContent=pct+'%';
}
function finalizeImgBubble(msg){
  const existing=document.getElementById('imgbub_'+msg.id);
  if(existing){
    const col=nc(msg.name);
    existing.innerHTML=
      '<div class="meta"><span class="nm" style="color:'+col+'">'+esc(msg.name)+'</span></div>'+
      '<div class="bubble"><div class="imgWrap">'+
        '<img src="'+msg.data+'" onclick="viewImg(this.src)" loading="lazy">'+
      '</div></div>'+
      '<div class="ts">'+ft(msg.ts)+'</div>';
  }else{appendBub(msg,true)}
}

// ── Voice progress + finalize ──
function showVoiceProgress(voiceId,name,ts){
  if(name===myName)return;
  const col=nc(name);
  const w=document.createElement('div');
  w.className='bw other new';w.id='imgbub_'+voiceId;
  w.innerHTML=
    '<div class="meta"><span class="nm" style="color:'+col+'">'+esc(name)+'</span></div>'+
    '<div class="bubble">'+
      '<div class="voiceBub">'+
        '<span style="font-size:20px">&#127908;</span>'+
        '<span style="color:var(--text2);font-size:12px;margin-left:6px">Receiving'+
          '<span class="df"><span></span><span></span><span></span></span>'+
        '</span>'+
      '</div>'+
    '</div>'+
    '<div class="ts">'+ft(ts)+'</div>';
  document.getElementById('msgs').appendChild(w);
  w.scrollIntoView({behavior:'smooth',block:'end'});
}
function finalizeVoiceBubble(info){
  const col=nc(info.name);
  const html=
    '<div class="meta"><span class="nm" style="color:'+col+'">'+esc(info.name)+'</span></div>'+
    '<div class="bubble">'+voiceBubHTML(info.id)+'</div>'+
    '<div class="ts">'+ft(info.ts)+'</div>';
  const existing=document.getElementById('imgbub_'+info.id);
  if(existing){
    existing.innerHTML=html;
  }else{
    const w=document.createElement('div');
    w.className='bw other new';w.id='imgbub_'+info.id;
    w.innerHTML=html;
    document.getElementById('msgs').appendChild(w);
    w.scrollIntoView({behavior:'smooth',block:'end'});
  }
}

// ── Reactions ──
function toggleRx(e,id){
  e.stopPropagation();
  const p=document.getElementById('rx_'+id);
  if(openRx&&openRx!==p)openRx.classList.remove('show');
  p.classList.toggle('show');
  openRx=p.classList.contains('show')?p:null;
}
document.addEventListener('click',()=>{if(openRx){openRx.classList.remove('show');openRx=null;}});
function sendRxD(e,msgId,emoji){
  e.stopPropagation();
  if(openRx){openRx.classList.remove('show');openRx=null;}
  ws.send(JSON.stringify({type:'reaction',msgId,emoji,name:myName}));
  applyRx(msgId,emoji,myName);
}
function applyRx(msgId,emoji,from){
  const row=document.getElementById('rxrow_'+msgId);if(!row)return;
  let chip=row.querySelector('[data-e="'+emoji+'"]');
  if(chip){chip.dataset.count=parseInt(chip.dataset.count||1)+1;chip.textContent=emoji+' '+chip.dataset.count;}
  else{const c=document.createElement('div');c.className='rxChip';c.dataset.e=emoji;c.dataset.count=1;c.textContent=emoji;
    c.onclick=()=>{ws.send(JSON.stringify({type:'reaction',msgId,emoji,name:myName}));applyRx(msgId,emoji,myName)};row.appendChild(c);}
}

// ── Online bar — shows (typing...) in chips ──
function renderOnline(users){
  const el=document.getElementById('onList');
  if(!users||!users.length){el.innerHTML='No one online';return;}
  const now=Date.now();
  el.innerHTML=users.map(u=>{
    const isTyp=u!==myName&&typUsers[u]&&(now-typUsers[u]<3000);
    return '<span class="chip'+(u===myName?' me':isTyp?' istyping':'')+'">'+
      esc(u)+(isTyp?'<span class="tyiDots"><span></span><span></span><span></span></span>':'')+
    '</span>';
  }).join(' ');
}

// ── Typing indicator bar ──
function onType(){
  if(ws&&ws.readyState===1)ws.send(JSON.stringify({type:'typing',name:myName}));
  clearTimeout(typTimer);typTimer=setTimeout(()=>{},2000);
}
function renderTyp(){
  const now=Date.now();
  Object.keys(typUsers).forEach(k=>{if(now-typUsers[k]>3000)delete typUsers[k]});
  const others=Object.keys(typUsers).filter(k=>k!==myName);
  const bar=document.getElementById('typBar');
  if(!others.length){bar.innerHTML='';return;}
  bar.innerHTML=esc(others.join(', '))+' is typing <span class="df"><span></span><span></span><span></span></span>';
  setTimeout(renderTyp,1500);
}

// ── Send text ──
function sendMsg(){
  const el=document.getElementById('txtInp');
  const text=el.innerText.trim();
  if(!text||!ws||ws.readyState!==1)return;
  const m={type:'chat',id:uid(),name:myName,text,ts:Date.now()};
  ws.send(JSON.stringify(m));
  const msgs=lm();msgs.push(m);sm(msgs);
  appendBub(m,true);
  el.innerText='';
  // Button press animation
  const btn=document.getElementById('sndBtn');
  btn.style.transform='scale(.82)';
  setTimeout(()=>btn.style.transform='',150);
}

// ── Send image (chunked) ──
function sendImage(input){
  const file=input.files[0];if(!file)return;
  const reader=new FileReader();
  reader.onload=e=>{
    const img=new Image();
    img.onload=()=>{
      const MAX=500;let w=img.width,h=img.height;
      if(w>MAX||h>MAX){if(w>h){h=Math.round(h*MAX/w);w=MAX;}else{w=Math.round(w*MAX/h);h=MAX;}}
      const cv=document.createElement('canvas');cv.width=w;cv.height=h;
      cv.getContext('2d').drawImage(img,0,0,w,h);
      const fullData=cv.toDataURL('image/jpeg',0.65);
      const imgId=uid();const ts=Date.now();
      const m={type:'image',id:imgId,name:myName,data:fullData,ts};
      const msgs=lm();msgs.push(m);sm(msgs);appendBub(m,true);
      const chunks=[];
      for(let i=0;i<fullData.length;i+=CHUNK_SIZE)chunks.push(fullData.slice(i,i+CHUNK_SIZE));
      ws.send(JSON.stringify({type:'img_start',imgId,total:chunks.length,name:myName,ts}));
      let idx=0;
      function sn(){
        if(idx>=chunks.length){ws.send(JSON.stringify({type:'img_end',imgId}));return;}
        ws.send(JSON.stringify({type:'img_chunk',imgId,idx,data:chunks[idx]}));
        idx++;setTimeout(sn,30);
      }
      sn();
    };
    img.src=e.target.result;
  };
  reader.readAsDataURL(file);input.value='';
}

// ── Voice recording ──
async function startVoice(e){
  if(e)e.preventDefault();
  if(isRecording)return;
  if(!window.MediaRecorder||!navigator.mediaDevices){
    alert('Voice messages require Firefox, or Chrome with HTTPS.\nTry: Firefox browser on Android.');
    return;
  }
  try{
    const stream=await navigator.mediaDevices.getUserMedia({audio:true,video:false});
    audioChunks=[];
    // Pick best supported format
    const types=['audio/webm;codecs=opus','audio/webm','audio/ogg;codecs=opus','audio/ogg'];
    const mimeType=types.find(t=>MediaRecorder.isTypeSupported(t))||'';
    const opts=mimeType?{mimeType}:{};
    mediaRecorder=new MediaRecorder(stream,opts);
    mediaRecorder.ondataavailable=ev=>{if(ev.data&&ev.data.size>0)audioChunks.push(ev.data)};
    mediaRecorder.onstop=()=>{
      stream.getTracks().forEach(t=>t.stop());
      if(audioChunks.length)sendVoice(new Blob(audioChunks,{type:mediaRecorder.mimeType||'audio/webm'}));
    };
    mediaRecorder.start(200);
    isRecording=true;
    document.getElementById('micBtn').classList.add('rec');
  }catch(err){
    alert('Mic error: '+err.message+'\n\nNote: Voice needs HTTPS or Firefox.');
  }
}

function stopVoice(){
  if(!isRecording)return;
  isRecording=false;
  document.getElementById('micBtn').classList.remove('rec');
  if(mediaRecorder&&mediaRecorder.state!=='inactive')mediaRecorder.stop();
}

function sendVoice(blob){
  if(!blob||blob.size<500)return; // ignore accidental taps
  const reader=new FileReader();
  reader.onload=e=>{
    const fullData=e.target.result;
    const voiceId=uid();const ts=Date.now();
    // Show own bubble immediately (data in voiceMap)
    voiceMap[voiceId]=fullData;
    appendBub({type:'voice',id:voiceId,name:myName,ts},true);
    // Chunk + send
    const chunks=[];
    for(let i=0;i<fullData.length;i+=CHUNK_SIZE)chunks.push(fullData.slice(i,i+CHUNK_SIZE));
    const total=chunks.length;
    ws.send(JSON.stringify({type:'voice_start',voiceId,total,name:myName,ts}));
    let idx=0;
    function sn(){
      if(idx>=total){ws.send(JSON.stringify({type:'voice_end',voiceId}));return;}
      ws.send(JSON.stringify({type:'voice_chunk',voiceId,idx,data:chunks[idx]}));
      idx++;setTimeout(sn,30);
    }
    sn();
  };
  reader.readAsDataURL(blob);
}

// ── Misc ──
function viewImg(src){document.getElementById('viewImg').src=src;document.getElementById('imgView').classList.add('show');}
function showLogs(){document.getElementById('logModal').classList.add('show');fetchLogs();}
function fetchLogs(){
  fetch('/logs').then(r=>r.text()).then(t=>{
    const el=document.getElementById('logContent');el.textContent=t||'No logs.';el.scrollTop=el.scrollHeight;
  }).catch(()=>{document.getElementById('logContent').textContent='Failed to load logs.';});
}
function clearChat(){
  if(confirm('Clear all messages?')){localStorage.removeItem(SK);document.getElementById('msgs').innerHTML='';msgMap={};}
}
window.onload=()=>document.getElementById('nameInp').focus();
</script>
</body>
</html>
)rawhtml";

// ─── HTTP handlers ────────────────────────────────────────────────────────
void handleRoot()     { httpServer.send_P(200, "text/html", HTML); }
void handleLogs()     { httpServer.send(200, "text/plain", getLogs()); }
// 204 = "No Content" → phone/laptop thinks internet is available
// → captive portal popup DOES NOT appear
// Users manually open: chat.ibmovs.com  OR  192.168.4.1
void handleNotFound() { httpServer.send(204, "text/plain", ""); }

// ─── Setup ────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200); delay(400);
  addLog("=== IBMOVS Chat Boot ===");

  // OLED init
  u8g2.begin();
  oledMain();
  addLog("[OLED] OK");

  // WiFi AP
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_IP, AP_SUBNET);
  WiFi.softAP(AP_SSID, AP_PASS);
  addLog("[WiFi] " + String(AP_SSID) + " @ 192.168.4.1");

  // DNS wildcard → all domains resolve to AP_IP
  // This makes chat.ibmovs.com work on connected devices
  dnsServer.start(53, "*", AP_IP);
  addLog("[DNS] Wildcard (*) → 192.168.4.1  |  chat.ibmovs.com ready");

  // HTTP
  httpServer.on("/", handleRoot);
  httpServer.on("/chat", handleRoot);
  httpServer.on("/logs", handleLogs);
  httpServer.onNotFound(handleNotFound); // 204 = no captive portal popup
  httpServer.begin();
  addLog("[HTTP] Port 80 OK");

  // WebSocket
  wsServer.begin();
  wsServer.onEvent(wsEvent);
  addLog("[WS] Port 81 OK");

  startTime = millis();
  addLog("[BOOT] GO!");
}

// ─── Loop ─────────────────────────────────────────────────────────────────
void loop() {
  dnsServer.processNextRequest();
  httpServer.handleClient();
  wsServer.loop();

  // Refresh OLED every second
  static unsigned long lo = 0;
  if (millis() - lo > 1000) { lo = millis(); oledDraw(); }
}
