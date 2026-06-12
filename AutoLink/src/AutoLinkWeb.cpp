#include <time.h>
// AutoLinkWeb.cpp — AutoLinkWeb implementation (Arduino/ESP32 only).
//
// Embeds the full dashboard HTML/CSS/JS as a raw string literal, connects
// to WiFi in begin(), starts esp_http_server, registers four endpoints
// (GET /, GET /stats, GET /logs, POST /reset), and runs a 1 Hz esp_timer
// to snapshot AutoLink stats for the /stats handler.
#ifdef ARDUINO

#include "AutoLinkWeb.h"
#include "ALink.h"          // StateToStr()
#include <Arduino.h>
#include <WiFi.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_heap_caps.h"
#include "esp_system.h"   // esp_restart()

namespace autolink {

// ---------------------------------------------------------------------------
// Embedded dashboard — single HTML file, all CSS + JS inline, no CDN deps.
// Dark mobile-first layout; updates via fetch() polling every 1 second.
// ---------------------------------------------------------------------------
static const char HTML_PAGE[] = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<title>AutoLink Monitor</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#f0f2f5;color:#111827;min-height:100vh}
header{background:#ffffff;padding:14px 18px;display:flex;align-items:center;justify-content:space-between;border-bottom:2px solid #d1d5db;position:sticky;top:0;z-index:10;box-shadow:0 2px 8px rgba(0,0,0,.08)}
h1{font-size:18px;font-weight:700;letter-spacing:.2px}
.sub{font-size:13px;color:#6b7280;margin-top:3px}
.pill{padding:6px 16px;border-radius:20px;font-size:13px;font-weight:700;letter-spacing:.5px;transition:background .3s,color .3s}
.ok{background:#d1fae5;color:#065f46}
.swp{background:#fee2e2;color:#991b1b}
.lck{background:#fef9c3;color:#92400e}
main{padding:14px;max-width:540px;margin:0 auto}
.alert{background:#fee2e2;border:1px solid #f87171;border-radius:10px;padding:10px 14px;color:#991b1b;font-size:14px;text-align:center;margin-bottom:12px;display:none}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-bottom:14px}
.card{background:#ffffff;border:1px solid #d1d5db;border-radius:12px;padding:14px;box-shadow:0 1px 3px rgba(0,0,0,.06)}
.lbl{font-size:11px;color:#6b7280;text-transform:uppercase;letter-spacing:.9px;margin-bottom:7px;font-weight:600}
.val{font-size:26px;font-weight:700;font-variant-numeric:tabular-nums;line-height:1.1}
.g{color:#059669}.b{color:#2563eb}.r{color:#dc2626}.a{color:#d97706}
.hint{font-size:12px;color:#6b7280;margin-top:5px}
.row{display:flex;align-items:center;justify-content:space-between;margin-bottom:8px}
.section-lbl{font-size:11px;color:#6b7280;text-transform:uppercase;letter-spacing:.9px;font-weight:600}
.btns{display:flex;gap:7px}
.btn{background:#e5e7eb;color:#374151;border:1px solid #d1d5db;padding:8px 16px;border-radius:8px;font-size:14px;font-weight:600;cursor:pointer;-webkit-tap-highlight-color:transparent}
.btn:active{opacity:.7}
.btn.on{background:#dbeafe;color:#1d4ed8;border-color:#93c5fd}
.btn.rst{background:#fee2e2;color:#dc2626;border-color:#fca5a5}
.btn.rbt{background:#fef3c7;color:#b45309;border-color:#fcd34d}
.log-wrap{position:relative}.log{background:#ffffff;border:1px solid #d1d5db;border-radius:12px 12px 0 0;padding:12px;height:240px;overflow-y:auto;font-family:ui-monospace,'Cascadia Code','Courier New',monospace;font-size:12px;line-height:1.65;-webkit-overflow-scrolling:touch}.log-fill-bar{height:5px;border-radius:0 0 8px 8px;background:linear-gradient(to right,#22c55e var(--pct,0%),#e5e7eb var(--pct,0%));border:1px solid #d1d5db;border-top:none;transition:background 0.3s}.log-overlay{display:none;position:fixed;inset:0;z-index:9999;background:#fff;flex-direction:column}.log-overlay.open{display:flex}.log-overlay .log{flex:1;height:auto;border-radius:0;border:none;border-bottom:1px solid #d1d5db}.log-overlay .log-fill-bar{border-radius:0;border:none;border-top:1px solid #e5e7eb}.log-overlay-hdr{display:flex;align-items:center;justify-content:space-between;padding:8px 14px;border-bottom:1px solid #d1d5db;background:#f9fafb}.log-overlay-hdr>span{font-weight:600;font-size:14px}
.E{color:#dc2626}.I{color:#374151}.D{color:#9ca3af}
.footer{text-align:center;padding:16px;font-size:12px;color:#9ca3af}
</style>
</head>
<body>
<header>
  <div>
    <h1>AutoLink Monitor</h1>
    <div class="sub" id="uptime">connecting&#x2026;</div>
  </div>
  <span class="pill swp" id="pill">SWP</span>
</header>
<main>
  <div class="alert" id="alert">Connection lost &#x2014; check power and WiFi</div>
  <div class="grid">
    <div class="card">
      <div class="lbl">TX Rate</div>
      <div class="val g" id="txbps">&#x2014;</div>
      <div class="hint" id="txtot">total &#x2014;</div>
    </div>
    <div class="card">
      <div class="lbl">RX Rate</div>
      <div class="val b" id="rxbps">&#x2014;</div>
      <div class="hint" id="rxtot">total &#x2014;</div>
    </div>
    <div class="card">
      <div class="lbl">Errors (lifetime)</div>
      <div class="val r" id="errcnt">&#x2014;</div>
      <div class="hint" id="discon">0 disconnects</div>
    </div>
    <div class="card">
      <div class="lbl">WiFi RSSI</div>
      <div class="val a" id="rssi">&#x2014;</div>
      <div class="hint" id="heap">heap &#x2014;</div>
      <div class="hint" id="baud">baud &#x2014;</div>
    </div>
  </div>
  <div class="row" style="margin-bottom:10px">
    <span class="section-lbl">Counters</span>
    <div class="btns">
      <button class="btn rst" id="rbtn" onclick="resetAll()">&#8635; Reset</button>
      <button class="btn rbt" id="rebootBtn" onclick="reboot()">&#9211; Reboot</button>
    </div>
  </div>
  <div class="row">
    <span class="section-lbl">Live Log</span>
    <div class="btns">
      <button class="btn" onclick="clearLog()">Clear</button>
      <button class="btn" id="cbtn" onclick="copyLog()">Copy</button>
      <button class="btn" id="pbtn" onclick="togglePause()">&#9646;&#9646; Pause</button>
      <button class="btn" onclick="openLogFull()" title="Maximize">&#x26F6;</button>
    </div>
  </div>
  <div class="log-wrap">
    <div class="log" id="log"></div>
    <div class="log-fill-bar" id="logFill"></div>
  </div>
  <div class="log-overlay" id="logOverlay">
    <div class="log-overlay-hdr">
      <span>Live Log</span>
      <div class="btns">
        <button class="btn" onclick="clearLog()">Clear</button>
        <button class="btn" id="cbtn2" onclick="copyLog()">Copy</button>
        <button class="btn" id="pbtn2" onclick="togglePause()">&#9646;&#9646; Pause</button>
        <button class="btn" onclick="closeLogFull()" title="Minimize">&#x2715; Close</button>
      </div>
    </div>
    <div class="log" id="logFull"></div>
    <div class="log-fill-bar" id="logFillFull"></div>
  </div>
</main>
<div class="footer"><span id="ver"></span> AutoLink Web Monitor &#x2014; <span id="host"></span></div>
<script>
var logPaused=false,logFullOpen=false,lastSeq=0,fails=0,busy=false;
document.getElementById('host').textContent=location.host;

// Fetch with an AbortController timeout (ms). Prevents stalled requests from
// holding open connections when the ESP is busy or WiFi is reconnecting.
function tfetch(url,opts,ms){
  var c=new AbortController();
  var id=setTimeout(function(){c.abort();},ms||2500);
  var o=Object.assign({signal:c.signal},opts||{});
  return fetch(url,o).then(function(r){clearTimeout(id);return r;},
                           function(e){clearTimeout(id);throw e;});
}

function bps(n){if(n>=1048576)return(n/1048576).toFixed(1)+' MB/s';if(n>=1024)return(n/1024).toFixed(1)+' KB/s';return n+' B/s';}
function bytes(n){if(n>=1073741824)return(n/1073741824).toFixed(2)+' GB';if(n>=1048576)return(n/1048576).toFixed(2)+' MB';if(n>=1024)return(n/1024).toFixed(1)+' KB';return n+' B';}
function hms(s){return[Math.floor(s/3600),Math.floor(s%3600/60),s%60].map(function(x){return('0'+x).slice(-2);}).join(':');}
function set(id,v){document.getElementById(id).textContent=v;}
function show(id){document.getElementById(id).style.display='block';}
function hide(id){document.getElementById(id).style.display='none';}

function setPill(st){var p=document.getElementById('pill');p.className='pill '+st.toLowerCase();p.textContent=st;}

function togglePause(){
  logPaused=!logPaused;
  var lbl=logPaused?'&#9654; Resume':'&#9646;&#9646; Pause';
  var cls=logPaused?'btn on':'btn';
  ['pbtn','pbtn2'].forEach(function(id){var b=document.getElementById(id);if(b){b.innerHTML=lbl;b.className=cls;}});
  if(!logPaused)poll();
}

function clearLog(){document.getElementById('log').innerHTML='';var f=document.getElementById('logFull');if(f)f.innerHTML='';updateFillBar();}

function copyLog(){
  var lines=Array.from(document.getElementById('log').children).map(function(e){return e.textContent;});
  var b=document.getElementById('cbtn');
  var text=lines.join('\n');
  if(navigator.clipboard&&window.isSecureContext){
    navigator.clipboard.writeText(text).then(function(){b.textContent='\u2713 Copied';},function(){fallbackCopy(text,b);});
  }else{fallbackCopy(text,b);}
  setTimeout(function(){b.textContent='Copy';},1500);
}
function fallbackCopy(text,b){
  var ta=document.createElement('textarea');
  ta.value=text;ta.style.position='fixed';ta.style.opacity='0';
  document.body.appendChild(ta);ta.focus();ta.select();
  try{document.execCommand('copy');b.textContent='\u2713 Copied';}
  catch(e){b.textContent='\u2717 Failed';}
  document.body.removeChild(ta);
}

async function resetAll(){
  var b=document.getElementById('rbtn');
  b.textContent='…';
  try{
    var r=await tfetch('/reset',{method:'POST'},2500);
    b.textContent=r.ok?'✓ Done':'✗ Err';
  }catch(e){b.textContent='✗ Err';}
  setTimeout(function(){b.textContent='↺ Reset';},1200);
}

async function reboot(){
  if(!confirm('Reboot the device now? The link will drop and reconnect in a few seconds.'))return;
  clearLog();lastSeq=0;
  var b=document.getElementById('rebootBtn');
  b.textContent='Rebooting…';
  try{
    await tfetch('/reboot',{method:'POST'},2500);
  }catch(e){/* device resets mid-response; an aborted fetch is expected */}
  show('alert');
  var tries=0;
  var iv=setInterval(async function(){
    tries++;
    try{
      var r=await tfetch('/stats',null,1500);
      if(r.ok){clearInterval(iv);location.reload();}
    }catch(e){}
    if(tries>30)clearInterval(iv);
  },1000);
}

function updateFillBar(){
  var t=document.getElementById('log').textContent;
  var pct=Math.min(100,Math.round(t.length/100))+'%';
  ['logFill','logFillFull'].forEach(function(id){var b=document.getElementById(id);if(b)b.style.setProperty('--pct',pct);});
}
function openLogFull(){
  logFullOpen=true;
  var o=document.getElementById('logOverlay');o.classList.add('open');
  var dst=document.getElementById('logFull');
  dst.innerHTML=document.getElementById('log').innerHTML;
  dst.scrollTop=dst.scrollHeight;updateFillBar();
}
function closeLogFull(){logFullOpen=false;document.getElementById('logOverlay').classList.remove('open');}
function appendLog(sev,seq,text){
  if(seq+1>lastSeq)lastSeq=seq+1;
  if(logPaused)return;
  function addTo(p){
    var atEnd=p.scrollHeight-p.scrollTop<=p.clientHeight+12;
    var d=document.createElement('div');d.className=sev;d.textContent=text;p.appendChild(d);
    if(p.textContent.length>10000){while(p.children.length>1&&p.textContent.length>8000)p.removeChild(p.firstChild);}
    if(atEnd)p.scrollTop=p.scrollHeight;
  }
  addTo(document.getElementById('log'));
  if(logFullOpen)addTo(document.getElementById('logFull'));
  updateFillBar();
}

async function poll(){
  if(busy)return;
  busy=true;
  try{
    var r=await tfetch('/stats',null,2500);
    if(!r.ok)throw 0;
    var d=await r.json();
    set('txbps',bps(d.txBps));
    set('txtot','total '+bytes(d.txTotal));
    set('rxbps',bps(d.rxBps));
    set('rxtot','total '+bytes(d.rxTotal));
    set('errcnt',d.errCount);
    set('discon', d.errTotal + (d.errTotal===1?' disconnect':' disconnects'));
    set('rssi',d.rssi+' dBm');
    set('heap','heap '+bytes(d.freeHeap));
    if(d.state==='OK'){
      set('baud', d.baudRate ? d.baudRate.toLocaleString()+' baud' : '?');
    } else if(d.state==='SWP'){
      set('baud', (d.baudRate ? d.baudRate.toLocaleString() : '?')+' \u21c4 sweeping');
    } else if(d.state==='LCK'){
      set('baud', (d.baudRate ? d.baudRate.toLocaleString() : '?')+' \u21c4 locking');
    } else {
      set('baud', d.baudRate ? d.baudRate.toLocaleString()+' baud' : '\u2014');
    }
    set('uptime','up '+hms(d.uptimeS));
    setPill(d.state);
    if(d.version)document.getElementById('ver').textContent='v'+d.version;
    fails=0;hide('alert');
  }catch(e){if(++fails>=3)show('alert');}
  try{
    var r2=await tfetch('/logs?since='+lastSeq,null,2500);
    if(!r2.ok)throw 0;
    var d2=await r2.json();
    d2.lines.forEach(function(l){appendLog(l.sev,l.seq,l.text);});
  }catch(e){}
  busy=false;
}

document.addEventListener('visibilitychange',function(){if(!document.hidden)poll();});
setInterval(poll,1000);
poll();
</script>
</body>
</html>)HTML";

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

AutoLinkWeb::AutoLinkWeb(AutoLink& link) : link_(link) {}

AutoLinkWeb::~AutoLinkWeb() {
    // Shutdown order matters:
    //   1. Stop the timer   — no more stat callbacks touching snap_
    //   2. Stop the server  — no more HTTP handlers touching snap_ or logRing_
    //   3. Clear the sink   — no more logSinkCb calls touching logRing_
    //   4. Free resources
    if (statTimer_) {
        esp_timer_stop(statTimer_);
        esp_timer_delete(statTimer_);
        statTimer_ = nullptr;
    }
    if (server_) {
        httpd_stop(server_);
        server_ = nullptr;
    }
    Log::getLog().clearSink();

    if (snapMtx_) { vSemaphoreDelete(snapMtx_); snapMtx_ = nullptr; }
    if (logMtx_)  { vSemaphoreDelete(logMtx_);  logMtx_  = nullptr; }
    free(logRing_);
    logRing_ = nullptr;
}

// ---------------------------------------------------------------------------
// begin() — connect WiFi, allocate resources, start server
// ---------------------------------------------------------------------------

bool AutoLinkWeb::begin(const char* ssid, const char* pass, uint16_t port) {
    if (enabled_) return true;
    port_ = port;

    Log& log = Log::getLog();
    log.info(TAG, "WiFi connect SSID=\"%s\" passLen=%u", ssid, (unsigned)strlen(pass));

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);

    const uint32_t startMs = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - startMs > WIFI_TIMEOUT_MS) break;
        delay(250);
    }

    if (WiFi.status() != WL_CONNECTED) {
        log.error(TAG, "WiFi connect failed — web monitor disabled");
        return false;
    }
    log.info(TAG, "WiFi connected IP=%s", WiFi.localIP().toString().c_str());

    // ----- allocate resources; clean up everything on any failure -----

    logRing_ = (LogEntry*)calloc(RING_CAP, sizeof(LogEntry));
    snapMtx_ = xSemaphoreCreateMutex();
    logMtx_  = xSemaphoreCreateMutex();

    if (!logRing_ || !snapMtx_ || !logMtx_) {
        log.error(TAG, "resource alloc failed");
        goto fail;
    }

    // Register sink now — ring and mutex are ready; NTP and "Web monitor"
    // log lines below will flow into the ring and appear in the web log panel.
    Log::getLog().setSink(logSinkCb, this);

    // ----- NTP sync — EST/EDT (UTC-5/UTC-4 with auto DST) -----
    // configTime uses the POSIX tz string; SNTP runs in the background.
    // We poll up to 5 s for a valid time; if it doesn't arrive we fall
    // back to uptime timestamps (marked with * in the log).
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
    tzset();
    {
        struct tm ti = {};
        const uint32_t ntpStart = millis();
        while (!getLocalTime(&ti, 0) && millis() - ntpStart < 5000) delay(100);
        if (getLocalTime(&ti, 0)) {
            ntpSynced_ = true;
            log.info(TAG, "NTP synced: %04d-%02d-%02d %02d:%02d:%02d EST/EDT",
                ti.tm_year+1900, ti.tm_mon+1, ti.tm_mday,
                ti.tm_hour, ti.tm_min, ti.tm_sec);
        } else {
            log.info(TAG, "NTP not available — timestamps are uptime (HH:MM:SS*)");
        }
    }


    {   // 1 Hz periodic stats timer
        const esp_timer_create_args_t ta = {
            .callback              = statTimerCb,
            .arg                   = this,
            .dispatch_method       = ESP_TIMER_TASK,
            .name                  = "al_web_stat",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&ta, &statTimer_) != ESP_OK
         || esp_timer_start_periodic(statTimer_, 1000000ULL) != ESP_OK) {
            log.error(TAG, "stat timer create failed");
            goto fail;
        }
    }

    {   // HTTP server
        httpd_config_t cfg   = HTTPD_DEFAULT_CONFIG();
        cfg.server_port      = port_;
        cfg.stack_size       = 6144;
        cfg.task_priority    = 4;
        cfg.max_open_sockets = 3;
        cfg.lru_purge_enable = true;

        if (httpd_start(&server_, &cfg) != ESP_OK) {
            log.error(TAG, "httpd_start failed");
            goto fail;
        }

        const httpd_uri_t r0 = { "/",      HTTP_GET,  handleRoot,  this };
        const httpd_uri_t r1 = { "/stats", HTTP_GET,  handleStats, this };
        const httpd_uri_t r2 = { "/logs",  HTTP_GET,  handleLogs,  this };
        const httpd_uri_t r3 = { "/reset",  HTTP_POST, handleReset,  this };
        const httpd_uri_t r4 = { "/reboot", HTTP_POST, handleReboot, this };
        httpd_register_uri_handler(server_, &r0);
        httpd_register_uri_handler(server_, &r1);
        httpd_register_uri_handler(server_, &r2);
        httpd_register_uri_handler(server_, &r3);
        httpd_register_uri_handler(server_, &r4);
    }

    enabled_ = true;
    log.info(TAG, "Web monitor at http://%s:%u", WiFi.localIP().toString().c_str(), port_);
    return true;

fail:
    if (statTimer_) { esp_timer_stop(statTimer_); esp_timer_delete(statTimer_); statTimer_ = nullptr; }
    if (server_)    { httpd_stop(server_); server_ = nullptr; }
    if (snapMtx_)   { vSemaphoreDelete(snapMtx_); snapMtx_ = nullptr; }
    if (logMtx_)    { vSemaphoreDelete(logMtx_);  logMtx_  = nullptr; }
    free(logRing_);  logRing_ = nullptr;
    return false;
}

String AutoLinkWeb::ip() const {
    return WiFi.localIP().toString();
}

// ---------------------------------------------------------------------------
// 1 Hz stats snapshot — runs in the esp_timer task
// ---------------------------------------------------------------------------

void AutoLinkWeb::statTimerCb(void* arg) {
    AutoLinkWeb* self = (AutoLinkWeb*)arg;

    uint64_t tx, rx, errs;
    self->link_.getStats(tx, rx, errs);

    xSemaphoreTake(self->snapMtx_, portMAX_DELAY);
    // Guard against the app calling resetStats() between samples — clamp to 0.
    self->snap_.txBps    = (tx >= self->prevTx_) ? (uint32_t)(tx - self->prevTx_) : 0;
    self->snap_.rxBps    = (rx >= self->prevRx_) ? (uint32_t)(rx - self->prevRx_) : 0;
    self->snap_.txTotal  = tx;
    self->snap_.rxTotal  = rx;
    self->snap_.errTotal = errs;
    self->snap_.errCount = (uint32_t)self->link_.getLifetimeErrors();
    strncpy(self->snap_.state, StateToStr(self->link_.getState()), 3);
    self->snap_.state[3] = '\0';
    self->snap_.rssi     = (int32_t)WiFi.RSSI();
    self->snap_.freeHeap = esp_get_free_heap_size();
    self->snap_.uptimeS  = millis() / 1000;
    self->snap_.baudRate = self->link_.getCurrentBaud();
    xSemaphoreGive(self->snapMtx_);

    self->prevTx_ = tx;
    self->prevRx_ = rx;
}

// ---------------------------------------------------------------------------
// Log sink — called from Log::emit() in any task
// ---------------------------------------------------------------------------

void AutoLinkWeb::logSinkCb(char sev, const char* tag, const char* msg, void* ctx) {
    AutoLinkWeb* self = (AutoLinkWeb*)ctx;
    if (!self->logRing_) return;
    // 5 ms timeout: if the HTTP log handler holds the mutex briefly, we wait;
    // if something is badly wrong, we drop the entry rather than blocking the caller.
    if (xSemaphoreTake(self->logMtx_, pdMS_TO_TICKS(5)) != pdTRUE) return;

    const uint32_t idx  = self->logHead_ % RING_CAP;
    self->logRing_[idx].seq = self->logHead_;
    self->logRing_[idx].sev = sev;
    char ts[12]; // "HH:MM:SS" or "HH:MM:SS*"
    if (self->ntpSynced_) {
        struct tm ti = {};
        if (getLocalTime(&ti, 0)) {
            snprintf(ts, sizeof(ts), "%02d:%02d:%02d",
                     ti.tm_hour, ti.tm_min, ti.tm_sec);
        } else {
            // NTP was synced but getLocalTime failed (rare) — use uptime
            uint32_t s = millis() / 1000;
            snprintf(ts, sizeof(ts), "%02lu:%02lu:%02lu*",
                     (unsigned long)(s/3600), (unsigned long)(s%3600/60), (unsigned long)(s%60));
        }
    } else {
        // No NTP — uptime with * suffix so the reader knows it's not wall-clock
        uint32_t s = millis() / 1000;
        snprintf(ts, sizeof(ts), "%02lu:%02lu:%02lu*",
                 (unsigned long)(s/3600), (unsigned long)(s%3600/60), (unsigned long)(s%60));
    }
    snprintf(self->logRing_[idx].line, LINE_CAP, "%s %c %s %s", ts, sev, tag, msg);
    self->logHead_++;

    xSemaphoreGive(self->logMtx_);
}

// ---------------------------------------------------------------------------
// HTTP handler: GET /
// ---------------------------------------------------------------------------

esp_err_t AutoLinkWeb::handleRoot(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    // sizeof - 1: exclude the NUL terminator of the string literal.
    httpd_resp_send(req, HTML_PAGE, sizeof(HTML_PAGE) - 1);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// HTTP handler: GET /stats
// ---------------------------------------------------------------------------

esp_err_t AutoLinkWeb::handleStats(httpd_req_t* req) {
    AutoLinkWeb* self = (AutoLinkWeb*)req->user_ctx;

    // Copy the snapshot under the mutex so the browser sees a consistent frame.
    Snapshot s;
    xSemaphoreTake(self->snapMtx_, portMAX_DELAY);
    s = self->snap_;
    xSemaphoreGive(self->snapMtx_);

    char buf[300];
    int  len = snprintf(buf, sizeof(buf),
        "{\"state\":\"%s\",\"errCount\":%lu,\"errTotal\":%llu,"
        "\"txBps\":%lu,\"rxBps\":%lu,"
        "\"txTotal\":%llu,\"rxTotal\":%llu,"
        "\"rssi\":%d,\"freeHeap\":%lu,\"uptimeS\":%lu,"
        "\"baudRate\":%lu,\"version\":\"" AUTOLINK_VERSION "\"}",
        s.state,
        (unsigned long)s.errCount,
        (unsigned long long)s.errTotal,
        (unsigned long)s.txBps,
        (unsigned long)s.rxBps,
        (unsigned long long)s.txTotal,
        (unsigned long long)s.rxTotal,
        (int)s.rssi,
        (unsigned long)s.freeHeap,
        (unsigned long)s.uptimeS,
        (unsigned long)s.baudRate);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// HTTP handler: GET /logs?since=N
// Returns {"lines":[{"seq":N,"sev":"I","text":"..."},...]}
// Client polls with ?since=lastSeq for incremental updates.
// ---------------------------------------------------------------------------

esp_err_t AutoLinkWeb::handleLogs(httpd_req_t* req) {
    AutoLinkWeb* self = (AutoLinkWeb*)req->user_ctx;

    // Parse optional ?since=N query parameter.
    char     query[48] = {};
    uint32_t since     = 0;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[20] = {};
        if (httpd_query_key_value(query, "since", val, sizeof(val)) == ESP_OK) {
            since = (uint32_t)strtoul(val, nullptr, 10);
        }
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr_chunk(req, "{\"lines\":[");

    bool first = true;
    if (xSemaphoreTake(self->logMtx_, pdMS_TO_TICKS(50)) == pdTRUE) {
        const uint32_t total = self->logHead_;
        // Walk only the entries that fit in the ring, starting at 'since'.
        uint32_t start = (total > (uint32_t)RING_CAP) ? (total - RING_CAP) : 0;
        if (since > start) start = since;

        // chunk: JSON wrapper + worst-case 2× escaped line + closing characters.
        char chunk[LINE_CAP * 2 + 64];

        for (uint32_t i = start; i < total; i++) {
            const LogEntry& e = self->logRing_[i % RING_CAP];
            if (e.seq != i) continue; // defensive: slot was overwritten

            if (!first) httpd_resp_sendstr_chunk(req, ",");
            first = false;

            int pos = snprintf(chunk, sizeof(chunk),
                "{\"seq\":%lu,\"sev\":\"%c\",\"text\":\"",
                (unsigned long)i, e.sev);

            // JSON-escape the log line in-place.
            for (const char* p = e.line; *p && pos < (int)(sizeof(chunk) - 4); p++) {
                switch (*p) {
                    case '"':  chunk[pos++] = '\\'; chunk[pos++] = '"';  break;
                    case '\\': chunk[pos++] = '\\'; chunk[pos++] = '\\'; break;
                    case '\n': chunk[pos++] = '\\'; chunk[pos++] = 'n';  break;
                    case '\r': chunk[pos++] = '\\'; chunk[pos++] = 'r';  break;
                    default:   chunk[pos++] = *p;                         break;
                }
            }
            chunk[pos++] = '"';
            chunk[pos++] = '}';
            if (httpd_resp_send_chunk(req, chunk, pos) != ESP_OK) {
                // Client disconnected mid-response; release the mutex and bail.
                xSemaphoreGive(self->logMtx_);
                return ESP_FAIL;
            }
        }
        xSemaphoreGive(self->logMtx_);
    }

    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_send_chunk(req, nullptr, 0); // terminate chunked transfer
    return ESP_OK;
}

// HTTP handler: POST /reset
// Calls resetStats() and resetErrors() on the AutoLink instance, then zeroes
// the sampler's prevTx_/prevRx_ so the next B/s reading is 0 rather than
// a spurious spike caused by the counters restarting from 0.
esp_err_t AutoLinkWeb::handleReset(httpd_req_t* req) {
    AutoLinkWeb* self = (AutoLinkWeb*)req->user_ctx;
    self->link_.resetStats();
    self->link_.resetErrors();
    if (xSemaphoreTake(self->snapMtx_, pdMS_TO_TICKS(50)) == pdTRUE) {
        self->prevTx_ = 0;
        self->prevRx_ = 0;
        xSemaphoreGive(self->snapMtx_);
    }
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, "ok", 2);
    return ESP_OK;
}

// HTTP handler: POST /reboot
// Sends the response first, then restarts the chip after a short delay so the
// "ok" reply reaches the browser before the connection drops. esp_restart()
// never returns; the device boots fresh and the dashboard JS polls /stats
// until it answers again, then reloads.
esp_err_t AutoLinkWeb::handleReboot(httpd_req_t* req) {
    Log::getLog().info("ALinkWeb", "Reboot requested via web — restarting in 200 ms");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, "rebooting", 9);
    vTaskDelay(pdMS_TO_TICKS(200));  // let the TCP response flush
    esp_restart();
    return ESP_OK;  // unreachable
}

} // namespace autolink
#endif // ARDUINO
