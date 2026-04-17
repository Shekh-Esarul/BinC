#include <Arduino.h>
#include <U8g2lib.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <DNSServer.h>
#include <ArduinoJson.h>

// ─── OLED ───
U8G2_SH1106_128X64_NONAME_F_4W_SW_SPI u8g2(U8G2_R0, 1, 2, 5, 4, 3);

// ─── Network ───
const char* AP_SSID = "IBMOVS-Chat";
const char* AP_PASS = "";
const IPAddress AP_IP(192, 168, 4, 1);
const IPAddress AP_SUBNET(255, 255, 255, 0);

WebServer        httpServer(80);
WebSocketsServer wsServer(81);
DNSServer        dnsServer;

// ─── State ───
unsigned long startTime;
int  totalMessages = 0;
bool clients[10]   = {false};
String clientNames[10];

// ─── Log buffer ───
#define MAX_LOGS 40
String logBuf[MAX_LOGS];
int    logHead = 0, logCount = 0;

void addLog(String msg) {
  Serial.println(msg);
  logBuf[logHead] = msg;
  logHead = (logHead + 1) % MAX_LOGS;
  if (logCount < MAX_LOGS) logCount++;
}

String getLogs() {
  String out = "";
  int start = (logCount < MAX_LOGS) ? 0 : logHead;
  for (int i = 0; i < logCount; i++)
    out += logBuf[(start + i) % MAX_LOGS] + "\n";
  return out;
}

// ─── OLED ───
void oledDraw() {
  int c = WiFi.softAPgetStationNum();
  unsigned long e = (millis() - startTime) / 1000;
  char tb[9]; sprintf(tb, "%02lu:%02lu:%02lu", e/3600, (e/60)%60, e%60);
  char cb[16]; sprintf(cb, "Users: %d", c);
  char mb[16]; sprintf(mb, "Msgs:  %d", totalMessages);
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(16, 10, "IBMOVS CHAT");
  u8g2.drawHLine(0, 13, 128);
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(0, 23, "WiFi: IBMOVS-Chat");
  u8g2.drawStr(0, 32, "IP: 192.168.4.1");
  u8g2.drawStr(0, 41, cb);
  u8g2.drawStr(0, 50, mb);
  u8g2.drawHLine(0, 53, 128);
  u8g2.setFont(u8g2_font_logisoso16_tf);
  u8g2.drawStr(22, 63, tb);
  u8g2.sendBuffer();
}

// ─── Broadcast helpers ───
void broadcastExcept(uint8_t skip, String p) {
  for (int i = 0; i < 10; i++)
    if (clients[i] && i != skip) wsServer.sendTXT(i, p);
}
void broadcastAll(String p) {
  for (int i = 0; i < 10; i++)
    if (clients[i]) wsServer.sendTXT(i, p);
}

// ─── Online list JSON ───
String buildOnlineList() {
  StaticJsonDocument<512> doc;
  doc["type"] = "online";
  JsonArray arr = doc.createNestedArray("users");
  for (int i = 0; i < 10; i++)
    if (clients[i] && clientNames[i].length() > 0) arr.add(clientNames[i]);
  String out; serializeJson(doc, out); return out;
}

// ─── WS events ───
void wsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      clients[num] = true; clientNames[num] = "";
      addLog("[WS] #" + String(num) + " connected");
      break;

    case WStype_DISCONNECTED: {
      String name = clientNames[num];
      clients[num] = false; clientNames[num] = "";
      addLog("[WS] #" + String(num) + " (" + name + ") disconnected");
      if (name.length()) {
        StaticJsonDocument<128> d; d["type"]="system";
        d["text"] = name + " left the chat";
        String o; serializeJson(d,o); broadcastAll(o);
        broadcastAll(buildOnlineList());
      }
      break;
    }

    case WStype_TEXT: {
      String msg = String((char*)payload);
      StaticJsonDocument<8192> doc;
      if (deserializeJson(doc, msg)) { addLog("[WS] JSON err"); return; }
      String t = doc["type"].as<String>();

      if (t == "join") {
        String name = doc["name"].as<String>();
        clientNames[num] = name;
        addLog("[JOIN] " + name);
        StaticJsonDocument<128> sys; sys["type"]="system";
        sys["text"] = name + " joined the chat \xF0\x9F\x91\x8B";
        String so; serializeJson(sys,so); broadcastExcept(num,so);
        broadcastAll(buildOnlineList());
        StaticJsonDocument<64> ack; ack["type"]="joined";
        String ao; serializeJson(ack,ao); wsServer.sendTXT(num,ao);
        return;
      }

      if (t == "chat" || t == "image") {
        totalMessages++;
        String name = clientNames[num];
        addLog("[MSG] " + name + ": " + (t=="image" ? "[image]" : doc["text"].as<String>()));
        broadcastAll(msg); // relay to everyone including sender
        return;
      }

      if (t == "typing") { broadcastExcept(num, msg); return; }
      if (t == "reaction") { broadcastAll(msg); return; }
      break;
    }
  }
}

// ─── HTML ───
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
  --border:#2a2a4a;--radius:18px;
}
html,body{height:100%;height:100dvh;overflow:hidden;background:var(--bg);color:var(--text);font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif}
#app{display:flex;flex-direction:column;height:100%;height:100dvh}
#hdr{background:var(--bg4);padding:10px 14px;display:flex;align-items:center;gap:10px;border-bottom:1px solid var(--border);flex-shrink:0;box-shadow:0 2px 12px #0006}
#hdr .av{width:38px;height:38px;border-radius:50%;background:linear-gradient(135deg,#4f8ef7,#6c63ff);display:flex;align-items:center;justify-content:center;font-weight:800;font-size:15px;flex-shrink:0}
#hdr .info{flex:1;min-width:0}
#hdr h1{font-size:15px;font-weight:700;color:var(--accent)}
#hdr .sub{font-size:11px;color:var(--text2)}
#hdr .btns{display:flex;gap:6px}
#hdr button{background:var(--bg3);border:1px solid var(--border);color:var(--text);border-radius:8px;padding:5px 9px;font-size:12px;cursor:pointer}
#onBar{background:var(--bg2);padding:5px 14px;font-size:11px;color:var(--accent);border-bottom:1px solid var(--border);flex-shrink:0;display:flex;align-items:center;gap:6px;overflow-x:auto;white-space:nowrap}
.onDot{width:7px;height:7px;background:var(--green);border-radius:50%;flex-shrink:0;box-shadow:0 0 6px var(--green)}
.onChip{background:var(--bg3);border:1px solid var(--border);border-radius:20px;padding:2px 8px;font-size:10px;color:var(--text2)}
.onChip.me{border-color:var(--accent);color:var(--accent)}
#msgs{flex:1;overflow-y:auto;padding:12px 14px;display:flex;flex-direction:column;gap:6px;scroll-behavior:smooth}
#msgs::-webkit-scrollbar{width:3px}
#msgs::-webkit-scrollbar-thumb{background:var(--border);border-radius:2px}
.bw{display:flex;flex-direction:column;max-width:78%}
.bw.mine{align-self:flex-end;align-items:flex-end}
.bw.other{align-self:flex-start;align-items:flex-start}
.meta{font-size:10px;color:var(--text2);margin-bottom:2px;padding:0 4px}
.meta .nm{font-weight:600}
.bubble{padding:9px 13px;border-radius:var(--radius);font-size:14px;line-height:1.45;word-break:break-word;position:relative;cursor:pointer}
.mine .bubble{background:linear-gradient(135deg,#4f8ef7,#6c63ff);border-bottom-right-radius:4px;color:#fff}
.other .bubble{background:var(--bg3);border-bottom-left-radius:4px;border:1px solid var(--border)}
.bubble img{max-width:100%;max-height:220px;border-radius:10px;display:block;margin-top:4px}
.ts{font-size:10px;opacity:.5;margin-top:2px;padding:0 4px}
.sys{align-self:center;background:#1118;border:1px solid var(--border);border-radius:20px;padding:3px 12px;font-size:11px;color:var(--text2);margin:2px 0}
.rxPicker{display:none;position:absolute;bottom:calc(100% + 6px);background:var(--bg3);border:1px solid var(--border);border-radius:12px;padding:6px 8px;gap:6px;flex-wrap:wrap;width:180px;z-index:10;box-shadow:0 4px 20px #0008}
.rxPicker.show{display:flex}
.rxPicker span{font-size:22px;cursor:pointer}
.rxRow{display:flex;gap:3px;margin-top:3px;flex-wrap:wrap}
.rxChip{background:var(--bg3);border:1px solid var(--border);border-radius:20px;padding:2px 7px;font-size:13px;cursor:pointer}
#typBar{padding:3px 16px;min-height:20px;font-size:11px;color:var(--text2);flex-shrink:0;font-style:italic}
.df{display:inline-flex;gap:3px;vertical-align:middle}
.df span{width:4px;height:4px;background:var(--text2);border-radius:50%;animation:df .9s infinite}
.df span:nth-child(2){animation-delay:.2s}.df span:nth-child(3){animation-delay:.4s}
@keyframes df{0%,80%,100%{opacity:.2}40%{opacity:1}}
#inp{padding:8px 12px;background:var(--bg2);border-top:1px solid var(--border);display:flex;gap:8px;align-items:flex-end;flex-shrink:0}
#txtInp{flex:1;background:var(--bg3);border:1px solid var(--border);border-radius:22px;padding:10px 16px;color:var(--text);font-size:14px;outline:none;resize:none;max-height:90px;min-height:40px;overflow-y:auto;line-height:1.4}
#txtInp:focus{border-color:var(--accent)}
#txtInp:empty:before{content:attr(placeholder);color:var(--text2);pointer-events:none}
.ibtn{width:40px;height:40px;border-radius:50%;border:none;cursor:pointer;display:flex;align-items:center;justify-content:center;font-size:17px;flex-shrink:0}
#imgBtn{background:var(--bg3);border:1px solid var(--border);color:var(--text2)}
#sndBtn{background:linear-gradient(135deg,#4f8ef7,#6c63ff);color:#fff;box-shadow:0 2px 10px #4f8ef755}
.modal{display:none;position:fixed;inset:0;background:#000a;z-index:200;align-items:center;justify-content:center}
.modal.show{display:flex}
.mbox{background:var(--bg3);border:1px solid var(--border);border-radius:20px;padding:28px 24px;width:min(320px,90vw);text-align:center}
.mbox h2{color:var(--accent);margin-bottom:6px;font-size:19px}
.mbox p{color:var(--text2);font-size:12px;margin-bottom:18px;line-height:1.5}
.mbox input{width:100%;background:var(--bg);border:1px solid var(--border);border-radius:10px;padding:11px 14px;color:var(--text);font-size:15px;outline:none;margin-bottom:12px}
.mbox input:focus{border-color:var(--accent)}
.pri{width:100%;background:linear-gradient(135deg,#4f8ef7,#6c63ff);border:none;border-radius:10px;padding:12px;color:#fff;font-size:15px;font-weight:700;cursor:pointer}
#logBox{background:var(--bg);border:1px solid var(--border);border-radius:16px;padding:16px;width:min(400px,95vw);max-height:72vh;display:flex;flex-direction:column;gap:8px}
#logBox h3{color:var(--accent);font-size:14px}
#logContent{flex:1;overflow-y:auto;font-family:monospace;font-size:11px;color:#7f8;background:#050505;border-radius:8px;padding:10px;white-space:pre-wrap;word-break:break-all;min-height:80px}
#logBox .lbtn{background:var(--bg3);border:1px solid var(--border);color:var(--text);border-radius:8px;padding:7px;cursor:pointer;font-size:13px}
#imgView{display:none;position:fixed;inset:0;background:#000d;z-index:300;align-items:center;justify-content:center}
#imgView.show{display:flex}
#imgView img{max-width:95vw;max-height:90vh;border-radius:12px}
#imgView .cls{position:absolute;top:16px;right:20px;color:#fff;font-size:28px;cursor:pointer;line-height:1}
</style>
</head>
<body>
<div id="app">
  <div id="hdr">
    <div class="av">IB</div>
    <div class="info"><h1>IBMOVS Chat</h1><div class="sub" id="subTxt">Connecting...</div></div>
    <div class="btns">
      <button onclick="showLogs()">&#128203; Logs</button>
      <button onclick="clearChat()">&#128465;</button>
    </div>
  </div>
  <div id="onBar"><div class="onDot"></div><span id="onList">No one online</span></div>
  <div id="msgs"></div>
  <div id="typBar"></div>
  <div id="inp">
    <button class="ibtn" id="imgBtn" onclick="document.getElementById('imgInp').click()">&#128247;</button>
    <input type="file" id="imgInp" accept="image/*" style="display:none" onchange="sendImage(this)">
    <div id="txtInp" contenteditable="true" placeholder="Message..."
         oninput="onType()"
         onkeydown="if(event.key==='Enter'&&!event.shiftKey){event.preventDefault();sendMsg()}"></div>
    <button class="ibtn" id="sndBtn" onclick="sendMsg()">&#10148;</button>
  </div>
</div>

<div class="modal show" id="nameModal">
  <div class="mbox">
    <div style="font-size:44px;margin-bottom:10px">&#128172;</div>
    <h2>IBMOVS Chat</h2>
    <p>Hosted on ESP32-S3-Zero<br>Local WiFi &bull; No internet needed</p>
    <input id="nameInp" type="text" placeholder="Your name..." maxlength="20" autocomplete="off"
           onkeydown="if(event.key==='Enter')join()">
    <button class="pri" onclick="join()">Join Chat &#128640;</button>
  </div>
</div>

<div class="modal" id="logModal">
  <div id="logBox">
    <h3>&#128203; ESP32 Logs</h3>
    <div id="logContent">Loading...</div>
    <button class="lbtn" onclick="fetchLogs()">&#128260; Refresh</button>
    <button class="lbtn" onclick="document.getElementById('logModal').classList.remove('show')">Close</button>
  </div>
</div>

<div id="imgView" onclick="this.classList.remove('show')">
  <div class="cls">&#10005;</div>
  <img id="viewImg" src="">
</div>

<script>
const SK='ibmovs_v4';
const RX=['&#128077;','&#10084;&#65039;','&#128514;','&#128558;','&#128546;','&#128293;','&#128079;','&#128175;'];
let ws,myName,myColor,typTimer;
let typUsers={};
let msgMap={};
const COLS=['#4f8ef7','#ff6b6b','#51cf66','#ffd43b','#cc5de8','#ff922b','#20c997','#f06595'];
function nc(n){let h=0;for(let c of n)h=(h*31+c.charCodeAt(0))&0xffff;return COLS[h%COLS.length]}
function ft(t){const d=new Date(t);return d.getHours().toString().padStart(2,'0')+':'+d.getMinutes().toString().padStart(2,'0')}
function esc(t){return t.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;')}
function uid(){return Math.random().toString(36).slice(2,10)}
function lm(){try{return JSON.parse(localStorage.getItem(SK)||'[]')}catch{return[]}}
function sm(m){if(m.length>300)m=m.slice(-300);localStorage.setItem(SK,JSON.stringify(m))}

function join(){
  const n=document.getElementById('nameInp').value.trim();
  if(!n)return document.getElementById('nameInp').focus();
  myName=n;myColor=nc(n);
  document.getElementById('nameModal').classList.remove('show');
  connectWS();
}

function connectWS(){
  document.getElementById('subTxt').textContent='Connecting...';
  ws=new WebSocket('ws://192.168.4.1:81');
  ws.onopen=()=>{
    document.getElementById('subTxt').textContent='ESP32-S3-Zero Hotspot';
    ws.send(JSON.stringify({type:'join',name:myName}));
    renderAll();
    document.getElementById('txtInp').focus();
  };
  ws.onclose=()=>{
    document.getElementById('subTxt').textContent='Reconnecting...';
    setTimeout(connectWS,2000);
  };
  ws.onmessage=e=>{try{handle(JSON.parse(e.data))}catch(err){console.error(err)}};
}

function handle(m){
  if(m.type==='online'){renderOnline(m.users);return}
  if(m.type==='system'){appendSys(m.text);return}
  if(m.type==='joined')return;
  if(m.type==='typing'){
    if(m.name===myName)return;
    typUsers[m.name]=Date.now();renderTyp();return;
  }
  if(m.type==='reaction'){applyRx(m.msgId,m.emoji,m.name);return}
  if(m.type==='chat'||m.type==='image'){
    const msgs=lm();
    if(!msgs.find(x=>x.id===m.id)){msgs.push(m);sm(msgs);appendBub(m,true)}
    return;
  }
}

function renderAll(){
  const box=document.getElementById('msgs');
  box.innerHTML='';msgMap={};
  lm().forEach(m=>appendBub(m,false));
  box.scrollTop=box.scrollHeight;
}

function appendBub(m,scroll){
  const mine=m.name===myName;
  const col=nc(m.name);
  const w=document.createElement('div');
  w.className='bw '+(mine?'mine':'other');
  w.dataset.id=m.id;
  let content='';
  if(m.type==='image'){
    content='<img src="'+m.data+'" onclick="viewImg(this.src)" loading="lazy">';
  } else {
    content=esc(m.text).replace(/\n/g,'<br>');
  }
  w.innerHTML=
    '<div class="meta"><span class="nm" style="color:'+col+'">'+esc(m.name)+'</span></div>'+
    '<div class="bubble" onclick="toggleRx(event,\''+m.id+'\')">'+
      content+
      '<div class="rxPicker" id="rx_'+m.id+'">'+
        RX.map(r=>'<span onclick="sendRx(event,\''+m.id+'\',\''+r+'\')">'+r+'</span>').join('')+
      '</div>'+
    '</div>'+
    '<div class="rxRow" id="rxrow_'+m.id+'"></div>'+
    '<div class="ts">'+ft(m.ts)+'</div>';
  document.getElementById('msgs').appendChild(w);
  msgMap[m.id]=w;
  if(scroll)w.scrollIntoView({behavior:'smooth',block:'end'});
}

function appendSys(text){
  const d=document.createElement('div');
  d.className='sys';d.textContent=text;
  document.getElementById('msgs').appendChild(d);
  d.scrollIntoView({behavior:'smooth',block:'end'});
}

let openRx=null;
function toggleRx(e,id){
  e.stopPropagation();
  const p=document.getElementById('rx_'+id);
  if(openRx&&openRx!==p)openRx.classList.remove('show');
  p.classList.toggle('show');
  openRx=p.classList.contains('show')?p:null;
}
document.addEventListener('click',()=>{if(openRx){openRx.classList.remove('show');openRx=null;}});

function sendRx(e,msgId,emoji){
  e.stopPropagation();
  if(openRx){openRx.classList.remove('show');openRx=null;}
  ws.send(JSON.stringify({type:'reaction',msgId,emoji,name:myName}));
  applyRx(msgId,emoji,myName);
}
function applyRx(msgId,emoji,from){
  const row=document.getElementById('rxrow_'+msgId);
  if(!row)return;
  let chip=row.querySelector('[data-e="'+emoji+'"]');
  if(chip){chip.dataset.count=parseInt(chip.dataset.count||1)+1;chip.textContent=emoji+' '+chip.dataset.count;}
  else{const c=document.createElement('div');c.className='rxChip';c.dataset.e=emoji;c.dataset.count=1;c.textContent=emoji;c.onclick=()=>sendRxDirect(msgId,emoji);row.appendChild(c);}
}
function sendRxDirect(msgId,emoji){
  ws.send(JSON.stringify({type:'reaction',msgId,emoji,name:myName}));
  applyRx(msgId,emoji,myName);
}

function renderOnline(users){
  const el=document.getElementById('onList');
  if(!users||!users.length){el.innerHTML='No one online';return;}
  el.innerHTML=users.map(u=>'<span class="onChip'+(u===myName?' me':'')+'">'+ esc(u)+'</span>').join(' ');
}

function onType(){
  if(ws&&ws.readyState===1)ws.send(JSON.stringify({type:'typing',name:myName}));
  clearTimeout(typTimer);
  typTimer=setTimeout(()=>{},2000);
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

function sendMsg(){
  const el=document.getElementById('txtInp');
  const text=el.innerText.trim();
  if(!text||!ws||ws.readyState!==1)return;
  const m={type:'chat',id:uid(),name:myName,text,ts:Date.now()};
  ws.send(JSON.stringify(m));
  const msgs=lm();msgs.push(m);sm(msgs);
  appendBub(m,true);
  el.innerText='';
}

function sendImage(input){
  const file=input.files[0];if(!file)return;
  const reader=new FileReader();
  reader.onload=e=>{
    const img=new Image();
    img.onload=()=>{
      const MAX=600;let w=img.width,h=img.height;
      if(w>MAX){h=Math.round(h*MAX/w);w=MAX;}
      const cv=document.createElement('canvas');cv.width=w;cv.height=h;
      cv.getContext('2d').drawImage(img,0,0,w,h);
      const data=cv.toDataURL('image/jpeg',0.7);
      const m={type:'image',id:uid(),name:myName,data,ts:Date.now()};
      ws.send(JSON.stringify(m));
      const msgs=lm();msgs.push(m);sm(msgs);
      appendBub(m,true);
    };
    img.src=e.target.result;
  };
  reader.readAsDataURL(file);input.value='';
}

function viewImg(src){document.getElementById('viewImg').src=src;document.getElementById('imgView').classList.add('show');}
function showLogs(){document.getElementById('logModal').classList.add('show');fetchLogs();}
function fetchLogs(){fetch('/logs').then(r=>r.text()).then(t=>{const el=document.getElementById('logContent');el.textContent=t||'No logs.';el.scrollTop=el.scrollHeight;}).catch(()=>{document.getElementById('logContent').textContent='Failed to fetch.';});}
function clearChat(){if(confirm('Clear all messages?')){localStorage.removeItem(SK);document.getElementById('msgs').innerHTML='';msgMap={};}}
window.onload=()=>document.getElementById('nameInp').focus();
</script>
</body>
</html>
)rawhtml";

void handleRoot()    { httpServer.send_P(200,"text/html",HTML); }
void handleLogs()    { httpServer.send(200,"text/plain",getLogs()); }
void handleNotFound(){ httpServer.sendHeader("Location","http://192.168.4.1/",true); httpServer.send(302,"text/plain",""); }

void setup() {
  Serial.begin(115200); delay(400);
  addLog("=== IBMOVS ESP32-S3 Boot ===");
  u8g2.begin(); oledDraw(); addLog("[OLED] OK");
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP,AP_IP,AP_SUBNET);
  WiFi.softAP(AP_SSID,AP_PASS);
  addLog("[WiFi] SoftAP: " + String(AP_SSID) + " @ 192.168.4.1");
  dnsServer.start(53,"*",AP_IP); addLog("[DNS] Captive portal OK");
  httpServer.on("/",handleRoot);
  httpServer.on("/chat",handleRoot);
  httpServer.on("/logs",handleLogs);
  httpServer.onNotFound(handleNotFound);
  httpServer.begin(); addLog("[HTTP] Port 80 OK");
  wsServer.begin(); wsServer.onEvent(wsEvent);
  addLog("[WS] Port 81 OK");
  startTime=millis(); addLog("[BOOT] All systems GO!");
}

void loop() {
  dnsServer.processNextRequest();
  httpServer.handleClient();
  wsServer.loop();
  static unsigned long lo=0;
  if(millis()-lo>1000){lo=millis();oledDraw();}
}
