/*
3.3V)
 *
 * LIBRARIES (Arduino Library Manager):
 *   - DHT sensor library       (Adafruit)
 *   - Adafruit Unified Sensor
 *   - ESPAsyncWebServer        (by ESP Async Web Server)
 *   - AsyncTCP                 (dependency)
 *   - Firebase ESP Client      (by Mobizt) ← NEW
 *
 * HOW TO ACCESS:
 *   LOCAL  → http://<ESP32_IP>/          (same LAN, no data used)
 *   REMOTE → Open your Firebase dashboard HTML (separate file)
 *            or point any browser at your Firebase DB URL
 *
 * IMPORTANT — fill in your Firebase credentials below.
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include "DHT.h"

// Firebase — using raw HTTPS REST API (no extra library needed,
// keeps binary small and avoids version conflicts)
// The Mobizt library is great but the REST approach below uses
// zero extra RAM and works on any ESP32 Arduino core.

// ─── USER CONFIGURATION ──────────────────────────────────────────────────

// Default WiFi (overridden by saved prefs or /wifi page)
String WIFI_SSID     = "5300341411";
String WIFI_PASSWORD = "angelchel1414";

// Google Apps Script (unchanged — still logs to Sheets)
const char* GOOGLE_SCRIPT_URL =
  "https://script.google.com/macros/s/AKfycbxHWbf0ks2ZSI9H1z3BqYVSXKmx_geBHuyBTbkIKt8X4KPYpbmcwP3jmC5d7Ebt6ITy-w/exec";

// ── Firebase Realtime Database ───────────────────────────────────────────
// Your RTDB host (no https://, no trailing slash)
const char* FIREBASE_HOST   = "thermowatch0-default-rtdb.asia-southeast1.firebasedatabase.app";
// Database secret OR leave blank if rules are ".read/.write: true"
// (you showed open rules so leave this empty)
const char* FIREBASE_SECRET = "";
// Path in the DB where live data is written
const char* FIREBASE_PATH   = "/live";
// ─────────────────────────────────────────────────────────────────────────

// ── Open-Meteo (Cavite, PH) ──────────────────────────────────────────────
// Same coords as your Apps Script
const float CAVITE_LAT = 14.4791;
const float CAVITE_LON = 120.8970;
// ─────────────────────────────────────────────────────────────────────────

#define DHT_PIN          4
#define DHT_TYPE         DHT11
#define LOG_INTERVAL_MS  (5UL * 60UL * 1000UL)   // 5 minutes
#define HISTORY_MAX      50

// AP fallback SSID (shown when WiFi fails after credential change)
const char* AP_SSID = "ThermoWatch-Setup";

DHT            dht(DHT_PIN, DHT_TYPE);
AsyncWebServer server(80);
Preferences    prefs;

// ── Reading history ───────────────────────────────────────────────────────
struct Reading {
  String time;
  float  temp;
  float  humidity;
  float  heatIndex;
  String status;
};
Reading history[HISTORY_MAX];
int     historyCount = 0;
int     historyHead  = 0;

// ── Latest values ─────────────────────────────────────────────────────────
float   latestTemp      = NAN;
float   latestHumidity  = NAN;
float   latestHeatIndex = NAN;
String  latestStatus    = "—";
String  latestTime      = "—";
int     totalSent       = 0;
int     totalFailed     = 0;
unsigned long lastLogTime = 0;
bool    firstRun          = true;
bool    apMode            = false;   // true when running as Access Point fallback

// ── AP timeout / WiFi cycling ─────────────────────────────────────────────
// When in AP mode and nobody connects within AP_TIMEOUT_MS, abandon AP and
// cycle through: new creds → original creds → AP again. Repeat forever.
#define AP_TIMEOUT_MS      (3UL * 60UL * 1000UL)  // 3 minutes in AP before reverting to main WiFi
unsigned long apStartTime  = 0;       // millis() when AP mode started
String  fallbackSSID       = "";      // always stores the LAST KNOWN WORKING WiFi
String  fallbackPass       = "";      // so we can revert even when outside

// ─────────────────────────────────────────────────────────────────────────
// HTML — Dashboard
// ─────────────────────────────────────────────────────────────────────────
const char DASHBOARD_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ThermoWatch — ESP32</title>
<style>
@import url('https://fonts.googleapis.com/css2?family=Space+Mono:wght@400;700&family=DM+Sans:wght@300;400;500;600&display=swap');
:root{
  --bg:#0d1117;--surface:#161b22;--surface2:#21262d;
  --border:#30363d;--text:#e6edf3;--muted:#8b949e;
  --green:#3fb950;--orange:#f0883e;--red:#f85149;
  --blue:#58a6ff;--yellow:#d29922;
}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'DM Sans',sans-serif;background:var(--bg);color:var(--text);min-height:100vh}
.topbar{display:flex;align-items:center;justify-content:space-between;padding:14px 24px;
  border-bottom:1px solid var(--border);background:var(--surface)}
.logo{font-family:'Space Mono',monospace;font-size:15px;color:var(--blue);letter-spacing:.05em}
.logo span{color:var(--muted)}
.wifi-badge{display:flex;align-items:center;gap:7px;font-size:12px;color:var(--muted)}
.wifi-dot{width:8px;height:8px;border-radius:50%;background:var(--green);
  box-shadow:0 0 6px var(--green);animation:pulse 2s infinite}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.4}}
.wifi-dot.offline{background:var(--red);box-shadow:0 0 6px var(--red);animation:none}
.container{max-width:960px;margin:0 auto;padding:24px 20px}
.grid4{display:grid;grid-template-columns:repeat(auto-fit,minmax(190px,1fr));gap:14px;margin-bottom:20px}
.card{background:var(--surface);border:1px solid var(--border);border-radius:10px;padding:18px 20px}
.card-label{font-size:10px;text-transform:uppercase;letter-spacing:.1em;color:var(--muted);margin-bottom:10px}
.card-value{font-family:'Space Mono',monospace;font-size:32px;font-weight:700;line-height:1}
.card-unit{font-size:14px;font-weight:400;color:var(--muted);margin-left:3px}
.card-sub{font-size:11px;color:var(--muted);margin-top:8px}
.badge{display:inline-block;font-size:11px;padding:3px 10px;border-radius:20px;
  font-family:'Space Mono',monospace;margin-top:8px}
.badge-green{background:rgba(63,185,80,.15);color:var(--green)}
.badge-orange{background:rgba(240,136,62,.15);color:var(--orange)}
.badge-red{background:rgba(248,81,73,.15);color:var(--red)}
.badge-blue{background:rgba(88,166,255,.15);color:var(--blue)}
.section-title{font-size:11px;text-transform:uppercase;letter-spacing:.1em;
  color:var(--muted);margin-bottom:12px;font-family:'Space Mono',monospace}
.info-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:14px;margin-bottom:20px}
.info-card{background:var(--surface);border:1px solid var(--border);border-radius:10px;padding:14px 18px}
.info-row{display:flex;justify-content:space-between;align-items:center;
  padding:6px 0;border-bottom:1px solid var(--border);font-size:13px}
.info-row:last-child{border-bottom:none}
.info-key{color:var(--muted)}
.info-val{font-family:'Space Mono',monospace;font-size:12px;color:var(--text)}
.table-wrap{background:var(--surface);border:1px solid var(--border);border-radius:10px;overflow:hidden;margin-bottom:20px}
table{width:100%;border-collapse:collapse;font-size:13px}
th{padding:10px 14px;text-align:left;font-size:10px;text-transform:uppercase;
  letter-spacing:.08em;color:var(--muted);border-bottom:1px solid var(--border);
  font-family:'Space Mono',monospace;background:var(--surface2)}
td{padding:9px 14px;border-bottom:1px solid var(--border);font-family:'Space Mono',monospace;font-size:12px}
tr:last-child td{border-bottom:none}
tr:hover td{background:var(--surface2)}
.temp-val{color:var(--orange)}
.hum-val{color:var(--blue)}
.hi-val{color:var(--yellow)}
.btn{display:inline-flex;align-items:center;gap:6px;padding:8px 16px;border-radius:7px;
  font-size:13px;font-family:'DM Sans',sans-serif;cursor:pointer;
  border:1px solid var(--border);background:var(--surface2);color:var(--text);
  text-decoration:none;transition:border-color .15s,background .15s}
.btn:hover{border-color:var(--blue);background:#1c2433}
.btn-primary{background:var(--blue);color:#0d1117;border-color:var(--blue)}
.btn-primary:hover{background:#79b8ff;border-color:#79b8ff}
.toolbar{display:flex;align-items:center;gap:10px;margin-bottom:16px;flex-wrap:wrap}
.refresh-note{font-size:11px;color:var(--muted);margin-left:auto}
.status-bar{font-size:11px;color:var(--muted);text-align:center;padding:12px;
  border-top:1px solid var(--border)}
.ap-banner{background:rgba(240,136,62,.12);border:1px solid rgba(240,136,62,.4);
  border-radius:8px;padding:12px 16px;margin-bottom:16px;font-size:13px;color:var(--orange)}
.signal-bar{display:inline-flex;align-items:flex-end;gap:2px;height:14px;vertical-align:middle;margin-right:4px}
.sb{width:3px;border-radius:1px;background:var(--border)}
.sb.on{background:var(--green)}
</style>
</head>
<body>
<div class="topbar">
  <div class="logo">Thermo<span>Watch</span> <span style="font-size:11px;color:var(--muted)">// ESP32 v2</span></div>
  <div class="wifi-badge">
    <div class="wifi-dot" id="wdot"></div>
    <span id="wlabel">Checking...</span>
    <span id="rssi-bars" class="signal-bar">
      <div class="sb" id="sb1" style="height:4px"></div>
      <div class="sb" id="sb2" style="height:7px"></div>
      <div class="sb" id="sb3" style="height:10px"></div>
      <div class="sb" id="sb4" style="height:14px"></div>
    </span>
    <span id="rssi-val" style="font-size:11px;"></span>
  </div>
</div>

<div class="container">
  <div id="ap-banner" class="ap-banner" style="display:none">
    ⚠️ <b>AP Fallback Mode</b> — ESP32 could not connect to WiFi.
    Connect to <b>ThermoWatch-Setup</b> and visit <b>http://192.168.4.1/wifi</b> to fix credentials.
    No data will be sent to Firebase or Google Sheets until WiFi is restored.
  </div>

  <div class="toolbar">
    <span class="section-title">Live readings</span>
    <span id="lastUpdate" class="refresh-note">—</span>
    <a href="/wifi" class="btn">&#9881; WiFi Settings</a>
    <button onclick="fetchData()" class="btn btn-primary">&#8635; Refresh</button>
  </div>

  <div class="grid4">
    <div class="card">
      <div class="card-label">Temperature</div>
      <div class="card-value" id="val-temp">—<span class="card-unit">°C</span></div>
      <div class="card-sub" id="sub-temp-f">— °F</div>
      <span class="badge badge-orange" id="badge-temp">—</span>
    </div>
    <div class="card">
      <div class="card-label">Humidity</div>
      <div class="card-value" id="val-hum">—<span class="card-unit">%</span></div>
      <div class="card-sub">Relative humidity</div>
      <span class="badge badge-blue" id="badge-hum">—</span>
    </div>
    <div class="card">
      <div class="card-label">Heat Index</div>
      <div class="card-value" id="val-hi">—<span class="card-unit">°C</span></div>
      <div class="card-sub" id="hi-source">Open-Meteo / Cavite</div>
      <span class="badge" id="badge-status">—</span>
    </div>
    <div class="card">
      <div class="card-label">Last Reading</div>
      <div class="card-value" style="font-size:18px" id="val-time">—</div>
      <div class="card-sub">Logs every 5 min</div>
      <div style="margin-top:8px;font-size:11px;color:var(--muted)">
        Sent: <span id="stat-sent" style="color:var(--green)">0</span> &nbsp;
        Failed: <span id="stat-fail" style="color:var(--red)">0</span>
      </div>
    </div>
  </div>

  <div class="section-title" style="margin-bottom:12px">Device info</div>
  <div class="info-grid">
    <div class="info-card">
      <div class="info-row"><span class="info-key">MAC Address</span><span class="info-val" id="di-mac">—</span></div>
      <div class="info-row"><span class="info-key">IP Address</span><span class="info-val" id="di-ip">—</span></div>
      <div class="info-row"><span class="info-key">Gateway</span><span class="info-val" id="di-gw">—</span></div>
    </div>
    <div class="info-card">
      <div class="info-row"><span class="info-key">WiFi SSID</span><span class="info-val" id="di-ssid">—</span></div>
      <div class="info-row"><span class="info-key">Signal (RSSI)</span><span class="info-val" id="di-rssi">—</span></div>
      <div class="info-row"><span class="info-key">Uptime</span><span class="info-val" id="di-uptime">—</span></div>
    </div>
  </div>

  <div class="section-title" style="margin-bottom:12px">Reading history (last 50)</div>
  <div class="table-wrap">
    <table>
      <thead><tr><th>#</th><th>Time</th><th>Temp °C</th><th>Humidity %</th><th>Heat Index *</th><th>Status</th></tr></thead>
      <tbody id="history-body"><tr><td colspan="6" style="text-align:center;color:var(--muted);padding:20px">Loading...</td></tr></tbody>
    </table>
  </div>
  <p style="font-size:11px;color:var(--muted);margin-bottom:20px">
    * Heat Index sourced from Open-Meteo weather API (Cavite, PH). Falls back to DHT11 calculation if API is unreachable.
  </p>
</div>

<div class="status-bar">
  ThermoWatch v2 &nbsp;·&nbsp; ESP32 + DHT11 &nbsp;·&nbsp;
  Auto-refresh every 30 s &nbsp;·&nbsp;
  <a href="/wifi" style="color:var(--blue)">Change WiFi</a> &nbsp;·&nbsp;
  <a href="/api/data" style="color:var(--blue)">JSON API</a>
</div>

<script>
function fetchData(){
  fetch('/api/data')
    .then(r=>r.json())
    .then(d=>{
      document.getElementById('val-temp').innerHTML = d.temp.toFixed(1)+'<span class="card-unit">°C</span>';
      document.getElementById('val-hum').innerHTML  = d.humidity.toFixed(1)+'<span class="card-unit">%</span>';
      document.getElementById('val-hi').innerHTML   = d.heatIndex.toFixed(1)+'<span class="card-unit">°C</span>';
      document.getElementById('val-time').textContent = d.lastTime;
      document.getElementById('sub-temp-f').textContent = (d.temp*9/5+32).toFixed(1)+' °F';
      document.getElementById('stat-sent').textContent = d.totalSent;
      document.getElementById('stat-fail').textContent = d.totalFailed;
      document.getElementById('lastUpdate').textContent = 'Updated: '+new Date().toLocaleTimeString();
      document.getElementById('hi-source').textContent = d.hiSource || 'Open-Meteo / Cavite';

      const sc = statusColor(d.status);
      const bs = document.getElementById('badge-status');
      bs.textContent = d.status; bs.className = 'badge badge-'+sc;
      document.getElementById('badge-temp').textContent = d.temp>=32?'Hot':d.temp>=28?'Warm':'Cool';
      document.getElementById('badge-hum').textContent  = d.humidity>=80?'High':d.humidity>=60?'Moderate':'Low';

      document.getElementById('di-mac').textContent  = d.mac;
      document.getElementById('di-ip').textContent   = d.ip;
      document.getElementById('di-gw').textContent   = d.gateway;
      document.getElementById('di-ssid').textContent = d.ssid;
      document.getElementById('di-rssi').textContent = d.rssi+' dBm';
      const up=d.uptime;
      const h=Math.floor(up/3600),m=Math.floor((up%3600)/60),s=up%60;
      document.getElementById('di-uptime').textContent=h+'h '+m+'m '+s+'s';

      const online=d.wifiConnected;
      document.getElementById('wdot').className='wifi-dot'+(online?'':' offline');
      document.getElementById('wlabel').textContent=online?d.ssid:'AP Mode';
      document.getElementById('rssi-val').textContent=online?d.rssi+' dBm':'';
      if(online) setSignalBars(d.rssi);
      document.getElementById('ap-banner').style.display=d.apMode?'block':'none';

      const tbody=document.getElementById('history-body');
      if(d.history&&d.history.length){
        tbody.innerHTML=d.history.map((r,i)=>
          `<tr>
            <td style="color:var(--muted)">${d.history.length-i}</td>
            <td>${r.time}</td>
            <td class="temp-val">${r.temp.toFixed(1)}</td>
            <td class="hum-val">${r.humidity.toFixed(1)}</td>
            <td class="hi-val">${r.heatIndex.toFixed(1)}</td>
            <td><span class="badge badge-${statusColor(r.status)}" style="padding:2px 8px;font-size:10px">${r.status}</span></td>
          </tr>`
        ).join('');
      } else {
        tbody.innerHTML='<tr><td colspan="6" style="text-align:center;color:var(--muted);padding:20px">No readings yet</td></tr>';
      }
    })
    .catch(()=>{document.getElementById('wlabel').textContent='Error';});
}

function statusColor(s){
  if(s==='Danger'||s==='Very Hot') return 'red';
  if(s==='Warm') return 'orange';
  if(s==='Comfortable') return 'green';
  return 'blue';
}

function setSignalBars(rssi){
  const lvl=rssi>=-50?4:rssi>=-65?3:rssi>=-75?2:rssi>=-85?1:0;
  for(let i=1;i<=4;i++){
    const el=document.getElementById('sb'+i);
    if(el) el.className='sb'+(i<=lvl?' on':'');
  }
}

fetchData();
setInterval(fetchData,30000);
</script>
</body>
</html>
)rawhtml";

// ─────────────────────────────────────────────────────────────────────────
// HTML — WiFi Manager (unchanged except added AP note)
// ─────────────────────────────────────────────────────────────────────────
const char WIFI_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>WiFi Settings — ThermoWatch</title>
<style>
@import url('https://fonts.googleapis.com/css2?family=Space+Mono:wght@400;700&family=DM+Sans:wght@300;400;500;600&display=swap');
:root{--bg:#0d1117;--surface:#161b22;--surface2:#21262d;--border:#30363d;--text:#e6edf3;--muted:#8b949e;--green:#3fb950;--blue:#58a6ff;--red:#f85149;--orange:#f0883e;}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'DM Sans',sans-serif;background:var(--bg);color:var(--text);min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px}
.panel{background:var(--surface);border:1px solid var(--border);border-radius:12px;padding:32px;width:100%;max-width:440px}
.panel-title{font-family:'Space Mono',monospace;font-size:16px;color:var(--blue);margin-bottom:6px}
.panel-sub{font-size:13px;color:var(--muted);margin-bottom:16px}
.safety-note{background:rgba(63,185,80,.08);border:1px solid rgba(63,185,80,.25);border-radius:8px;padding:11px 14px;margin-bottom:20px;font-size:12px;color:var(--green);line-height:1.5}
.cur-info{background:var(--surface2);border:1px solid var(--border);border-radius:8px;padding:12px 14px;margin-bottom:20px;font-size:13px}
.cur-row{display:flex;justify-content:space-between;padding:4px 0;border-bottom:1px solid var(--border)}
.cur-row:last-child{border-bottom:none}
.cur-key{color:var(--muted)}
.cur-val{font-family:'Space Mono',monospace;font-size:12px}
label{display:block;font-size:12px;text-transform:uppercase;letter-spacing:.08em;color:var(--muted);margin-bottom:6px}
input[type=text],input[type=password]{width:100%;padding:10px 12px;background:var(--surface2);border:1px solid var(--border);border-radius:7px;color:var(--text);font-size:14px;font-family:'DM Sans',sans-serif;margin-bottom:16px;outline:none}
input:focus{border-color:var(--blue)}
.show-pw{display:flex;align-items:center;gap:7px;font-size:12px;color:var(--muted);margin-bottom:20px;cursor:pointer}
.btn{width:100%;padding:11px;border-radius:7px;font-size:14px;font-weight:500;font-family:'DM Sans',sans-serif;cursor:pointer;border:none;background:var(--blue);color:#0d1117;transition:background .15s}
.btn:hover{background:#79b8ff}
.btn-back{background:var(--surface2);color:var(--text);border:1px solid var(--border);margin-top:10px;font-size:13px}
.btn-back:hover{border-color:var(--blue);background:#1c2433}
.msg{padding:10px 14px;border-radius:7px;font-size:13px;margin-bottom:16px;display:none;line-height:1.5}
.msg.ok{background:rgba(63,185,80,.15);color:var(--green);border:1px solid rgba(63,185,80,.3);display:block}
.msg.err{background:rgba(248,81,73,.15);color:var(--red);border:1px solid rgba(248,81,73,.3);display:block}
</style>
</head>
<body>
<div class="panel">
  <div class="panel-title">&#9881; WiFi Settings</div>
  <div class="panel-sub">Change the WiFi network without reflashing.</div>

  <div class="safety-note">
    🛡️ <b>Safety Net Active</b><br>
    If the new credentials fail, the ESP32 automatically starts an Access Point:<br>
    <b>SSID: ThermoWatch-Setup</b> (no password)<br>
    Connect to it and open <b>http://192.168.4.1/wifi</b> to try again.
    You will never be locked out.
  </div>

  <div class="cur-info">
    <div class="cur-row"><span class="cur-key">Current SSID</span><span class="cur-val" id="cur-ssid">—</span></div>
    <div class="cur-row"><span class="cur-key">Status</span><span class="cur-val" id="cur-status">—</span></div>
    <div class="cur-row"><span class="cur-key">IP Address</span><span class="cur-val" id="cur-ip">—</span></div>
    <div class="cur-row"><span class="cur-key">Signal</span><span class="cur-val" id="cur-rssi">—</span></div>
  </div>

  <div id="msg" class="msg"></div>

  <form onsubmit="saveWifi(event)">
    <label>New WiFi SSID</label>
    <input type="text" id="new-ssid" placeholder="Enter network name" autocomplete="off">
    <label>Password</label>
    <input type="password" id="new-pass" placeholder="Enter password" autocomplete="off">
    <label class="show-pw">
      <input type="checkbox" onchange="document.getElementById('new-pass').type=this.checked?'text':'password'">
      Show password
    </label>
    <button type="submit" class="btn">Save &amp; Reconnect</button>
  </form>
  <button onclick="location.href='/'" class="btn btn-back">&#8592; Back to Dashboard</button>
</div>

<script>
fetch('/api/data').then(r=>r.json()).then(d=>{
  document.getElementById('cur-ssid').textContent   = d.ssid;
  document.getElementById('cur-status').textContent = d.apMode ? 'AP Fallback Mode' : (d.wifiConnected ? 'Connected' : 'Disconnected');
  document.getElementById('cur-status').style.color = d.wifiConnected&&!d.apMode ? '#3fb950' : '#f85149';
  document.getElementById('cur-ip').textContent     = d.ip;
  document.getElementById('cur-rssi').textContent   = d.apMode ? 'N/A' : d.rssi + ' dBm';
});

function saveWifi(e){
  e.preventDefault();
  const ssid=document.getElementById('new-ssid').value.trim();
  const pass=document.getElementById('new-pass').value;
  if(!ssid){showMsg('SSID cannot be empty.','err');return;}
  showMsg('Saving and reconnecting — if you lose connection, connect to <b>ThermoWatch-Setup</b> (no password) and visit http://192.168.4.1/wifi','ok');
  fetch('/api/setwifi?ssid='+encodeURIComponent(ssid)+'&pass='+encodeURIComponent(pass))
    .then(r=>r.json())
    .then(d=>{
      if(!d.ok) showMsg('Error: '+d.msg,'err');
    })
    .catch(()=>{
      showMsg('Connection dropped (expected). If new WiFi works, find the new IP.<br>Otherwise join <b>ThermoWatch-Setup</b> → <b>http://192.168.4.1/wifi</b>','ok');
    });
}
function showMsg(txt,cls){
  const el=document.getElementById('msg');
  el.innerHTML=txt; el.className='msg '+cls;
}
</script>
</body>
</html>
)rawhtml";

// ─────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────

void printSeparator(char c='-',int n=52){for(int i=0;i<n;i++)Serial.print(c);Serial.println();}

String getStatus(float hi){
  if(hi>=54) return "Danger";
  if(hi>=42) return "Very Hot";
  if(hi>=33) return "Warm";
  if(hi>=27) return "Comfortable";
  return "Cool";
}

// Fallback Rothfusz heat index (used only if Open-Meteo is unreachable)
float calcHeatIndexFallback(float tempC, float humidity){
  if(tempC<27||humidity<40) return tempC;
  float T=tempC*9.0/5.0+32.0, R=humidity;
  float HI=-42.379
    +2.04901523*T+10.14333127*R
    -0.22475541*T*R-0.00683783*T*T-0.05481717*R*R
    +0.00122874*T*T*R+0.00085282*T*R*R
    -0.00000199*T*T*R*R;
  if(R<13&&T>=80&&T<=112) HI-=((13.0-R)/4.0)*sqrt((17.0-abs(T-95.0))/17.0);
  if(R>85&&T>=80&&T<=87)  HI+=((R-85.0)/10.0)*((87.0-T)/5.0);
  return (HI-32.0)*5.0/9.0;
}

String formatUptime(unsigned long ms){
  unsigned long s=ms/1000;
  return String(s/3600)+"h "+String((s%3600)/60)+"m "+String(s%60)+"s";
}

void addToHistory(float t,float h,float hi,String st,String tm){
  history[historyHead]={tm,t,h,hi,st};
  historyHead=(historyHead+1)%HISTORY_MAX;
  if(historyCount<HISTORY_MAX) historyCount++;
}

// ─────────────────────────────────────────────────────────────────────────
// Open-Meteo heat index fetch
// Returns NAN if the fetch fails (fallback will be used)
// ─────────────────────────────────────────────────────────────────────────
String hiSource = "—";

float fetchOpenMeteoHeatIndex(){
  Serial.println("[OpenMeteo] Fetching apparent temperature for Cavite...");
  if(WiFi.status()!=WL_CONNECTED){
    Serial.println("  [OpenMeteo] WiFi not connected — skipping.");
    return NAN;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  // Use the "current_weather" style URL — simpler, smaller response
  String url = "https://api.open-meteo.com/v1/forecast";
  url += "?latitude="  + String(CAVITE_LAT, 4);
  url += "&longitude=" + String(CAVITE_LON, 4);
  url += "&current=apparent_temperature";
  url += "&timezone=Asia%2FManila";

  http.begin(client, url);
  http.setTimeout(12000);
  int code = http.GET();
  String body = http.getString();
  http.end();

  Serial.printf("  [OpenMeteo] HTTP %d | body len: %d\n", code, body.length());
  if(code!=200){
    Serial.println("  [OpenMeteo] Failed — will use fallback calculation.");
    return NAN;
  }

  // The response looks like:
  // {"latitude":...,"current_units":{...,"apparent_temperature":"°C"},
  //  "current":{"time":"...","interval":900,"apparent_temperature":33.2}}
  //
  // We need the value INSIDE "current":{...}, NOT the one in "current_units".
  // Strategy: find "current":{ first, then search for "apparent_temperature" after that.

  int currentBlock = body.indexOf("\"current\":{");
  if(currentBlock < 0) currentBlock = body.indexOf("\"current\": {");
  if(currentBlock < 0){
    Serial.println("  [OpenMeteo] 'current' block not found. Raw: " + body.substring(0,200));
    return NAN;
  }

  // Search for the key only after the "current":{ position
  int idx = body.indexOf("\"apparent_temperature\":", currentBlock);
  if(idx < 0){
    Serial.println("  [OpenMeteo] 'apparent_temperature' not found in current block.");
    return NAN;
  }

  int start = idx + strlen("\"apparent_temperature\":");
  while(start < (int)body.length() && (body[start]==' ' || body[start]=='\t')) start++;
  int end = start;
  // include minus sign and decimal point
  while(end < (int)body.length() &&
        (isdigit(body[end]) || body[end]=='.' || body[end]=='-')) end++;

  String valStr = body.substring(start, end);
  if(valStr.length()==0){
    Serial.println("  [OpenMeteo] Value string empty — parse failed.");
    return NAN;
  }

  float val = valStr.toFloat();
  Serial.printf("  [OpenMeteo] apparent_temperature = %.1f C (parsed from '%s')\n",
                val, valStr.c_str());
  return val;
}

// ─────────────────────────────────────────────────────────────────────────
// Firebase RTDB — push via REST (PUT overwrites /live node)
// Data cost: ~1 write per 5 min = tiny. Spark free tier limit: 1 GB transfer/month.
// Each write is ~200 bytes → 200B × 288 writes/day × 30 days ≈ 1.7 MB/month
// ─────────────────────────────────────────────────────────────────────────
bool sendToFirebase(float temp, float hum, float hi, String status){
  Serial.println("[Firebase] Sending to RTDB...");
  if(WiFi.status()!=WL_CONNECTED){
    Serial.println("  [Firebase] WiFi not connected — skip.");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  String url = "https://";
  url += FIREBASE_HOST;
  url += FIREBASE_PATH;
  url += ".json";
  if(strlen(FIREBASE_SECRET)>0){
    url += "?auth=";
    url += FIREBASE_SECRET;
  }

  // Build JSON payload manually
  String payload = "{";
  payload += "\"temp\":"     +String(temp,1)+",";
  payload += "\"humidity\":" +String(hum,1)+",";
  payload += "\"heatIndex\":"+String(hi,1)+",";
  payload += "\"status\":\""  +status+"\",";
  payload += "\"hiSource\":\""+hiSource+"\",";
  payload += "\"uptime\":"   +String(millis()/1000)+",";
  payload += "\"ssid\":\""   +WiFi.SSID()+"\",";
  payload += "\"rssi\":"     +String(WiFi.RSSI())+",";
  payload += "\"ip\":\""     +WiFi.localIP().toString()+"\"";
  payload += "}";

  http.begin(client, url);
  http.addHeader("Content-Type","application/json");
  http.setTimeout(10000);

  unsigned long t0=millis();
  int code=http.PUT(payload);
  String resp=http.getString();
  http.end();

  Serial.printf("  [Firebase] HTTP %d (%lu ms) — %s\n", code, millis()-t0, resp.c_str());
  return (code==200);
}

// ─────────────────────────────────────────────────────────────────────────
// JSON API for local dashboard
// ─────────────────────────────────────────────────────────────────────────
String buildJson(){
  String json="{";
  json+="\"temp\":"       +String(isnan(latestTemp)?0:latestTemp,1)+",";
  json+="\"humidity\":"   +String(isnan(latestHumidity)?0:latestHumidity,1)+",";
  json+="\"heatIndex\":"  +String(isnan(latestHeatIndex)?0:latestHeatIndex,1)+",";
  json+="\"status\":\""   +latestStatus+"\",";
  json+="\"hiSource\":\""  +hiSource+"\",";
  json+="\"lastTime\":\""  +latestTime+"\",";
  json+="\"totalSent\":"  +String(totalSent)+",";
  json+="\"totalFailed\":"+String(totalFailed)+",";
  json+="\"apMode\":"     +String(apMode?"true":"false")+",";
  json+="\"wifiConnected\":";
  json+=(WiFi.status()==WL_CONNECTED ? "true" : "false");
  json+=",";
  json+="\"ssid\":\""     +(apMode?String(AP_SSID):WiFi.SSID())+"\",";
  json+="\"ip\":\""       +(apMode?String("192.168.4.1"):WiFi.localIP().toString())+"\",";
  json+="\"gateway\":\""  +(apMode?String("192.168.4.1"):WiFi.gatewayIP().toString())+"\",";
  json+="\"mac\":\""      +WiFi.macAddress()+"\",";
  json+="\"rssi\":"       +String(apMode?0:WiFi.RSSI())+",";
  json+="\"uptime\":"     +String(millis()/1000)+",";
  json+="\"history\":[";
  int count=0;
  for(int i=historyCount-1;i>=0;i--){
    int idx=(historyHead-1-i+HISTORY_MAX)%HISTORY_MAX;
    Reading& r=history[idx];
    if(count) json+=",";
    json+="{\"time\":\""+r.time+"\",\"temp\":"+String(r.temp,1)+
          ",\"humidity\":"+String(r.humidity,1)+
          ",\"heatIndex\":"+String(r.heatIndex,1)+
          ",\"status\":\""+r.status+"\"}";
    count++;
  }
  json+="]}";
  return json;
}

// ─────────────────────────────────────────────────────────────────────────
// WiFi connection helpers
// ─────────────────────────────────────────────────────────────────────────

bool connectWifi(String ssid, String pass, int timeoutSec=20){
  Serial.printf("[WiFi] Connecting to: %s\n", ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  int ms=0;
  while(WiFi.status()!=WL_CONNECTED && ms<timeoutSec*1000){
    delay(500); Serial.print("."); ms+=500;
  }
  Serial.println();
  if(WiFi.status()==WL_CONNECTED){
    Serial.printf("[WiFi] Connected! IP: %s | RSSI: %d dBm\n",
      WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
  }
  Serial.println("[WiFi] Connection FAILED.");
  return false;
}

// Start Access Point fallback — always reachable at 192.168.4.1
void startAPFallback(){
  Serial.println("[WiFi] Starting AP fallback: " + String(AP_SSID));
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID);   // No password — open AP
  Serial.printf("[WiFi] AP IP: %s\n", WiFi.softAPIP().toString().c_str());
  Serial.printf("[WiFi] AP will timeout in %lu minutes then revert to main WiFi.\n", AP_TIMEOUT_MS/60000);
  apMode     = true;
  apStartTime = millis();
}

// Permanently revert to the main WiFi (fallbackSSID).
// Saves it to flash so next reboot also uses it. Called once after AP timeout.
void revertToMainWifi(){
  Serial.printf("[WiFi] Reverting to main WiFi: %s\n", fallbackSSID.c_str());
  WiFi.softAPdisconnect(true);
  apMode = false;

  // Overwrite flash with the known-good credentials so they survive a reboot
  prefs.begin("wifi",false);
  prefs.putString("ssid", fallbackSSID);
  prefs.putString("pass", fallbackPass);
  prefs.end();

  WIFI_SSID     = fallbackSSID;
  WIFI_PASSWORD = fallbackPass;

  bool ok = connectWifi(fallbackSSID, fallbackPass, 20);
  if(ok){
    Serial.printf("[WiFi] Back on main WiFi! IP: %s\n",
      WiFi.localIP().toString().c_str());
  } else {
    // Main WiFi also down (power cut, etc.) — just stay disconnected and keep running.
    // The loop will NOT retry; DHT11 still reads, cloud logging skipped until reconnected.
    Serial.println("[WiFi] Main WiFi unreachable right now — running offline.");
    Serial.println("[WiFi] Will reconnect automatically when it comes back.");
  }
}

// ─────────────────────────────────────────────────────────────────────────
// Google Sheets sender (unchanged logic)
// ─────────────────────────────────────────────────────────────────────────
bool sendToGoogleSheets(float temp, float hum, float hi){
  Serial.println("[Sheets] Sending to Google Sheets...");
  if(WiFi.status()!=WL_CONNECTED){
    Serial.println("  [Sheets] WiFi down — skip.");
    return false;
  }

  String url = String(GOOGLE_SCRIPT_URL);
  url += "?temp="     +String(temp,1);
  url += "&humidity=" +String(hum,1);
  url += "&heatIndex="+String(hi,1);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, url);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000);

  unsigned long t0=millis();
  int code=http.GET();
  String body=http.getString();
  http.end();

  Serial.printf("  [Sheets] HTTP %d (%lu ms) — %s\n", code, millis()-t0, body.c_str());
  return (code==200 && body.indexOf("SUCCESS")>=0);
}

// ─────────────────────────────────────────────────────────────────────────
// Setup
// ─────────────────────────────────────────────────────────────────────────
void setup(){
  Serial.begin(115200);
  delay(800);
  printSeparator('=');
  Serial.println("  ThermoWatch v2 — Firebase + Open-Meteo + AP Safety Net");
  printSeparator('=');

  // Load saved WiFi credentials
  prefs.begin("wifi", false);
  String savedSSID = prefs.getString("ssid","");
  String savedPass = prefs.getString("pass","");
  if(savedSSID.length()>0){
    Serial.printf("[Prefs] Loaded: %s\n", savedSSID.c_str());
    WIFI_SSID=savedSSID; WIFI_PASSWORD=savedPass;
  } else {
    Serial.println("[Prefs] No saved WiFi — using sketch defaults.");
  }
  prefs.end();

  // DHT11 init
  Serial.printf("[DHT11] Init on GPIO %d\n", DHT_PIN);
  dht.begin();
  delay(2000);

  // Connect to WiFi — fall back to AP if it fails.
  // On first boot with default creds, record them as the fallback.
  fallbackSSID = WIFI_SSID;
  fallbackPass = WIFI_PASSWORD;
  bool wifiOK = connectWifi(WIFI_SSID, WIFI_PASSWORD);
  if(wifiOK){
    apMode = false;
    // Record this working network as the fallback
    fallbackSSID = WIFI_SSID;
    fallbackPass = WIFI_PASSWORD;
  } else {
    startAPFallback();
  }

  // ── Web server routes ────────────────────────────────────────────────
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req){
    req->send_P(200,"text/html",DASHBOARD_HTML);
  });

  server.on("/wifi", HTTP_GET, [](AsyncWebServerRequest* req){
    req->send_P(200,"text/html",WIFI_HTML);
  });

  server.on("/api/data", HTTP_GET, [](AsyncWebServerRequest* req){
    req->send(200,"application/json",buildJson());
  });

  // Save WiFi credentials — now with AP fallback safety net
  server.on("/api/setwifi", HTTP_GET, [](AsyncWebServerRequest* req){
    if(!req->hasParam("ssid")){
      req->send(400,"application/json","{\"ok\":false,\"msg\":\"Missing ssid\"}");
      return;
    }
    String newSSID = req->getParam("ssid")->value();
    String newPass = req->hasParam("pass") ? req->getParam("pass")->value() : "";

    Serial.printf("[WiFiMgr] New credentials: SSID=%s\n", newSSID.c_str());

    // Remember the CURRENT working credentials before touching anything
    if(WiFi.status()==WL_CONNECTED){
      fallbackSSID = WIFI_SSID;
      fallbackPass = WIFI_PASSWORD;
      Serial.printf("[WiFiMgr] Main WiFi saved as fallback: %s\n", fallbackSSID.c_str());
    }

    // Do NOT save new credentials to flash yet —
    // only save them permanently if they actually connect successfully.
    req->send(200,"application/json","{\"ok\":true}");
    delay(500);

    WiFi.disconnect(); delay(500);

    Serial.printf("[WiFiMgr] Trying new credentials: %s\n", newSSID.c_str());
    bool ok = connectWifi(newSSID, newPass, 20);

    if(ok){
      // New WiFi works — save it permanently
      apMode    = false;
      WIFI_SSID = newSSID; WIFI_PASSWORD = newPass;
      prefs.begin("wifi",false);
      prefs.putString("ssid", newSSID);
      prefs.putString("pass", newPass);
      prefs.end();
      // Update fallback to new working network
      fallbackSSID = newSSID; fallbackPass = newPass;
      Serial.printf("[WiFiMgr] Connected! IP: %s — new credentials saved.\n",
        WiFi.localIP().toString().c_str());
    } else {
      // New credentials failed — start AP for 3 minutes so user can fix from nearby.
      // After the timeout the loop will call revertToMainWifi() exactly once and stop.
      Serial.println("[WiFiMgr] New credentials FAILED.");
      Serial.println("[WiFiMgr] Starting AP (ThermoWatch-Setup) for 3 minutes...");
      Serial.println("[WiFiMgr] If nobody connects, will revert to main WiFi permanently.");
      startAPFallback();
    }
  });

  server.begin();
  Serial.println("[Server] HTTP server started on port 80.");
  if(apMode){
    Serial.println("[Server] AP mode → http://192.168.4.1");
  } else {
    Serial.printf("[Server] Open browser → http://%s\n",
      WiFi.localIP().toString().c_str());
  }
  printSeparator('=');
}

// ─────────────────────────────────────────────────────────────────────────
// Loop
// ─────────────────────────────────────────────────────────────────────────
unsigned long loopTick=0;

void loop(){
  unsigned long now=millis();
  loopTick++;

  if(loopTick%30==0){
    Serial.printf("[HEARTBEAT] Uptime: %s | Mode: %s | RSSI: %d\n",
      formatUptime(now).c_str(),
      apMode?"AP-Fallback":(WiFi.status()==WL_CONNECTED?"WiFi":"LOST"),
      apMode?0:WiFi.RSSI());
  }

  // ── AP mode watchdog ────────────────────────────────────────────────────
  // Simple, one-shot logic:
  //   • If a client connects to the AP → reset timeout (user is actively fixing it)
  //   • After 3 minutes with no client → revert to main WiFi (5300341411) and STOP.
  //     The new failed credentials are discarded. No more cycling.
  if(apMode){
    unsigned long apElapsed  = now - apStartTime;
    bool          clientHere = (WiFi.softAPgetStationNum() > 0);

    if(clientHere){
      apStartTime = now;   // user is connected — give them full 3 min from now
      if(loopTick%30==0)
        Serial.println("[AP] Client connected — holding AP open.");
    } else if(apElapsed >= AP_TIMEOUT_MS){
      Serial.println("[AP] 3-minute timeout — no client. Reverting to main WiFi.");
      revertToMainWifi();  // one-shot: connects to 5300341411, saves to flash, done
    } else if(loopTick%30==0){
      Serial.printf("[AP] Waiting... %lus / %lus\n", apElapsed/1000, AP_TIMEOUT_MS/1000);
    }
  }

  if(firstRun||(now-lastLogTime>=LOG_INTERVAL_MS)){
    firstRun=false; lastLogTime=now;
    int cycleN=totalSent+totalFailed+1;

    printSeparator();
    Serial.printf("[CYCLE #%d] %s\n",cycleN,formatUptime(now).c_str());
    printSeparator();

    // DHT read with retries
    float h=NAN,t=NAN;
    for(int i=1;i<=5;i++){
      h=dht.readHumidity(); t=dht.readTemperature();
      if(!isnan(h)&&!isnan(t)) break;
      Serial.printf("  [DHT] Attempt %d/5 failed...\n",i);
      delay(2000);
    }
    if(isnan(h)||isnan(t)){
      Serial.println("  [ERROR] DHT11 all retries failed — skipping cycle.");
      totalFailed++;
      delay(1000);
      return;
    }

    // ── Heat Index from Open-Meteo (same source as Apps Script) ─────────
    float hi=fetchOpenMeteoHeatIndex();
    if(isnan(hi)){
      // Fallback: calculate from DHT11 values
      hi=calcHeatIndexFallback(t,h);
      hiSource="Calculated (DHT11 fallback)";
      Serial.printf("  [HI] Fallback calc: %.1f C\n",hi);
    } else {
      hiSource="Open-Meteo (Cavite)";
      Serial.printf("  [HI] Open-Meteo: %.1f C\n",hi);
    }

    String st=getStatus(hi);
    latestTemp=t; latestHumidity=h; latestHeatIndex=hi; latestStatus=st;

    unsigned long s=now/1000;
    char tbuf[10];
    snprintf(tbuf,sizeof(tbuf),"%02lu:%02lu:%02lu",(s/3600)%24,(s%3600)/60,s%60);
    latestTime=String(tbuf);
    addToHistory(t,h,hi,st,latestTime);

    Serial.println();
    Serial.println("  +-----------------------------------+");
    Serial.printf( "  | Sensor Temp  : %6.1f C           |\n",t);
    Serial.printf( "  | Humidity     : %6.1f %%           |\n",h);
    Serial.printf( "  | Heat Index   : %6.1f C %-10s|\n",hi,("("+hiSource+")").c_str());
    Serial.printf( "  | Status       : %-18s  |\n",st.c_str());
    Serial.println("  +-----------------------------------+");
    Serial.println();

    // Only send to cloud if WiFi is connected (not in AP mode)
    if(!apMode && WiFi.status()==WL_CONNECTED){
      // Send to Firebase (low data cost, enables remote dashboard)
      bool fbOK=sendToFirebase(t,h,hi,st);
      Serial.println(fbOK?"[Firebase] SUCCESS":"[Firebase] FAILED");

      // Send to Google Sheets (your Apps Script also re-fetches HI from Open-Meteo)
      bool shOK=sendToGoogleSheets(t,h,hi);
      if(shOK){ totalSent++;   Serial.println("[Sheets] SUCCESS"); }
      else    { totalFailed++; Serial.println("[Sheets] FAILED");  }
    } else {
      Serial.println("[Cloud] AP/offline mode — cloud logging skipped.");
      totalFailed++;
    }

    Serial.printf("[STATS] Sent: %d | Failed: %d\n", totalSent, totalFailed);
    printSeparator();
  }

  delay(1000);
}
