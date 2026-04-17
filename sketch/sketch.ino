#include <Arduino.h>
#include <U8g2lib.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>

// â”€â”€â”€ OLED: SCK=GP1, SDA=GP2, CS=GP5, DC=GP4, RES=GP3 â”€â”€â”€
U8G2_SH1106_128X64_NONAME_F_4W_SW_SPI u8g2(
  U8G2_R0, 1, 2, 5, 4, 3
);

// â”€â”€â”€ SoftAP Config â”€â”€â”€
const char* AP_SSID     = "IBMOVS-Chat";
const char* AP_PASSWORD = "";          // open network
const IPAddress AP_IP(192, 168, 4, 1);
const IPAddress AP_SUBNET(255, 255, 255, 0);

WebServer server(80);
DNSServer dnsServer;

unsigned long startTime;
int connectedClients = 0;

// â”€â”€â”€ Chat HTML (stored in flash) â”€â”€â”€
const char CHAT_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>IBMOVS Chat</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{font-family:'Segoe UI',sans-serif;background:#0f0f1a;color:#e0e0e0;height:100dvh;display:flex;flex-direction:column}
  #header{background:linear-gradient(135deg,#1a1a2e,#16213e);padding:12px 16px;display:flex;align-items:center;gap:10px;border-bottom:1px solid #2a2a4a}
  #header .logo{width:36px;height:36px;background:linear-gradient(135deg,#00d4ff,#0066ff);border-radius:50%;display:flex;align-items:center;justify-content:center;font-weight:bold;font-size:14px;color:#fff}
  #header h1{font-size:16px;color:#00d4ff}
  #header .sub{font-size:11px;color:#888;margin-top:2px}
  #onlineBar{background:#111122;padding:6px 16px;font-size:11px;color:#00d4ff;border-bottom:1px solid #1a1a3a}
  #messages{flex:1;overflow-y:auto;padding:12px 16px;display:flex;flex-direction:column;gap:8px}
  .msg{max-width:80%;padding:8px 12px;border-radius:12px;font-size:13px;line-height:1.4;word-break:break-word}
  .msg .meta{font-size:10px;margin-bottom:3px;opacity:0.7}
  .msg.mine{background:linear-gradient(135deg,#0066ff,#0044cc);align-self:flex-end;border-bottom-right-radius:3px}
  .msg.mine .meta{color:#aad4ff;text-align:right}
  .msg.other{background:#1e1e3a;align-self:flex-start;border-bottom-left-radius:3px;border:1px solid #2a2a4a}
  .msg.other .meta{color:#888}
  .msg.system{background:#111;color:#666;font-size:11px;align-self:center;border-radius:20px;padding:4px 12px;border:1px solid #222}
  #inputArea{padding:10px 12px;background:#111122;border-top:1px solid #1a1a3a;display:flex;gap:8px}
  #msgInput{flex:1;background:#1a1a2e;border:1px solid #2a2a4a;border-radius:20px;padding:10px 16px;color:#e0e0e0;font-size:14px;outline:none}
  #msgInput:focus{border-color:#0066ff}
  #sendBtn{background:linear-gradient(135deg,#0066ff,#0044cc);border:none;border-radius:50%;width:42px;height:42px;color:#fff;font-size:18px;cursor:pointer;display:flex;align-items:center;justify-content:center}
  #nameModal{position:fixed;inset:0;background:rgba(0,0,0,0.85);display:flex;align-items:center;justify-content:center;z-index:100}
  #nameBox{background:#1a1a2e;border:1px solid #2a2a4a;border-radius:16px;padding:28px 24px;width:280px;text-align:center}
  #nameBox h2{color:#00d4ff;margin-bottom:6px;font-size:18px}
  #nameBox p{color:#888;font-size:12px;margin-bottom:16px}
  #nameInput{width:100%;background:#111;border:1px solid #2a2a4a;border-radius:8px;padding:10px 14px;color:#e0e0e0;font-size:15px;outline:none;margin-bottom:12px}
  #joinBtn{width:100%;background:linear-gradient(135deg,#0066ff,#0044cc);border:none;border-radius:8px;padding:11px;color:#fff;font-size:15px;font-weight:bold;cursor:pointer}
  ::-webkit-scrollbar{width:4px}
  ::-webkit-scrollbar-track{background:#0f0f1a}
  ::-webkit-scrollbar-thumb{background:#2a2a4a;border-radius:2px}
</style>
</head>
<body>

<div id="nameModal">
  <div id="nameBox">
    <div style="font-size:36px;margin-bottom:8px">ðŸ’¬</div>
    <h2>IBMOVS Chat</h2>
    <p>Hosted by ESP32-S3-Zero<br>Enter your name to join</p>
    <input id="nameInput" type="text" placeholder="Your name..." maxlength="20" autocomplete="off">
    <button id="joinBtn" onclick="joinChat()">Join Chat</button>
  </div>
</div>

<div id="header">
  <div class="logo">IB</div>
  <div>
    <h1>IBMOVS Chat</h1>
    <div class="sub">ESP32-S3-Zero Hotspot</div>
  </div>
</div>
<div id="onlineBar">ðŸŸ¢ <span id="onlineCount">0</span> online</div>
<div id="messages"></div>
<div id="inputArea">
  <input id="msgInput" type="text" placeholder="Type a message..." maxlength="200" autocomplete="off" onkeydown="if(event.key==='Enter')sendMsg()">
  <button id="sendBtn" onclick="sendMsg()">âž¤</button>
</div>

<script>
// â”€â”€ State â”€â”€
const STORAGE_KEY = 'ibmovs_chat_v1';
const USERS_KEY   = 'ibmovs_users_v1';
let myName = '';
let myColor = '';
let lastSeen = 0;

const COLORS = ['#00d4ff','#ff6b6b','#51cf66','#ffd43b','#cc5de8','#ff922b','#20c997','#f06595'];

function getColor(name) {
  let h = 0;
  for(let c of name) h = (h * 31 + c.charCodeAt(0)) & 0xffff;
  return COLORS[h % COLORS.length];
}

// â”€â”€ Storage helpers â”€â”€
function loadMessages() {
  try { return JSON.parse(localStorage.getItem(STORAGE_KEY) || '[]'); } catch(e) { return []; }
}
function saveMessages(msgs) {
  // keep last 200
  if(msgs.length > 200) msgs = msgs.slice(-200);
  localStorage.setItem(STORAGE_KEY, JSON.stringify(msgs));
}
function loadUsers() {
  try { return JSON.parse(localStorage.getItem(USERS_KEY) || '{}'); } catch(e) { return {}; }
}
function saveUsers(u) { localStorage.setItem(USERS_KEY, JSON.stringify(u)); }

// â”€â”€ Presence â”€â”€
function updatePresence() {
  if(!myName) return;
  const users = loadUsers();
  users[myName] = Date.now();
  // prune inactive (>15s)
  for(const [k,v] of Object.entries(users)) {
    if(Date.now() - v > 15000) delete users[k];
  }
  saveUsers(users);
  const count = Object.keys(users).length;
  document.getElementById('onlineCount').textContent = count;
}

// â”€â”€ Join â”€â”€
function joinChat() {
  const n = document.getElementById('nameInput').value.trim();
  if(!n) { document.getElementById('nameInput').focus(); return; }
  myName  = n;
  myColor = getColor(n);
  document.getElementById('nameModal').style.display = 'none';
  updatePresence();
  renderAll();
  addSystemMsg(myName + ' joined the chat ðŸ‘‹');
  setInterval(poll, 1000);
  setInterval(updatePresence, 5000);
  document.getElementById('msgInput').focus();
}

// â”€â”€ Render â”€â”€
function renderAll() {
  const box  = document.getElementById('messages');
  const msgs = loadMessages();
  box.innerHTML = '';
  msgs.forEach(m => appendBubble(m, false));
  box.scrollTop = box.scrollHeight;
  if(msgs.length) lastSeen = msgs[msgs.length-1].ts;
}

function appendBubble(m, scroll=true) {
  const box = document.getElementById('messages');
  const div = document.createElement('div');
  if(m.type === 'system') {
    div.className = 'msg system';
    div.textContent = m.text;
  } else {
    const mine = m.name === myName;
    div.className = 'msg ' + (mine ? 'mine' : 'other');
    const color = getColor(m.name);
    div.innerHTML = `<div class="meta" style="color:${color}">${m.name} Â· ${formatTime(m.ts)}</div><div>${escHtml(m.text)}</div>`;
  }
  box.appendChild(div);
  if(scroll) box.scrollTop = box.scrollHeight;
}

function escHtml(t) {
  return t.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
}
function formatTime(ts) {
  const d = new Date(ts);
  return d.getHours().toString().padStart(2,'0') + ':' + d.getMinutes().toString().padStart(2,'0');
}

// â”€â”€ Poll for new messages â”€â”€
function poll() {
  updatePresence();
  const msgs = loadMessages();
  const newMsgs = msgs.filter(m => m.ts > lastSeen);
  newMsgs.forEach(m => {
    if(m.name !== myName) appendBubble(m);
  });
  if(newMsgs.length) lastSeen = newMsgs[newMsgs.length-1].ts;
}

// â”€â”€ Send â”€â”€
function sendMsg() {
  const input = document.getElementById('msgInput');
  const text  = input.value.trim();
  if(!text || !myName) return;
  const msg = { name: myName, text, ts: Date.now(), type: 'chat' };
  const msgs = loadMessages();
  msgs.push(msg);
  saveMessages(msgs);
  appendBubble(msg);
  lastSeen = msg.ts;
  input.value = '';
  input.focus();
}

function addSystemMsg(text) {
  const msg = { type: 'system', text, ts: Date.now() };
  const msgs = loadMessages();
  msgs.push(msg);
  saveMessages(msgs);
  appendBubble(msg);
  lastSeen = msg.ts;
}

// â”€â”€ Auto-focus name input â”€â”€
window.onload = () => document.getElementById('nameInput').focus();
</script>
</body>
</html>
)rawhtml";

// â”€â”€â”€ OLED Display Helper â”€â”€â”€
void oledStatus(const char* line1, const char* line2 = "", const char* line3 = "") {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 10, line1);
  u8g2.setFont(u8g2_font_5x7_tf);
  if(strlen(line2)) u8g2.drawStr(0, 24, line2);
  if(strlen(line3)) u8g2.drawStr(0, 34, line3);
  u8g2.sendBuffer();
}

// â”€â”€â”€ Web Handlers â”€â”€â”€
void handleRoot() {
  server.send_P(200, "text/html", CHAT_HTML);
}

void handleNotFound() {
  // Captive portal â€” redirect everything to root
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.send(302, "text/plain", "");
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== IBMOVS ESP32-S3 Boot ===");

  // â”€â”€ OLED Init â”€â”€
  Serial.println("[OLED] Initializing...");
  oledStatus("IBMOVS Boot", "OLED OK");
  u8g2.begin();
  oledStatus("IBMOVS Boot", "OLED OK");
  Serial.println("[OLED] OK");
  delay(500);

  // â”€â”€ WiFi SoftAP â”€â”€
  Serial.println("[WiFi] Starting SoftAP...");
  oledStatus("Starting WiFi", "SoftAP...");
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_IP, AP_SUBNET);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.printf("[WiFi] SSID: %s\n", AP_SSID);
  Serial.printf("[WiFi] IP:   %s\n", AP_IP.toString().c_str());
  oledStatus("WiFi UP!", AP_SSID, "192.168.4.1");
  delay(800);

  // â”€â”€ DNS (Captive Portal) â”€â”€
  Serial.println("[DNS] Starting captive portal DNS...");
  dnsServer.start(53, "*", AP_IP);
  Serial.println("[DNS] OK â€” all domains -> 192.168.4.1");

  // â”€â”€ Web Server â”€â”€
  Serial.println("[HTTP] Starting web server...");
  server.on("/", handleRoot);
  server.on("/chat", handleRoot);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("[HTTP] Server started on port 80");

  startTime = millis();
  Serial.println("[BOOT] All systems GO!");
}

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();

  // â”€â”€ Update OLED every second â”€â”€
  static unsigned long lastOled = 0;
  if(millis() - lastOled > 1000) {
    lastOled = millis();

    unsigned long elapsed = (millis() - startTime) / 1000;
    int ss = elapsed % 60;
    int mm = (elapsed / 60) % 60;
    int hh = elapsed / 3600;

    char timeBuf[9];
    sprintf(timeBuf, "%02d:%02d:%02d", hh, mm, ss);

    int clients = WiFi.softAPgetStationNum();
    if(clients != connectedClients) {
      connectedClients = clients;
      Serial.printf("[WiFi] Clients connected: %d\n", clients);
    }

    char clientBuf[20];
    sprintf(clientBuf, "Users: %d", clients);

    u8g2.clearBuffer();

    // Title
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(20, 10, "IBMOVS CHAT");
    u8g2.drawHLine(0, 13, 128);

    // SSID
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0, 24, "WiFi: IBMOVS-Chat");

    // IP
    u8g2.drawStr(0, 33, "IP: 192.168.4.1");

    // Clients
    u8g2.drawStr(0, 42, clientBuf);

    // Uptime
    u8g2.drawHLine(0, 45, 128);
    u8g2.setFont(u8g2_font_logisoso16_tf);
    u8g2.drawStr(22, 63, timeBuf);

    u8g2.sendBuffer();
  }
}
