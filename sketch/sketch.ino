/*
 ╔══════════════════════════════════════════════════════════╗
 ║         IBMOVS Chat — ESP32-S3-Zero  v3.1  FINAL        ║
 ║  Board : Waveshare ESP32-S3-Zero                         ║
 ║  OLED  : 4-pin I2C SSD1306 128x64                       ║
 ║          SDA=GPIO4  |  SCL=GPIO3  (SW I2C)              ║
 ╚══════════════════════════════════════════════════════════╝

 FIXES in v3.1:
  [1] WiFi.softAP() — NULL password instead of "" (KEY FIX for open AP)
  [2] WiFi.mode(WIFI_OFF) then WIFI_AP — clean state reset
  [3] WiFi.setSleep(false) — prevents AP radio from sleeping
  [4] WiFi.setTxPower(WIFI_POWER_19_5dBm) — max TX for stable AP
  [5] 500ms delay after softAP() — allow full init
  [6] Removed Wire.begin() — conflicts with SW_I2C u8g2
  [7] wsServer.setAutoPing() REMOVED — not in WebSockets 2.7.x
  [8] AP retry logic if first attempt fails
*/

#include <Arduino.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <DNSServer.h>
#include <ArduinoJson.h>

// ─── OLED SW_I2C: SCL=GPIO3, SDA=GPIO4 ──────────────────────────────────────
U8G2_SSD1306_128X64_NONAME_F_SW_I2C u8g2(U8G2_R0, /*clk=*/3, /*data=*/4, U8X8_PIN_NONE);

// ─── Network ─────────────────────────────────────────────────────────────────
const char*     AP_SSID = "IBMOVS-Chat";
const char*     AP_PASS = NULL;   // NULL = open, "" can silently fail on ESP32-S3
const IPAddress AP_IP(192, 168, 4, 1);
const IPAddress AP_GW(192, 168, 4, 1);
const IPAddress AP_SUBNET(255, 255, 255, 0);

WebServer        httpServer(80);
WebSocketsServer wsServer(81);
DNSServer        dnsServer;

// ─── State ───────────────────────────────────────────────────────────────────
unsigned long startTime = 0;
int  totalMsgs = 0;
bool clients[10];
String clientNames[10];

// ─── Log buffer ──────────────────────────────────────────────────────────────
#define MAX_LOGS 50
String logBuf[MAX_LOGS];
int logHead = 0, logCount = 0;

void addLog(const String& msg) {
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

// ─── OLED screens ────────────────────────────────────────────────────────────
#define SCR_IDLE 0
#define SCR_MAIN 1
#define SCR_ACT  2
#define SCR_TIME 3

int           oledScr   = SCR_IDLE;
unsigned long oledNext  = 0;
String        actSndr   = "";
String        actMsg    = "";
bool          actNew    = false;
bool          wiping    = false;
int           wipeX     = 128;
unsigned long wipeStart = 0;
int           nextScr   = SCR_IDLE;

String trOled(const String& s, int n) {
  if ((int)s.length() <= n) return s;
  return s.substring(0, n - 2) + "..";
}

void oledIdle() {
  u8g2.clearBuffer();
  u8g2.drawFrame(0, 0, 128, 64);
  u8g2.setFont(u8g2_font_6x10_tf);
  int tw = u8g2.getStrWidth("IBMOVS");
  u8g2.drawStr((128 - tw) / 2, 13, "IBMOVS");
  u8g2.setFont(u8g2_font_5x7_tf);
  tw = u8g2.getStrWidth("CHAT READY");
  u8g2.drawStr((128 - tw) / 2, 22, "CHAT READY");
  u8g2.drawHLine(4, 26, 120);
  u8g2.drawStr(4, 36, "WiFi: IBMOVS-Chat");
  u8g2.drawStr(4, 46, "IP  : 192.168.4.1");
  if ((millis() / 600) % 2 == 0)
    u8g2.drawStr(4, 57, "> chat.ibmovs");
  else
    u8g2.drawStr(4, 57, "  chat.ibmovs <");
  u8g2.sendBuffer();
}

void oledMain() {
  int c = WiFi.softAPgetStationNum();
  char cu[22], cm[22];
  sprintf(cu, "Users : %d", c);
  sprintf(cm, "Msgs  : %d", totalMsgs);
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  int tw = u8g2.getStrWidth("IBMOVS CHAT");
  u8g2.drawStr((128 - tw) / 2, 10, "IBMOVS CHAT");
  u8g2.drawHLine(0, 12, 128);
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(0, 23, "WiFi : IBMOVS-Chat");
  u8g2.drawStr(0, 32, "IP   : 192.168.4.1");
  u8g2.drawStr(0, 41, cu);
  u8g2.drawStr(0, 50, cm);
  u8g2.drawHLine(0, 53, 128);
  u8g2.setFont(u8g2_font_4x6_tf);
  u8g2.drawStr(0, 62, "chat.ibmovs | 192.168.4.1");
  u8g2.sendBuffer();
}

void oledActivity() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 10, "LAST MSG");
  u8g2.drawHLine(0, 12, 128);
  u8g2.setFont(u8g2_font_5x7_tf);
  if (actSndr.length() == 0) {
    u8g2.drawStr(8, 36, "No messages yet..");
  } else {
    u8g2.drawStr(0, 24, trOled(actSndr, 21).c_str());
    String m1 = trOled(actMsg, 21);
    u8g2.drawStr(0, 34, m1.c_str());
    if ((int)actMsg.length() > 21)
      u8g2.drawStr(0, 44, trOled(actMsg.substring(21), 21).c_str());
  }
  int c = WiFi.softAPgetStationNum();
  char s[28]; sprintf(s, "%d online | %d msgs", c, totalMsgs);
  u8g2.drawHLine(0, 53, 128);
  u8g2.setFont(u8g2_font_4x6_tf);
  u8g2.drawStr(0, 62, s);
  u8g2.sendBuffer();
}

void oledUptime() {
  unsigned long e = (millis() - startTime) / 1000;
  char tb[12];
  sprintf(tb, "%02lu:%02lu:%02lu", e / 3600, (e / 60) % 60, e % 60);
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
  char s[28]; sprintf(s, "Users:%d  Msgs:%d", c, totalMsgs);
  u8g2.setFont(u8g2_font_4x6_tf);
  tw = u8g2.getStrWidth(s);
  u8g2.drawStr((128 - tw) / 2, 58, s);
  u8g2.sendBuffer();
}

void drawScreen(int scr) {
  switch (scr) {
    case SCR_IDLE: oledIdle();     break;
    case SCR_MAIN: oledMain();     break;
    case SCR_ACT:  oledActivity(); break;
    case SCR_TIME: oledUptime();   break;
  }
}

void startWipe(int to) {
  nextScr = to; wiping = true; wipeX = 128; wipeStart = millis();
}

void oledDraw() {
  unsigned long now = millis();
  int stations = WiFi.softAPgetStationNum();

  if (wiping) {
    unsigned long elapsed = now - wipeStart;
    wipeX = 128 - (int)(elapsed * 128 / 250);
    if (wipeX <= 0) {
      wiping = false; oledScr = nextScr; drawScreen(oledScr); return;
    }
    u8g2.clearBuffer();
    drawScreen(nextScr);
    u8g2.setDrawColor(0);
    for (int x = 0; x < wipeX; x++) u8g2.drawVLine(x, 0, 64);
    u8g2.setDrawColor(1);
    u8g2.sendBuffer();
    return;
  }

  if (actNew) {
    actNew = false; oledNext = now + 5000; startWipe(SCR_ACT); return;
  }

  if (now >= oledNext) {
    int nextS;
    if (stations == 0) {
      nextS = (oledScr == SCR_IDLE) ? SCR_TIME : SCR_IDLE;
      oledNext = now + ((nextS == SCR_TIME) ? 3000 : 5000);
    } else {
      if      (oledScr == SCR_MAIN) nextS = SCR_ACT;
      else if (oledScr == SCR_ACT)  nextS = SCR_TIME;
      else                          nextS = SCR_MAIN;
      oledNext = now + ((nextS == SCR_TIME) ? 3000 : 4000);
    }
    startWipe(nextS); return;
  }

  if (oledScr == SCR_TIME || oledScr == SCR_IDLE) drawScreen(oledScr);
}

// ─── Broadcast ───────────────────────────────────────────────────────────────
void broadcastExcept(uint8_t skip, const String& p) {
  for (int i = 0; i < 10; i++)
    if (clients[i] && i != skip) wsServer.sendTXT(i, p);
}
void broadcastAll(const String& p) {
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

// ─── WebSocket ───────────────────────────────────────────────────────────────
void wsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {

    case WStype_CONNECTED:
      clients[num] = true; clientNames[num] = "";
      addLog("[WS] #" + String(num) + " connected");
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
      StaticJsonDocument<256> hdr;
      DeserializationError err = deserializeJson(hdr, msg.substring(0, min((int)msg.length(), 200)));
      if (!err) {
        String t = hdr["type"] | "";
        if (t == "img_start" || t == "img_chunk" || t == "img_end") {
          broadcastExcept(num, msg);
          if (t == "img_end") { totalMsgs++; actSndr = clientNames[num]; actMsg = "[Image]"; actNew = true; addLog("[IMG] " + clientNames[num]); }
          return;
        }
        if (t == "voice_start" || t == "voice_chunk" || t == "voice_end") {
          broadcastExcept(num, msg);
          if (t == "voice_end") { totalMsgs++; actSndr = clientNames[num]; actMsg = "[Voice]"; actNew = true; addLog("[VOICE] " + clientNames[num]); }
          return;
        }
        if (t == "typing")   { broadcastExcept(num, msg); return; }
        if (t == "reaction") { broadcastAll(msg); return; }
        if (t == "read")     { broadcastAll(msg); return; }
        if (t == "pin")      { broadcastAll(msg); return; }
        if (t == "delete")   { broadcastAll(msg); return; }
      }
      StaticJsonDocument<1024> doc;
      if (deserializeJson(doc, msg)) { addLog("[WS] JSON err"); return; }
      String msgType = doc["type"] | "";
      if (msgType == "join") {
        String name = doc["name"] | "Unknown";
        clientNames[num] = name;
        addLog("[JOIN] " + name);
        StaticJsonDocument<128> sys;
        sys["type"] = "system"; sys["text"] = name + " joined the chat";
        String so; serializeJson(sys, so); broadcastExcept(num, so);
        broadcastAll(buildOnlineList());
        StaticJsonDocument<64> ack; ack["type"] = "joined";
        String ao; serializeJson(ack, ao); wsServer.sendTXT(num, ao);
        if (WiFi.softAPgetStationNum() > 0 && oledScr == SCR_IDLE) startWipe(SCR_MAIN);
        return;
      }
      if (msgType == "chat") {
        totalMsgs++;
        String txt = doc["text"] | "";
        addLog("[MSG] " + clientNames[num] + ": " + txt);
        actSndr = clientNames[num]; actMsg = txt; actNew = true;
        broadcastAll(msg); return;
      }
      break;
    }
    default: break;
  }
}

// ─── HTML ─────────────────────────────────────────────────────────────────────
const char HTML[] PROGMEM = R"rawhtml(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<title>IBMOVS Chat</title>
<style>
*{box-sizing:border-box;margin:0;padding:0;-webkit-tap-highlight-color:transparent}
:root{--bg:#07070f;--bg2:#0d0d1a;--bg3:#13132a;--bg4:#1a1a35;--accent:#5b9cf6;--accent2:#7c6cf8;--green:#27c97a;--red:#e74c3c;--text:#e4e4f0;--text2:#8888aa;--border:#252540;--r:18px;--glow:#5b9cf633;--shadow:0 4px 24px #0009}
[data-theme=light]{--bg:#f0f2fc;--bg2:#e8eaf8;--bg3:#dde0f5;--bg4:#d0d4ee;--text:#1a1a35;--text2:#5a5a7a;--border:#c0c5e0;--glow:#5b9cf622;--shadow:0 4px 24px #0002}
html,body{height:100%;height:100dvh;overflow:hidden;background:var(--bg);color:var(--text);font-family:'Segoe UI',system-ui,sans-serif;transition:background .3s,color .3s}
body::before{content:'';position:fixed;inset:0;background-image:linear-gradient(var(--border) 1px,transparent 1px),linear-gradient(90deg,var(--border) 1px,transparent 1px);background-size:32px 32px;opacity:.3;pointer-events:none;z-index:0}
#app{display:flex;flex-direction:column;height:100%;height:100dvh;position:relative;z-index:1}
#hdr{background:linear-gradient(120deg,var(--bg4),var(--bg2) 60%,var(--bg4));background-size:300% 100%;animation:hShimmer 8s linear infinite;padding:10px 14px;display:flex;align-items:center;gap:10px;border-bottom:1px solid var(--border);flex-shrink:0;box-shadow:0 2px 20px #0006}
@keyframes hShimmer{0%{background-position:0%}100%{background-position:300%}}
.av{width:40px;height:40px;border-radius:50%;background:linear-gradient(135deg,var(--accent),var(--accent2));display:flex;align-items:center;justify-content:center;font-weight:800;font-size:15px;flex-shrink:0;color:#fff;animation:avGlow 3s ease-in-out infinite}
@keyframes avGlow{0%,100%{box-shadow:0 0 0 0 var(--glow)}50%{box-shadow:0 0 0 10px transparent}}
.info{flex:1;min-width:0}
.info h1{font-size:15px;font-weight:700;background:linear-gradient(90deg,var(--accent),var(--accent2));-webkit-background-clip:text;-webkit-text-fill-color:transparent}
.info .sub{font-size:11px;color:var(--text2)}
.hbtns{display:flex;gap:6px}
.hbtn{background:var(--bg3);border:1px solid var(--border);color:var(--text);border-radius:10px;padding:5px 9px;font-size:12px;cursor:pointer;transition:all .15s}
.hbtn:active{transform:scale(.88)}
#connBar{height:3px;background:var(--bg2);flex-shrink:0}
#connFill{height:100%;width:0%;background:linear-gradient(90deg,var(--green),var(--accent));transition:width .5s,background .5s}
#onBar{background:var(--bg2);padding:5px 14px;font-size:11px;border-bottom:1px solid var(--border);flex-shrink:0;display:flex;align-items:center;gap:6px;overflow-x:auto;white-space:nowrap}
#onBar::-webkit-scrollbar{display:none}
.dot{width:7px;height:7px;background:var(--green);border-radius:50%;flex-shrink:0;animation:dotGlow 2s ease-in-out infinite}
@keyframes dotGlow{0%,100%{box-shadow:0 0 4px var(--green)}50%{box-shadow:0 0 16px var(--green)}}
.chip{background:var(--bg3);border:1px solid var(--border);border-radius:20px;padding:2px 9px;font-size:10px;color:var(--text2);flex-shrink:0;transition:all .25s}
.chip.me{border-color:var(--accent);color:var(--accent)}
.chip.istyping{border-color:#ffd43b88;color:#ffd43b;background:#ffd43b11}
.tyiDots{display:inline-flex;gap:2px;vertical-align:middle;margin-left:3px}
.tyiDots span{width:3px;height:3px;background:#ffd43b;border-radius:50%;animation:tdf .7s infinite}
.tyiDots span:nth-child(2){animation-delay:.15s}.tyiDots span:nth-child(3){animation-delay:.3s}
@keyframes tdf{0%,80%,100%{opacity:.2}40%{opacity:1}}
#srchBar{display:none;padding:5px 14px;background:var(--bg2);border-bottom:1px solid var(--border);flex-shrink:0;align-items:center;gap:8px}
#srchBar.show{display:flex}
#srchInp{flex:1;background:var(--bg3);border:1px solid var(--border);border-radius:20px;padding:6px 14px;color:var(--text);font-size:13px;outline:none}
#srchInp:focus{border-color:var(--accent)}
#srchClose{background:none;border:none;color:var(--text2);font-size:18px;cursor:pointer}
#pinnedBanner{display:none;padding:5px 14px;background:linear-gradient(90deg,var(--bg4),var(--bg2));border-bottom:1px solid var(--border);font-size:11px;color:var(--text2);cursor:pointer;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;flex-shrink:0}
#pinnedBanner.show{display:block}
#pinnedBanner span.lbl{color:var(--accent)}
#msgs{flex:1;overflow-y:auto;padding:12px 14px 8px;display:flex;flex-direction:column;gap:6px;scroll-behavior:smooth}
#msgs::-webkit-scrollbar{width:3px}
#msgs::-webkit-scrollbar-thumb{background:var(--border);border-radius:2px}
.bw{display:flex;flex-direction:column;max-width:78%}
.bw.mine{align-self:flex-end;align-items:flex-end}
.bw.other{align-self:flex-start;align-items:flex-start}
.bw.new{animation:bubIn .3s cubic-bezier(.34,1.56,.64,1) both}
@keyframes bubIn{from{opacity:0;transform:translateY(14px) scale(.9)}to{opacity:1;transform:none}}
.bw.hidden{display:none}
.meta{font-size:10px;color:var(--text2);margin-bottom:2px;padding:0 4px}
.nm{font-weight:600}
.replyQuote{background:var(--bg4);border-left:3px solid var(--accent);border-radius:8px;padding:4px 8px;font-size:11px;color:var(--text2);margin-bottom:4px;cursor:pointer;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.replyQuote strong{color:var(--accent);display:block;font-size:10px}
.bubble{padding:9px 13px;border-radius:var(--r);font-size:14px;line-height:1.45;word-break:break-word;position:relative;cursor:pointer;transition:opacity .12s,transform .12s}
.bubble:active{opacity:.8;transform:scale(.97)}
.mine .bubble{background:linear-gradient(135deg,var(--accent),var(--accent2));border-bottom-right-radius:4px;color:#fff;box-shadow:0 3px 14px var(--glow)}
.other .bubble{background:var(--bg3);border-bottom-left-radius:4px;border:1px solid var(--border);box-shadow:var(--shadow)}
.bubble.pinned-bub::after{content:'📌';position:absolute;top:-6px;right:-4px;font-size:12px}
.imgWrap{position:relative;min-width:80px;min-height:60px}
.imgWrap img{max-width:100%;max-height:220px;border-radius:10px;display:block;cursor:zoom-in}
.imgProg{position:absolute;inset:0;background:#0009;display:flex;flex-direction:column;align-items:center;justify-content:center;border-radius:10px;gap:8px}
.imgProg .pbar{width:80%;height:5px;background:#fff3;border-radius:3px;overflow:hidden}
.imgProg .pfill{height:100%;background:var(--accent);transition:width .2s;border-radius:3px}
.imgProg .ptxt{font-size:12px;color:#fff;font-weight:600}
.voiceBub{display:flex;align-items:center;gap:8px;min-width:155px;padding:2px 0}
.vPlayBtn{width:34px;height:34px;border-radius:50%;border:none;cursor:pointer;display:flex;align-items:center;justify-content:center;font-size:14px;flex-shrink:0;transition:transform .15s;background:rgba(255,255,255,.25);color:#fff}
.other .vPlayBtn{background:var(--accent);color:#fff}
.vPlayBtn:active{transform:scale(.8)}
.vBars{display:flex;align-items:center;gap:2px;flex:1;height:26px}
.vBar{width:3px;border-radius:2px;background:rgba(255,255,255,.45)}
.other .vBar{background:var(--accent)}
.vBar:nth-child(1){height:6px}.vBar:nth-child(2){height:13px}.vBar:nth-child(3){height:20px}.vBar:nth-child(4){height:16px}.vBar:nth-child(5){height:10px}.vBar:nth-child(6){height:18px}.vBar:nth-child(7){height:8px}.vBar:nth-child(8){height:14px}
.vBars.playing .vBar{animation:vbW .5s ease-in-out infinite}
.vBars.playing .vBar:nth-child(1){animation-delay:0s}.vBars.playing .vBar:nth-child(2){animation-delay:.06s}.vBars.playing .vBar:nth-child(3){animation-delay:.12s}.vBars.playing .vBar:nth-child(4){animation-delay:.18s}.vBars.playing .vBar:nth-child(5){animation-delay:.24s}.vBars.playing .vBar:nth-child(6){animation-delay:.30s}.vBars.playing .vBar:nth-child(7){animation-delay:.36s}.vBars.playing .vBar:nth-child(8){animation-delay:.42s}
@keyframes vbW{0%,100%{height:4px}50%{height:22px}}
.vDur{font-size:11px;opacity:.75;flex-shrink:0;font-variant-numeric:tabular-nums;min-width:30px}
.rr{font-size:9px;opacity:.5;margin-top:1px;padding:0 4px;color:var(--accent)}
.ts{font-size:10px;opacity:.45;margin-top:2px;padding:0 4px}
.sys{align-self:center;background:var(--bg2);border:1px solid var(--border);border-radius:20px;padding:3px 14px;font-size:11px;color:var(--text2);margin:3px 0;animation:sysIn .35s ease}
@keyframes sysIn{from{opacity:0;transform:scale(.86)}to{opacity:1;transform:none}}
.rxPicker{display:none;position:absolute;bottom:calc(100% + 6px);left:0;background:var(--bg3);border:1px solid var(--border);border-radius:14px;padding:7px 9px;gap:6px;flex-wrap:wrap;width:210px;z-index:10;box-shadow:var(--shadow);animation:rxIn .18s ease}
@keyframes rxIn{from{opacity:0;transform:scale(.85)}to{opacity:1;transform:none}}
.rxPicker.show{display:flex}
.rxPicker span{font-size:22px;cursor:pointer;transition:transform .12s;display:inline-block}
.rxPicker span:hover{transform:scale(1.3)}
.rxRow{display:flex;gap:3px;margin-top:3px;flex-wrap:wrap}
.rxChip{background:var(--bg3);border:1px solid var(--border);border-radius:20px;padding:2px 8px;font-size:13px;cursor:pointer}
#ctxMenu{display:none;position:fixed;background:var(--bg3);border:1px solid var(--border);border-radius:14px;padding:6px 0;z-index:50;box-shadow:var(--shadow);min-width:160px;animation:ctxIn .15s ease}
@keyframes ctxIn{from{opacity:0;transform:scale(.9)}to{opacity:1;transform:none}}
#ctxMenu.show{display:block}
.ctxItem{padding:9px 16px;font-size:13px;cursor:pointer;display:flex;align-items:center;gap:8px;color:var(--text);transition:background .1s}
.ctxItem:hover{background:var(--bg4)}
.ctxItem.danger{color:var(--red)}
#typBar{padding:3px 16px;min-height:20px;font-size:11px;color:var(--text2);flex-shrink:0;font-style:italic}
.df{display:inline-flex;gap:3px;vertical-align:middle}
.df span{width:4px;height:4px;background:var(--text2);border-radius:50%;animation:df .9s infinite}
.df span:nth-child(2){animation-delay:.2s}.df span:nth-child(3){animation-delay:.4s}
@keyframes df{0%,80%,100%{opacity:.2}40%{opacity:1}}
#inp{padding:8px 12px;background:var(--bg2);border-top:1px solid var(--border);display:flex;flex-direction:column;gap:6px;flex-shrink:0}
#replyPreview{display:none;background:var(--bg4);border-left:3px solid var(--accent);border-radius:8px;padding:5px 10px;font-size:11px;color:var(--text2);align-items:center;gap:8px}
#replyPreview.show{display:flex}
#replyPreview strong{color:var(--accent)}
#rpClose{background:none;border:none;color:var(--text2);font-size:16px;cursor:pointer;margin-left:auto}
#inpRow{display:flex;gap:8px;align-items:flex-end}
#txtInp{flex:1;background:var(--bg3);border:1px solid var(--border);border-radius:22px;padding:10px 16px;color:var(--text);font-size:14px;outline:none;max-height:90px;min-height:40px;overflow-y:auto;line-height:1.4;transition:border-color .2s,box-shadow .2s}
#txtInp:focus{border-color:var(--accent);box-shadow:0 0 0 3px var(--glow)}
#txtInp:empty:before{content:attr(placeholder);color:var(--text2);pointer-events:none}
.ibtn{width:40px;height:40px;border-radius:50%;border:none;cursor:pointer;display:flex;align-items:center;justify-content:center;font-size:17px;flex-shrink:0;transition:all .18s}
.ibtn:active{transform:scale(.8)}
#imgBtn,#micBtn,#emoBtn{background:var(--bg3);border:1px solid var(--border);color:var(--text2)}
#micBtn.rec{background:var(--red)!important;border-color:var(--red)!important;color:#fff!important;animation:recPulse .7s ease-in-out infinite}
@keyframes recPulse{0%,100%{box-shadow:0 0 0 0 #e74c3c44}50%{box-shadow:0 0 0 10px transparent}}
#sndBtn{background:linear-gradient(135deg,var(--accent),var(--accent2));color:#fff;box-shadow:0 2px 14px var(--glow)}
#emoPicker{display:none;padding:8px 14px;background:var(--bg2);border-top:1px solid var(--border);flex-shrink:0;grid-template-columns:repeat(auto-fill,minmax(34px,1fr));gap:4px}
#emoPicker.show{display:grid}
.emoKey{font-size:20px;cursor:pointer;text-align:center;padding:4px;border-radius:8px;transition:transform .1s;line-height:1}
.emoKey:hover{transform:scale(1.25);background:var(--bg3)}
#fabDown{display:none;position:fixed;bottom:88px;right:14px;width:40px;height:40px;border-radius:50%;background:linear-gradient(135deg,var(--accent),var(--accent2));border:none;color:#fff;font-size:20px;cursor:pointer;box-shadow:var(--shadow);align-items:center;justify-content:center;z-index:30}
#fabDown.show{display:flex}
.modal{display:none;position:fixed;inset:0;background:#0009;z-index:200;align-items:center;justify-content:center;backdrop-filter:blur(4px)}
.modal.show{display:flex}
.mbox{background:var(--bg3);border:1px solid var(--border);border-radius:22px;padding:28px 24px;width:min(340px,92vw);text-align:center;animation:mboxIn .28s cubic-bezier(.34,1.56,.64,1) both;box-shadow:var(--shadow)}
@keyframes mboxIn{from{opacity:0;transform:scale(.78) translateY(24px)}to{opacity:1;transform:none}}
.mbox h2{margin-bottom:6px;font-size:20px;background:linear-gradient(90deg,var(--accent),var(--accent2));-webkit-background-clip:text;-webkit-text-fill-color:transparent}
.mbox p{color:var(--text2);font-size:12px;margin-bottom:18px;line-height:1.6}
.mbox input{width:100%;background:var(--bg);border:1px solid var(--border);border-radius:12px;padding:12px 16px;color:var(--text);font-size:15px;outline:none;margin-bottom:12px;transition:border-color .2s,box-shadow .2s}
.mbox input:focus{border-color:var(--accent);box-shadow:0 0 0 3px var(--glow)}
.pri{width:100%;background:linear-gradient(135deg,var(--accent),var(--accent2));border:none;border-radius:12px;padding:13px;color:#fff;font-size:15px;font-weight:700;cursor:pointer}
.pri:active{opacity:.8}
#logBox{background:var(--bg);border:1px solid var(--border);border-radius:18px;padding:16px;width:min(420px,96vw);max-height:72vh;display:flex;flex-direction:column;gap:8px;box-shadow:var(--shadow)}
#logBox h3{color:var(--accent);font-size:14px}
#logContent{flex:1;overflow-y:auto;font-family:monospace;font-size:11px;color:#7f8;background:#020202;border-radius:8px;padding:10px;white-space:pre-wrap;word-break:break-all;min-height:80px}
#logBox .lbtn{background:var(--bg3);border:1px solid var(--border);color:var(--text);border-radius:8px;padding:8px;cursor:pointer;font-size:13px}
#imgView{display:none;position:fixed;inset:0;background:#000d;z-index:300;align-items:center;justify-content:center;cursor:zoom-out;backdrop-filter:blur(8px)}
#imgView.show{display:flex}
#imgView img{max-width:96vw;max-height:92vh;border-radius:14px;animation:imgVIn .2s ease}
@keyframes imgVIn{from{opacity:0;transform:scale(.85)}to{opacity:1;transform:none}}
#imgView .cls{position:absolute;top:18px;right:22px;color:#fff;font-size:30px;cursor:pointer;background:#0006;border-radius:50%;width:36px;height:36px;display:flex;align-items:center;justify-content:center}
#toast{position:fixed;bottom:92px;left:50%;transform:translateX(-50%) translateY(20px);background:var(--bg3);border:1px solid var(--border);border-radius:20px;padding:7px 18px;font-size:12px;color:var(--text);z-index:400;opacity:0;pointer-events:none;transition:all .3s;white-space:nowrap;box-shadow:var(--shadow)}
#toast.show{opacity:1;transform:translateX(-50%) translateY(0)}
</style>
</head>
<body>
<div id="app">
  <div id="hdr">
    <div class="av">IB</div>
    <div class="info"><h1>IBMOVS Chat</h1><div class="sub" id="subTxt">Connecting...</div></div>
    <div class="hbtns">
      <button class="hbtn" onclick="toggleSearch()">🔍</button>
      <button class="hbtn" onclick="toggleTheme()">🌓</button>
      <button class="hbtn" onclick="showLogs()">📋</button>
      <button class="hbtn" onclick="clearChat()">🗑</button>
    </div>
  </div>
  <div id="connBar"><div id="connFill"></div></div>
  <div id="onBar"><div class="dot"></div><span id="onList">No one online</span></div>
  <div id="pinnedBanner" onclick="scrollToPin()"><span class="lbl">📌 Pinned:</span> <span id="pinnedText"></span></div>
  <div id="srchBar">
    <input id="srchInp" placeholder="Search messages..." oninput="searchMsgs(this.value)">
    <button id="srchClose" onclick="toggleSearch()">✕</button>
  </div>
  <div id="msgs"></div>
  <div id="typBar"></div>
  <div id="inp">
    <div id="replyPreview">
      <div><strong id="rpName"></strong><span id="rpText" style="display:block;opacity:.7"></span></div>
      <button id="rpClose" onclick="clearReply()">✕</button>
    </div>
    <div id="inpRow">
      <button class="ibtn" id="emoBtn" onclick="toggleEmo()">😊</button>
      <button class="ibtn" id="imgBtn" onclick="document.getElementById('imgInp').click()">📷</button>
      <input type="file" id="imgInp" accept="image/*" style="display:none" onchange="sendImage(this)">
      <button class="ibtn" id="micBtn" onpointerdown="startVoice(event)" onpointerup="stopVoice()" onpointerleave="stopVoice()">🎙</button>
      <div id="txtInp" contenteditable="true" placeholder="Message..." oninput="onType()" onkeydown="if(event.key==='Enter'&&!event.shiftKey){event.preventDefault();sendMsg()}"></div>
      <button class="ibtn" id="sndBtn" onclick="sendMsg()">➤</button>
    </div>
    <div id="emoPicker"></div>
  </div>
</div>
<button id="fabDown" onclick="scrollToBottom()">↓</button>
<div class="modal show" id="nameModal">
  <div class="mbox">
    <div style="font-size:52px;margin-bottom:10px">💬</div>
    <h2>IBMOVS Chat</h2>
    <p>Hosted on ESP32-S3-Zero<br>Local WiFi • No internet needed<br>
    <span style="color:var(--accent);font-size:11px">chat.ibmovs &bull; 192.168.4.1</span></p>
    <input id="nameInp" type="text" placeholder="Your name..." maxlength="20" autocomplete="off" onkeydown="if(event.key==='Enter')join()">
    <button class="pri" onclick="join()">Join Chat 🚀</button>
  </div>
</div>
<div class="modal" id="logModal">
  <div id="logBox">
    <h3>📋 ESP32 Logs</h3>
    <div id="logContent">Loading...</div>
    <button class="lbtn" onclick="fetchLogs()">🔄 Refresh</button>
    <button class="lbtn" onclick="document.getElementById('logModal').classList.remove('show')">Close</button>
  </div>
</div>
<div id="imgView" onclick="this.classList.remove('show')">
  <div class="cls">✕</div><img id="viewImg" src="">
</div>
<div id="ctxMenu">
  <div class="ctxItem" onclick="ctxReply()">↩ Reply</div>
  <div class="ctxItem" onclick="ctxPin()">📌 Pin</div>
  <div class="ctxItem" onclick="ctxCopy()">📋 Copy</div>
  <div class="ctxItem danger" onclick="ctxDelete()">🗑 Delete</div>
</div>
<div id="toast"></div>
<script>
const SK='ibmovs_v6',CHUNK_SIZE=3000;
const RX=['👍','❤️','😂','😮','😢','🔥','👏','💯','🎉','👀'];
const COLS=['#5b9cf6','#ff6b6b','#51cf66','#ffd43b','#cc5de8','#ff922b','#20c997','#f06595','#74c0fc','#ff8787'];
const EMOS=['😀','😂','😍','🥺','😎','🤔','😅','🙏','🔥','❤️','👍','✅','⚡','🎉','💡','🌟','😢','😡','🤯','💀','🥳','👋','✨','🫡','💬','🎙','📷','🎵','💪','👀'];
let ws,myName,typUsers={},typTimer,msgMap={},imgAssembly={},voiceAssembly={},voiceMap={};
let onlineUsers=[],mediaRecorder=null,audioChunks=[],isRecording=false,currentAudio=null;
let openRx=null,replyTo=null,pinnedMsgId=null,ctxMsgId='',ctxMsgText='',searchActive=false;
function nc(n){let h=0;for(let c of n)h=(h*31+c.charCodeAt(0))&0xffff;return COLS[h%COLS.length]}
function ft(t){const d=new Date(t);return d.getHours().toString().padStart(2,'0')+':'+d.getMinutes().toString().padStart(2,'0')}
function esc(t){return String(t).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;')}
function uid(){return Math.random().toString(36).slice(2,10)}
function lm(){try{return JSON.parse(localStorage.getItem(SK)||'[]')}catch{return[]}}
function sm(m){if(m.length>300)m=m.slice(-300);localStorage.setItem(SK,JSON.stringify(m))}
function toast(msg,ms=2200){const t=document.getElementById('toast');t.textContent=msg;t.classList.add('show');clearTimeout(t._t);t._t=setTimeout(()=>t.classList.remove('show'),ms)}
function haptic(){if(navigator.vibrate)navigator.vibrate(30)}
function toggleTheme(){const t=document.documentElement.dataset.theme;document.documentElement.dataset.theme=t==='light'?'':'light';localStorage.setItem('ibmTheme',document.documentElement.dataset.theme)}
(()=>{const t=localStorage.getItem('ibmTheme');if(t)document.documentElement.dataset.theme=t})();
(()=>{const p=document.getElementById('emoPicker');EMOS.forEach(e=>{const b=document.createElement('div');b.className='emoKey';b.textContent=e;b.onclick=()=>{const inp=document.getElementById('txtInp');inp.focus();document.execCommand('insertText',false,e)};p.appendChild(b)})})();
function toggleEmo(){document.getElementById('emoPicker').classList.toggle('show')}
function toggleSearch(){searchActive=!searchActive;document.getElementById('srchBar').classList.toggle('show',searchActive);if(searchActive)document.getElementById('srchInp').focus();else{document.getElementById('srchInp').value='';searchMsgs('')}}
function searchMsgs(q){const qq=q.toLowerCase().trim();document.querySelectorAll('#msgs .bw').forEach(el=>{if(!qq){el.classList.remove('hidden');return}el.classList.toggle('hidden',!(el.textContent||'').toLowerCase().includes(qq))})}
document.getElementById('msgs').addEventListener('scroll',function(){const a=this.scrollHeight-this.scrollTop-this.clientHeight<100;document.getElementById('fabDown').classList.toggle('show',!a)});
function scrollToBottom(){const m=document.getElementById('msgs');m.scrollTo({top:m.scrollHeight,behavior:'smooth'})}
function setConn(pct,col){const f=document.getElementById('connFill');f.style.width=pct+'%';if(col)f.style.background=col}
function join(){const n=document.getElementById('nameInp').value.trim();if(!n)return document.getElementById('nameInp').focus();myName=n;document.getElementById('nameModal').classList.remove('show');connectWS();if(!window.MediaRecorder||!navigator.mediaDevices){const b=document.getElementById('micBtn');b.style.opacity='.35';b.title='Voice needs Firefox or HTTPS'}}
function connectWS(){document.getElementById('subTxt').textContent='Connecting...';setConn(20,'#ffd43b');ws=new WebSocket('ws://192.168.4.1:81');ws.onopen=()=>{document.getElementById('subTxt').textContent='ESP32-S3 \u2022 Connected \uD83D\uDFE2';setConn(100,'#27c97a');ws.send(JSON.stringify({type:'join',name:myName}));renderAll();document.getElementById('txtInp').focus()};ws.onclose=()=>{document.getElementById('subTxt').textContent='Reconnecting...';setConn(0,'#e74c3c');setTimeout(connectWS,2000)};ws.onmessage=e=>{try{handle(JSON.parse(e.data))}catch(err){console.error(err)}};ws.onerror=e=>console.error('[WS]',e)}
function handle(m){
  if(m.type==='online'){onlineUsers=m.users||[];renderOnline(onlineUsers);return}
  if(m.type==='system'){appendSys(m.text);return}
  if(m.type==='joined')return;
  if(m.type==='typing'){if(m.name!==myName){typUsers[m.name]=Date.now();renderTyp();renderOnline(onlineUsers)}return}
  if(m.type==='reaction'){applyRx(m.msgId,m.emoji,m.name);return}
  if(m.type==='read'){applyRead(m.msgId);return}
  if(m.type==='pin'){applyPin(m.msgId,m.text,m.name);return}
  if(m.type==='delete'){applyDelete(m.msgId);return}
  if(m.type==='chat'){const msgs=lm();if(!msgs.find(x=>x.id===m.id)){msgs.push(m);sm(msgs);appendBub(m,true)}if(ws&&ws.readyState===1)ws.send(JSON.stringify({type:'read',msgId:m.id,name:myName}));return}
  if(m.type==='img_start'){imgAssembly[m.imgId]={chunks:[],total:m.total,name:m.name,ts:m.ts,id:m.imgId};showImgProgress(m.imgId,m.name,m.ts,0,m.total);return}
  if(m.type==='img_chunk'){const a=imgAssembly[m.imgId];if(!a)return;a.chunks[m.idx]=m.data;updateImgProgress(m.imgId,a.chunks.filter(Boolean).length,a.total);return}
  if(m.type==='img_end'){const a=imgAssembly[m.imgId];if(!a)return;const msg={type:'image',id:a.id,name:a.name,data:a.chunks.join(''),ts:a.ts};delete imgAssembly[m.imgId];const msgs=lm();msgs.push(msg);sm(msgs);finalizeImgBubble(msg);return}
  if(m.type==='voice_start'){voiceAssembly[m.voiceId]={chunks:[],total:m.total,name:m.name,ts:m.ts,id:m.voiceId};showVoiceProgress(m.voiceId,m.name,m.ts);return}
  if(m.type==='voice_chunk'){const a=voiceAssembly[m.voiceId];if(!a)return;a.chunks[m.idx]=m.data;return}
  if(m.type==='voice_end'){const a=voiceAssembly[m.voiceId];if(!a)return;const fd=a.chunks.join('');delete voiceAssembly[m.voiceId];voiceMap[a.id]=fd;finalizeVoiceBubble({id:a.id,name:a.name,ts:a.ts});return}
}
function setReply(id,name,text){replyTo={id,name,text};document.getElementById('rpName').textContent=name;document.getElementById('rpText').textContent=(text||'').slice(0,60);document.getElementById('replyPreview').classList.add('show');document.getElementById('txtInp').focus()}
function clearReply(){replyTo=null;document.getElementById('replyPreview').classList.remove('show')}
function applyPin(msgId,text,from){pinnedMsgId=msgId;document.getElementById('pinnedText').textContent=text||'[media]';document.getElementById('pinnedBanner').classList.add('show');const el=msgMap[msgId];if(el){const b=el.querySelector('.bubble');if(b)b.classList.add('pinned-bub')}toast('📌 Pinned by '+from)}
function scrollToPin(){if(!pinnedMsgId)return;const el=msgMap[pinnedMsgId];if(el)el.scrollIntoView({behavior:'smooth',block:'center'})}
function applyDelete(msgId){const el=msgMap[msgId];if(el){el.style.transition='all .3s';el.style.opacity='0';el.style.transform='scale(.85)';setTimeout(()=>el.remove(),310);delete msgMap[msgId]}}
function applyRead(msgId){const el=document.getElementById('rr_'+msgId);if(el)el.textContent='✓✓ seen'}
function showCtx(e,id,text){e.preventDefault();e.stopPropagation();ctxMsgId=id;ctxMsgText=text;const m=document.getElementById('ctxMenu');m.style.left=Math.min(e.clientX,window.innerWidth-170)+'px';m.style.top=Math.min(e.clientY,window.innerHeight-160)+'px';m.classList.add('show')}
document.addEventListener('click',()=>{document.getElementById('ctxMenu').classList.remove('show');if(openRx){openRx.classList.remove('show');openRx=null}});
function ctxReply(){const el=msgMap[ctxMsgId];const nm=el?el.querySelector('.nm')?.textContent:'';setReply(ctxMsgId,nm,ctxMsgText);document.getElementById('ctxMenu').classList.remove('show')}
function ctxPin(){if(ws&&ws.readyState===1)ws.send(JSON.stringify({type:'pin',msgId:ctxMsgId,text:ctxMsgText,name:myName}));document.getElementById('ctxMenu').classList.remove('show')}
function ctxCopy(){navigator.clipboard?.writeText(ctxMsgText).then(()=>toast('Copied!')).catch(()=>toast('Copy failed'));document.getElementById('ctxMenu').classList.remove('show')}
function ctxDelete(){if(ws&&ws.readyState===1)ws.send(JSON.stringify({type:'delete',msgId:ctxMsgId,name:myName}));applyDelete(ctxMsgId);document.getElementById('ctxMenu').classList.remove('show')}
function voiceBubHTML(id){return '<div class="voiceBub"><button class="vPlayBtn" id="vpb_'+id+'" onclick="playVoice(\''+id+'\',event)">&#9654;</button><div class="vBars" id="vw_'+id+'">'+'<div class="vBar"></div>'.repeat(8)+'</div><span class="vDur" id="vd_'+id+'">0:00</span></div>'}
function appendBub(m,scroll){
  const mine=m.name===myName,col=nc(m.name);
  const w=document.createElement('div');w.className='bw '+(mine?'mine':'other')+(scroll?' new':'');w.id='imgbub_'+m.id;
  let content='';
  if(m.type==='image')content='<div class="imgWrap"><img src="'+m.data+'" onclick="viewImg(this.src)" loading="lazy"></div>';
  else if(m.type==='voice'){if(m.data)voiceMap[m.id]=m.data;content=voiceBubHTML(m.id)}
  else content=esc(m.text||'').replace(/\n/g,'<br>');
  const rq=m.replyTo?'<div class="replyQuote" onclick="scrollToMsg(\''+m.replyTo.id+'\')"><strong>'+esc(m.replyTo.name)+'</strong>'+esc((m.replyTo.text||'').slice(0,60))+'</div>':'';
  const txtSafe=(m.text||'').replace(/\\/g,'\\\\').replace(/'/g,"\\'").replace(/\n/g,' ');
  w.innerHTML='<div class="meta"><span class="nm" style="color:'+col+'">'+esc(m.name)+'</span></div>'+rq+
    '<div class="bubble" onclick="toggleRx(event,\''+m.id+'\')" oncontextmenu="showCtx(event,\''+m.id+'\',\''+txtSafe+'\')">'+content+
    '<div class="rxPicker" id="rx_'+m.id+'">'+RX.map(r=>'<span onclick="sendRxD(event,\''+m.id+'\',\''+r+'\')">'+r+'</span>').join('')+'</div></div>'+
    '<div class="rxRow" id="rxrow_'+m.id+'"></div>'+(mine?'<div class="rr" id="rr_'+m.id+'">&#10003;</div>':'')+
    '<div class="ts">'+ft(m.ts)+'</div>';
  document.getElementById('msgs').appendChild(w);msgMap[m.id]=w;
  if(scroll){const msgs=document.getElementById('msgs');if(msgs.scrollHeight-msgs.scrollTop-msgs.clientHeight<200)w.scrollIntoView({behavior:'smooth',block:'end'});if(!mine)playNotif();haptic()}
}
function scrollToMsg(id){const el=msgMap[id];if(el)el.scrollIntoView({behavior:'smooth',block:'center'})}
let audioCtx=null;
function playNotif(){try{if(!audioCtx)audioCtx=new(window.AudioContext||window.webkitAudioContext)();const o=audioCtx.createOscillator(),g=audioCtx.createGain();o.connect(g);g.connect(audioCtx.destination);o.type='sine';o.frequency.value=880;g.gain.setValueAtTime(0.12,audioCtx.currentTime);g.gain.exponentialRampToValueAtTime(0.001,audioCtx.currentTime+0.15);o.start();o.stop(audioCtx.currentTime+0.15)}catch(e){}}
function playVoice(id,e){if(e)e.stopPropagation();const src=voiceMap[id];if(!src)return;if(currentAudio&&currentAudio._id===id&&!currentAudio.paused){currentAudio.pause();resetVoiceUI(id);currentAudio=null;return}if(currentAudio&&!currentAudio.paused){currentAudio.pause();if(currentAudio._id)resetVoiceUI(currentAudio._id);currentAudio=null}const audio=new Audio(src);audio._id=id;currentAudio=audio;const btn=document.getElementById('vpb_'+id),wave=document.getElementById('vw_'+id),dur=document.getElementById('vd_'+id);if(btn)btn.innerHTML='&#9646;&#9646;';if(wave)wave.classList.add('playing');audio.ontimeupdate=()=>{if(!dur)return;const t=Math.floor(audio.currentTime);dur.textContent=Math.floor(t/60)+':'+(t%60).toString().padStart(2,'0')};audio.onended=()=>{resetVoiceUI(id);currentAudio=null};audio.onerror=()=>{resetVoiceUI(id);currentAudio=null};audio.play().catch(()=>resetVoiceUI(id))}
function resetVoiceUI(id){const btn=document.getElementById('vpb_'+id),wave=document.getElementById('vw_'+id),dur=document.getElementById('vd_'+id);if(btn)btn.innerHTML='&#9654;';if(wave)wave.classList.remove('playing');if(dur)dur.textContent='0:00'}
function appendSys(text){const d=document.createElement('div');d.className='sys';d.textContent=text;document.getElementById('msgs').appendChild(d);d.scrollIntoView({behavior:'smooth',block:'end'})}
function showImgProgress(imgId,name,ts,got,total){if(name===myName)return;const col=nc(name),w=document.createElement('div');w.className='bw other new';w.id='imgbub_'+imgId;const pct=total>0?Math.round(got/total*100):0;w.innerHTML='<div class="meta"><span class="nm" style="color:'+col+'">'+esc(name)+'</span></div><div class="bubble"><div class="imgWrap"><div class="imgProg" id="iprog_'+imgId+'"><div class="pbar"><div class="pfill" id="pfill_'+imgId+'" style="width:'+pct+'%"></div></div><div class="ptxt" id="ptxt_'+imgId+'">'+pct+'%</div></div></div></div><div class="ts">'+ft(ts)+'</div>';document.getElementById('msgs').appendChild(w);w.scrollIntoView({behavior:'smooth',block:'end'});msgMap[imgId]=w}
function updateImgProgress(imgId,got,total){const fill=document.getElementById('pfill_'+imgId),txt=document.getElementById('ptxt_'+imgId);if(!fill)return;const pct=Math.round(got/total*100);fill.style.width=pct+'%';if(txt)txt.textContent=pct+'%'}
function finalizeImgBubble(msg){const col=nc(msg.name);const html='<div class="meta"><span class="nm" style="color:'+col+'">'+esc(msg.name)+'</span></div><div class="bubble"><div class="imgWrap"><img src="'+msg.data+'" onclick="viewImg(this.src)" loading="lazy"></div></div><div class="ts">'+ft(msg.ts)+'</div>';const ex=document.getElementById('imgbub_'+msg.id);if(ex)ex.innerHTML=html;else{const w=document.createElement('div');w.className='bw other new';w.id='imgbub_'+msg.id;w.innerHTML=html;document.getElementById('msgs').appendChild(w);w.scrollIntoView({behavior:'smooth',block:'end'});msgMap[msg.id]=w}}
function showVoiceProgress(voiceId,name,ts){if(name===myName)return;const col=nc(name),w=document.createElement('div');w.className='bw other new';w.id='imgbub_'+voiceId;w.innerHTML='<div class="meta"><span class="nm" style="color:'+col+'">'+esc(name)+'</span></div><div class="bubble"><div class="voiceBub"><span style="font-size:22px">&#127908;</span><span style="color:var(--text2);font-size:12px;margin-left:6px">Receiving<span class="df"><span></span><span></span><span></span></span></span></div></div><div class="ts">'+ft(ts)+'</div>';document.getElementById('msgs').appendChild(w);w.scrollIntoView({behavior:'smooth',block:'end'});msgMap[voiceId]=w}
function finalizeVoiceBubble(info){const col=nc(info.name);const html='<div class="meta"><span class="nm" style="color:'+col+'">'+esc(info.name)+'</span></div><div class="bubble">'+voiceBubHTML(info.id)+'</div><div class="ts">'+ft(info.ts)+'</div>';const ex=document.getElementById('imgbub_'+info.id);if(ex)ex.innerHTML=html;else{const w=document.createElement('div');w.className='bw other new';w.id='imgbub_'+info.id;w.innerHTML=html;document.getElementById('msgs').appendChild(w);w.scrollIntoView({behavior:'smooth',block:'end'});msgMap[info.id]=w}playNotif();haptic()}
function toggleRx(e,id){e.stopPropagation();const p=document.getElementById('rx_'+id);if(openRx&&openRx!==p)openRx.classList.remove('show');p.classList.toggle('show');openRx=p.classList.contains('show')?p:null}
function sendRxD(e,msgId,emoji){e.stopPropagation();if(openRx){openRx.classList.remove('show');openRx=null}ws.send(JSON.stringify({type:'reaction',msgId,emoji,name:myName}));applyRx(msgId,emoji,myName)}
function applyRx(msgId,emoji,from){const row=document.getElementById('rxrow_'+msgId);if(!row)return;let chip=row.querySelector('[data-e="'+emoji+'"]');if(chip){chip.dataset.count=parseInt(chip.dataset.count||1)+1;chip.textContent=emoji+' '+chip.dataset.count}else{const c=document.createElement('div');c.className='rxChip';c.dataset.e=emoji;c.dataset.count=1;c.textContent=emoji;c.onclick=()=>{ws.send(JSON.stringify({type:'reaction',msgId,emoji,name:myName}));applyRx(msgId,emoji,myName)};row.appendChild(c);c.animate([{transform:'scale(0)'},{transform:'scale(1.25)'},{transform:'scale(1)'}],{duration:300})}}
function renderOnline(users){const el=document.getElementById('onList');if(!users||!users.length){el.innerHTML='No one online';return}const now=Date.now();el.innerHTML=users.map(u=>{const isTyp=u!==myName&&typUsers[u]&&(now-typUsers[u]<3000);return'<span class="chip'+(u===myName?' me':isTyp?' istyping':'')+'">'+ esc(u)+(isTyp?'<span class="tyiDots"><span></span><span></span><span></span></span>':'')+'</span>'}).join(' ')}
function onType(){if(ws&&ws.readyState===1)ws.send(JSON.stringify({type:'typing',name:myName}));clearTimeout(typTimer);typTimer=setTimeout(()=>{},2000)}
function renderTyp(){const now=Date.now();Object.keys(typUsers).forEach(k=>{if(now-typUsers[k]>3000)delete typUsers[k]});const others=Object.keys(typUsers).filter(k=>k!==myName);const bar=document.getElementById('typBar');if(!others.length){bar.innerHTML='';return}bar.innerHTML=esc(others.join(', '))+' is typing <span class="df"><span></span><span></span><span></span></span>';setTimeout(renderTyp,1500)}
function sendMsg(){const el=document.getElementById('txtInp'),text=el.innerText.trim();if(!text||!ws||ws.readyState!==1)return;const m={type:'chat',id:uid(),name:myName,text,ts:Date.now()};if(replyTo)m.replyTo=replyTo;ws.send(JSON.stringify(m));const msgs=lm();msgs.push(m);sm(msgs);appendBub(m,true);el.innerText='';clearReply();document.getElementById('emoPicker').classList.remove('show');const btn=document.getElementById('sndBtn');btn.style.transform='scale(.8)';setTimeout(()=>btn.style.transform='',150);haptic()}
function sendImage(input){const file=input.files[0];if(!file)return;const reader=new FileReader();reader.onload=e=>{const img=new Image();img.onload=()=>{const MAX=500;let w=img.width,h=img.height;if(w>MAX||h>MAX){if(w>h){h=Math.round(h*MAX/w);w=MAX}else{w=Math.round(w*MAX/h);h=MAX}}const cv=document.createElement('canvas');cv.width=w;cv.height=h;cv.getContext('2d').drawImage(img,0,0,w,h);const fullData=cv.toDataURL('image/jpeg',0.65);const imgId=uid(),ts=Date.now();const m={type:'image',id:imgId,name:myName,data:fullData,ts};const msgs=lm();msgs.push(m);sm(msgs);appendBub(m,true);const chunks=[];for(let i=0;i<fullData.length;i+=CHUNK_SIZE)chunks.push(fullData.slice(i,i+CHUNK_SIZE));ws.send(JSON.stringify({type:'img_start',imgId,total:chunks.length,name:myName,ts}));let idx=0;function sn(){if(idx>=chunks.length){ws.send(JSON.stringify({type:'img_end',imgId}));return}ws.send(JSON.stringify({type:'img_chunk',imgId,idx,data:chunks[idx]}));idx++;setTimeout(sn,30)}sn()};img.src=e.target.result};reader.readAsDataURL(file);input.value=''}
async function startVoice(e){if(e)e.preventDefault();if(isRecording)return;if(!window.MediaRecorder||!navigator.mediaDevices){toast('Voice needs Firefox or HTTPS');return}try{const stream=await navigator.mediaDevices.getUserMedia({audio:true,video:false});audioChunks=[];const types=['audio/webm;codecs=opus','audio/webm','audio/ogg;codecs=opus','audio/ogg'];const mimeType=types.find(t=>MediaRecorder.isTypeSupported(t))||'';mediaRecorder=new MediaRecorder(stream,mimeType?{mimeType}:{});mediaRecorder.ondataavailable=ev=>{if(ev.data&&ev.data.size>0)audioChunks.push(ev.data)};mediaRecorder.onstop=()=>{stream.getTracks().forEach(t=>t.stop());if(audioChunks.length)sendVoice(new Blob(audioChunks,{type:mediaRecorder.mimeType||'audio/webm'}))};mediaRecorder.start(200);isRecording=true;document.getElementById('micBtn').classList.add('rec');haptic()}catch(err){toast('Mic error: '+err.message)}}
function stopVoice(){if(!isRecording)return;isRecording=false;document.getElementById('micBtn').classList.remove('rec');if(mediaRecorder&&mediaRecorder.state!=='inactive')mediaRecorder.stop()}
function sendVoice(blob){if(!blob||blob.size<500)return;const reader=new FileReader();reader.onload=e=>{const fullData=e.target.result,voiceId=uid(),ts=Date.now();voiceMap[voiceId]=fullData;appendBub({type:'voice',id:voiceId,name:myName,ts},true);const chunks=[];for(let i=0;i<fullData.length;i+=CHUNK_SIZE)chunks.push(fullData.slice(i,i+CHUNK_SIZE));ws.send(JSON.stringify({type:'voice_start',voiceId,total:chunks.length,name:myName,ts}));let idx=0;function sn(){if(idx>=chunks.length){ws.send(JSON.stringify({type:'voice_end',voiceId}));return}ws.send(JSON.stringify({type:'voice_chunk',voiceId,idx,data:chunks[idx]}));idx++;setTimeout(sn,30)}sn()};reader.readAsDataURL(blob)}
function renderAll(){const box=document.getElementById('msgs');box.innerHTML='';msgMap={};lm().forEach(m=>appendBub(m,false));box.scrollTop=box.scrollHeight}
function viewImg(src){document.getElementById('viewImg').src=src;document.getElementById('imgView').classList.add('show')}
function showLogs(){document.getElementById('logModal').classList.add('show');fetchLogs()}
function fetchLogs(){fetch('/logs').then(r=>r.text()).then(t=>{const el=document.getElementById('logContent');el.textContent=t||'No logs.';el.scrollTop=el.scrollHeight}).catch(()=>{document.getElementById('logContent').textContent='Failed.'})}
function clearChat(){if(confirm('Clear all local messages?')){localStorage.removeItem(SK);document.getElementById('msgs').innerHTML='';msgMap={}}}
(()=>{let lt=null,moved=false;document.getElementById('msgs').addEventListener('pointerdown',e=>{moved=false;const bub=e.target.closest('.bubble');if(!bub)return;lt=setTimeout(()=>{if(!moved){const wrap=bub.closest('.bw');if(!wrap)return;showCtx(e,wrap.id.replace('imgbub_',''),bub.innerText||'');haptic()}},600)});document.getElementById('msgs').addEventListener('pointermove',()=>{moved=true;clearTimeout(lt)});document.getElementById('msgs').addEventListener('pointerup',()=>clearTimeout(lt))})();
window.onload=()=>document.getElementById('nameInp').focus();
</script>
</body>
</html>
)rawhtml";

// ─── HTTP handlers ────────────────────────────────────────────────────────────
void handleRoot()     { httpServer.send_P(200, "text/html", HTML); }
void handleLogs()     { httpServer.send(200, "text/plain", getLogs()); }
void handleNotFound() { httpServer.send(204, "text/plain", ""); }

// ─── setup() ─────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(1000);  // Let USB-CDC settle on ESP32-S3

  for (int i = 0; i < 10; i++) { clients[i] = false; clientNames[i] = ""; }

  addLog("=== IBMOVS Chat v3.1 ===");

  // OLED — SW_I2C, no Wire.begin()
  if (u8g2.begin()) addLog("[OLED] OK SDA=4 SCL=3");
  else              addLog("[OLED] FAIL check wiring");
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(0, 18, "IBMOVS Chat v3.1");
  u8g2.drawStr(0, 30, "Starting WiFi...");
  u8g2.sendBuffer();

  // WiFi AP — correct init sequence for ESP32-S3
  WiFi.mode(WIFI_OFF);          // full reset
  delay(200);
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);         // CRITICAL: disable modem sleep
  WiFi.setTxPower(WIFI_POWER_19_5dBm);  // max TX

  WiFi.softAPConfig(AP_IP, AP_GW, AP_SUBNET);
  bool apOK = WiFi.softAP(AP_SSID, NULL);  // NULL = open network
  delay(500);                    // CRITICAL: wait for AP to fully init

  if (!apOK) {
    addLog("[WiFi] FAIL — retry...");
    delay(1000);
    apOK = WiFi.softAP(AP_SSID, NULL);
    delay(500);
  }

  addLog(apOK ? "[WiFi] AP OK: IBMOVS-Chat @ 192.168.4.1"
               : "[WiFi] AP STILL FAILED");

  // Update OLED
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(0, 18, apOK ? "WiFi AP READY!" : "WiFi FAILED!");
  u8g2.drawStr(0, 30, "IP: 192.168.4.1");
  u8g2.sendBuffer();
  delay(600);

  // DNS: only chat.ibmovs, not wildcard
  dnsServer.start(53, "chat.ibmovs", AP_IP);
  addLog("[DNS] chat.ibmovs -> 192.168.4.1");

  httpServer.on("/",     handleRoot);
  httpServer.on("/chat", handleRoot);
  httpServer.on("/logs", handleLogs);
  httpServer.onNotFound(handleNotFound);
  httpServer.begin();
  addLog("[HTTP] Port 80 OK");

  wsServer.begin();
  wsServer.onEvent(wsEvent);
  addLog("[WS] Port 81 OK");

  startTime = millis();
  oledNext  = millis() + 5000;
  addLog("[BOOT] DONE");
  oledIdle();
}

// ─── loop() ──────────────────────────────────────────────────────────────────
void loop() {
  dnsServer.processNextRequest();
  httpServer.handleClient();
  wsServer.loop();

  static unsigned long lo = 0;
  unsigned long now = millis();
  if (wiping) {
    if (now - lo > 28) { lo = now; oledDraw(); }
  } else {
    if (now - lo > 1000) { lo = now; oledDraw(); }
  }
}
