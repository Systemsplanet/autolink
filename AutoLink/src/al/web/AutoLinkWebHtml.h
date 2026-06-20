// AutoLinkWebHtml.h — embedded dashboard HTML/CSS/JS.
//
// Single source of truth for the dashboard page. Included by
// AutoLinkWeb.cpp (Arduino build) and by the host test
// (AutoLinkWebTest.cpp) to verify the presence of role-conditional
// UI elements, CSS class names, and JS handler bindings.
//
// The version string is spliced in via AUTOLINK_VERSION. Keep the
// AUTOLINK_VERSION token in a single line between R"HTML( and
// )HTML" so the C++ preprocessor splices the value before the
// raw string is processed.
#pragma once
#include "AutoLink.h"   // AUTOLINK_VERSION

namespace autolink {

// The full dashboard HTML, as a single raw string literal. The
// AUTOLINK_VERSION token in the footer + console.log is spliced
// at preprocessor time so the value always matches the firmware
// build.
static const char DASHBOARD_HTML[] = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<title>AutoLink Monitor</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#f0f2f5;color:#111827;min-height:100vh}
/* Hide Ping-only controls (fill mode radio, pause/start) when the
   device is a Pong. Pong is receive-only, so these are meaningless
   affordances. The body[data-role="pong"] attribute is set by the
   /stats poll handler once we know the role. Default: show
   everything (until the first /stats response lands, the user
   doesn't yet know if they're looking at Ping or Pong). */
body[data-role="pong"] .ping-only{display:none}
header{background:#ffffff;padding:14px 18px;display:flex;align-items:center;justify-content:space-between;border-bottom:2px solid #d1d5db;position:sticky;top:0;z-index:10;box-shadow:0 2px 8px rgba(0,0,0,.08);gap:10px;flex-wrap:wrap}
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
.g{color:#059669}.b{color:#2563eb}.r{color:#dc2626}.a{color:#7c3aed}
.hint{font-size:11px;color:#6b7280;margin-top:3px;font-variant-numeric:tabular-nums}
.row{display:flex;align-items:center;justify-content:space-between;gap:10px;margin-bottom:8px;flex-wrap:wrap}
.section-lbl{font-size:12px;font-weight:700;color:#374151;text-transform:uppercase;letter-spacing:.6px}
.btns{display:flex;gap:6px;flex-wrap:wrap;align-items:center}
.btn{font:600 12px -apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;padding:7px 12px;border:1px solid #d1d5db;background:#fff;border-radius:8px;cursor:pointer;color:#374151;transition:background .15s;-webkit-tap-highlight-color:transparent;user-select:none}
.btn:hover{background:#f3f4f6}
.btn.on{background:#dbeafe;color:#1d4ed8;border-color:#93c5fd}
.btn.pause.on{background:#fef3c7;color:#92400e;border-color:#fcd34d}
.btn.rst{color:#dc2626;border-color:#fca5a5}
.btn.rst:hover{background:#fef2f2}
.btn.rbt{color:#7c2d12;border-color:#fdba74;background:#fff7ed}
.btn.rbt:hover{background:#ffedd5}
.lvl-group{display:inline-flex;border:1px solid #d1d5db;border-radius:8px;overflow:hidden;background:#f9fafb;flex-wrap:wrap}
.lvl-group label{display:flex;align-items:center;gap:4px;padding:6px 10px;font-size:12px;font-weight:600;color:#374151;cursor:pointer;-webkit-tap-highlight-color:transparent;border-right:1px solid #d1d5db;user-select:none}
.lvl-group label:last-child{border-right:none}
.lvl-group input[type=radio]{margin:0;accent-color:#2563eb}
.lvl-group label.on{background:#dbeafe;color:#1d4ed8}
.role-pill{background:#e0e7ff;color:#3730a3;padding:3px 10px;border-radius:12px;font-size:13px;font-weight:600;letter-spacing:.3px;margin-left:6px}
.role-pill.empty{display:none}
.log-wrap{position:relative}
.log{background:#1f2937;color:#e5e7eb;font:11px ui-monospace,Menlo,Consolas,monospace;border-radius:10px;padding:10px 12px;height:200px;overflow-y:auto;line-height:1.5;border:1px solid #d1d5db}
.log>div{white-space:pre-wrap;word-break:break-all}
.log .E{color:#fca5a5}.log .W{color:#fcd34d}.log .I{color:#93c5fd}.log .D{color:#9ca3af}.log .V{color:#c4b5fd}
.log-fill-bar{height:5px;border-radius:0 0 8px 8px;background:linear-gradient(to right,#22c55e var(--pct,0%),#e5e7eb var(--pct,0%));border:1px solid #d1d5db;border-top:none;transition:background 0.3s}
.log-overlay{display:none;position:fixed;inset:0;z-index:9999;background:#fff;flex-direction:column}
.log-overlay.open{display:flex}
.log-overlay .log{flex:1;height:auto;border-radius:0;border:none;border-bottom:1px solid #d1d5db}
.log-overlay .log-fill-bar{border-radius:0;border:none;border-top:1px solid #e5e7eb}
.log-overlay-hdr{display:flex;align-items:center;justify-content:space-between;padding:8px 14px;border-bottom:1px solid #d1d5db;background:#f9fafb}
.log-overlay-hdr>span{font-weight:600;font-size:14px}
.log-overlay-close{padding:6px 12px;border:1px solid #d1d5db;background:#fff;border-radius:8px;font:600 12px -apple-system,sans-serif;cursor:pointer}
.footer{text-align:center;font-size:11px;color:#6b7280;padding:12px 8px 24px}
.footer span{font-weight:600;color:#374151}
@media(max-width:380px){.val{font-size:22px}.log{height:160px}}
</style>
</head>
<body>
<header>
  <div style="flex:1;min-width:160px">
    <h1>AutoLink Monitor <span class="role-pill empty" id="rolePill"></span></h1>
    <div class="sub" id="uptime">connecting&#x2026;</div>
  </div>
  <button class="btn rbt" id="rebootBtnTop" onclick="reboot()" title="Reboot device">&#9211; Reboot</button>
  <div class="lvl-group ping-only" id="modeGroup" role="radiogroup" aria-label="Fill mode" style="margin-right:8px">
    <label><input type="radio" name="mode" value="seq" id="modeSeq"><span>Sequential</span></label>
    <label><input type="radio" name="mode" value="rand" id="modeRand"><span>Random</span></label>
  </div>
  <button class="btn pause ping-only" id="topPbtn" onclick="toggleMsgPause()">&#9654; Start</button>
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
      <div class="lbl">Errors</div>
      <div class="val r" id="discon">&#x2014;</div>
      <div class="hint" id="lostmsgs">0 lost msgs</div>
      <div class="hint" id="errcnt">0 frame errors</div>
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
    </div>
  </div>
  <div class="row">
    <span class="section-lbl">Live Log</span>
    <div class="btns">
      <div class="lvl-group" id="lvlGroup" role="radiogroup" aria-label="Log level">
        <!-- v4.1.16: None removed. Silences all logger and is unrecoverable
             without reflash or manual NVS clear; the server now also
             rejects lv=0 with a 400. -->
        <label><input type="radio" name="lvl" value="1"><span>Error</span></label>
        <label><input type="radio" name="lvl" value="2"><span>Warn</span></label>
        <label><input type="radio" name="lvl" value="3"><span>Info</span></label>
        <label><input type="radio" name="lvl" value="4"><span>Debug</span></label>
        <!-- Verbose above Debug: per-frame control traffic (ALink
             sendFrame TX) that is too noisy for normal operation but
             useful for wire-trace forensics. -->
        <label><input type="radio" name="lvl" value="5"><span>Verbose</span></label>
      </div>
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
      <span>Live Log (maximized)</span>
      <div class="btns">
        <div class="lvl-group" id="lvlGroup2" role="radiogroup" aria-label="Log level">
          <label><input type="radio" name="lvl2" value="1"><span>Error</span></label>
          <label><input type="radio" name="lvl2" value="2"><span>Warn</span></label>
          <label><input type="radio" name="lvl2" value="3"><span>Info</span></label>
          <label><input type="radio" name="lvl2" value="4"><span>Debug</span></label>
          <!-- Verbose in the overlay's log-level group too. -->
          <label><input type="radio" name="lvl2" value="5"><span>Verbose</span></label>
        </div>
        <button class="btn" onclick="clearLog()">Clear</button>
        <button class="btn" id="cbtn2" onclick="copyLog()">Copy</button>
        <button class="btn" id="pbtn2" onclick="togglePause()">&#9646;&#9646; Pause</button>
        <button class="btn log-overlay-close" onclick="closeLogFull()">Close</button>
      </div>
    </div>
    <div class="log" id="logFull"></div>
    <div class="log-fill-bar" id="logFillFull"></div>
  </div>
  <div class="footer"><span id="ver">v)HTML" AUTOLINK_VERSION R"HTML(</span> AutoLink Web Monitor &#x2014; <span id="host"></span></div>
</main>
<script>
// On load: log the HTML build version (compiled in at link time)
// and start the dashboard. The firmware's reported version arrives
// on the first /stats response — a mismatch there means the firmware
// was built from a different tree than the HTML.
console.log('[autolink] dashboard script loaded, HTML build v)HTML" AUTOLINK_VERSION R"HTML(');
console.log('[autolink] starting up…');

// v5.1.29: msgPaused starts true so the button reads "Start" on
// load. The device is actually paused at boot (paused_=true in
// UtilPing) and waits for /pausemsg?p=0 to begin sending. Showing
// "Start" up front matches the device state and avoids the
// confusing "Pause button while nothing's happening" UX. The /stats
// poll reconciles in case anything diverges.
var logPaused=false,msgPaused=true,logFullOpen=false,lastSeq=0,fails=0,busy=false,currentLvl=null;
var currentMode=null;
// v5.1.33: device role as seen by the dashboard. Null = unknown
// (before first /stats). Drives the ping-only controls' visibility
// at the JS level so we never show a Pause/Start button that will
// only flicker when clicked. CSS hides the same elements via
// body[data-role="pong"] but that requires the first /stats poll
// to land; before then, the button is briefly visible AND clickable.
// Now JS hides them immediately on Pong reveal, AND ignores clicks
// while role is null (the click handler is a no-op).
var deviceRole=null;

function fallbackCopy(text,b){
  var ta=document.createElement('textarea');
  ta.value=text;ta.style.position='fixed';ta.style.opacity='0';
  document.body.appendChild(ta);ta.focus();ta.select();
  try{
    var ok=document.execCommand('copy');
    b.textContent=ok?'\u2713 Copied':'\u2717 Failed';
    console.log('[autolink] copy log: fallback path '+(ok?'succeeded':'failed'));
  }catch(e){
    b.textContent='\u2717 Failed';
    console.warn('[autolink] copy log: fallback threw '+(e&&e.message?e.message:e));
  }
  document.body.removeChild(ta);
}

async function resetAll(){
  console.log('[autolink] button: reset (counters)');
  var b=document.getElementById('rbtn');
  b.textContent='\u2026';
  try{
    var r=await tfetch('/reset',{method:'POST'},5000);
    b.textContent=r.ok?'\u2713 Done':'\u2717 Err';
    console.log('[autolink] reset result: '+(r.ok?'OK':'fail http '+(r.status||'?')));
  }catch(e){
    b.textContent='\u2717 Err';
    console.warn('[autolink] reset failed: '+(e&&e.message?e.message:e));
  }
  setTimeout(function(){b.textContent='\u21bb Reset';},1200);
}

async function reboot(){
  if(!confirm('Reboot the device now? The link will drop and reconnect in a few seconds.'))return;
  console.log('[autolink] button: reboot (device reset requested)');
  clearLog();lastSeq=0;
  var b=document.getElementById('rebootBtnTop');
  b.textContent='Rebooting\u2026';
  try{
    var r=await tfetch('/reboot',{method:'POST'},5000);
    console.log('[autolink] reboot ack: '+(r&&r.ok?'OK':'fail http '+(r&&r.status||'?')));
  }catch(e){
    // An aborted fetch is expected: the device resets mid-response.
    console.log('[autolink] reboot: device disconnected (expected)');
  }
  show('alert');
  var tries=0;
  var iv=setInterval(async function(){
    tries++;
    try{
      var r=await tfetch('/stats',null,5000);
      if(r.ok){
        clearInterval(iv);
        console.log('[autolink] reboot: device back online after '+(tries+1)+' s, reloading');
        location.reload();
      }
    }catch(e){}
    if(tries>30){
      clearInterval(iv);
      console.warn('[autolink] reboot: device did not come back online within 30 s');
    }
  },1000);
}

function updateFillBar(){
  var t=document.getElementById('log').textContent;
  var pct=Math.min(100,Math.round(t.length/5000))+'%';
  ['logFill','logFillFull'].forEach(function(id){var b=document.getElementById(id);if(b)b.style.setProperty('--pct',pct);});
}
function openLogFull(){
  logFullOpen=true;
  var o=document.getElementById('logOverlay');o.classList.add('open');
  var dst=document.getElementById('logFull');
  dst.innerHTML=document.getElementById('log').innerHTML;
  dst.scrollTop=dst.scrollHeight;updateFillBar();
  console.log('[autolink] button: log overlay opened ('+document.getElementById('log').children.length+' entries)');
}
function closeLogFull(){
  logFullOpen=false;
  document.getElementById('logOverlay').classList.remove('open');
  console.log('[autolink] button: log overlay closed');
}
function appendLog(sev,seq,text){
  if(seq+1>lastSeq)lastSeq=seq+1;
  if(logPaused)return;
  function addTo(p){
    var atEnd=p.scrollHeight-p.scrollTop<=p.clientHeight+12;
    var d=document.createElement('div');d.className=sev;d.textContent=text;p.appendChild(d);
    if(p.textContent.length>500000){while(p.children.length>1&&p.textContent.length>400000)p.removeChild(p.firstChild);}
    if(atEnd)p.scrollTop=p.scrollHeight;
  }
  addTo(document.getElementById('log'));
  if(logFullOpen)addTo(document.getElementById('logFull'));
  updateFillBar();
  // Verbose mode: log every line to console. Off by default to keep
  // the console readable; toggle by setting localStorage.verbose=1
  // in DevTools.
  try{if(localStorage.verbose==='1')console.log('[autolink] log['+seq+'] '+sev+' '+text);}catch(_){}
}

function tfetch(url,opts,ms){
  var c=new AbortController();
  var id=setTimeout(function(){c.abort();},ms||5000);
  var o=Object.assign({signal:c.signal},opts||{});
  var method=(opts&&opts.method)||'GET';
  console.log('[autolink] '+method+' '+url+' (timeout '+(ms||5000)+'ms)');
  return fetch(url,o).then(function(r){
      clearTimeout(id);
      console.log('[autolink] '+method+' '+url+' -> '+r.status);
      return r;
    },function(e){
      clearTimeout(id);
      console.warn('[autolink] '+method+' '+url+' FAILED: '+(e&&e.message?e.message:e));
      throw e;
    });
}

function bps(n){if(n>=1048576)return(n/1048576).toFixed(1)+' MB/s';if(n>=1024)return(n/1024).toFixed(1)+' KB/s';return n+' B/s';}
function bytes(n){if(n>=1073741824)return(n/1073741824).toFixed(2)+' GB';if(n>=1048576)return(n/1048576).toFixed(2)+' MB';if(n>=1024)return(n/1024).toFixed(1)+' KB';return n+' B';}
function hms(s){return[Math.floor(s/3600),Math.floor(s%3600/60),s%60].map(function(x){return('0'+x).slice(-2);}).join(':');}

function set(id,v){var e=document.getElementById(id);if(e)e.textContent=v;}
function setPill(st){
  var p=document.getElementById('pill');
  if(!p)return;
  p.textContent=st;p.className='pill '+(st==='OK'?'ok':st==='SWP'?'swp':st==='LCK'?'lck':'');
}
function show(id){var e=document.getElementById(id);if(e)e.style.display='block';}
function hide(id){var e=document.getElementById(id);if(e)e.style.display='none';}

function clearLog(){
  var n=document.getElementById('log').children.length;
  ['log','logFull'].forEach(function(id){var e=document.getElementById(id);if(e)e.innerHTML='';});
  updateFillBar();
  console.log('[autolink] button: clear log (cleared '+n+' entries)');
}
function copyLog(){
  var t=document.getElementById('log').textContent;
  var bytes=t.length;
  if(navigator.clipboard){
    navigator.clipboard.writeText(t).then(function(){
      var b=document.getElementById('cbtn');
      b.textContent='\u2713 Copied';
      setTimeout(function(){b.textContent='Copy';},1200);
      console.log('[autolink] button: copy log ('+bytes+' bytes copied to clipboard)');
    }).catch(function(e){
      fallbackCopy(t,document.getElementById('cbtn'));
      console.warn('[autolink] copy log: clipboard API failed ('+(e&&e.message?e.message:e)+'), used fallback');
    });
  }else{
    fallbackCopy(t,document.getElementById('cbtn'));
    console.log('[autolink] button: copy log (fallback path, '+bytes+' bytes)');
  }
}
function togglePause(){
  logPaused=!logPaused;
  var lbl=logPaused?'\u25b6 Resume':'\u25ae\u25ae Pause';
  ['pbtn','pbtn2'].forEach(function(id){var b=document.getElementById(id);if(b)b.textContent=lbl;});
  console.log('[autolink] button: log scroll '+(logPaused?'paused':'resumed'));
}
// v5.1.29: toggleMsgPause hits the device (POST /pausemsg) AND
// updates the local JS state. v5.1.33: the local flip is no longer
// optimistic-then-revert — that caused a visible 100 ms flicker
// ("Pause" briefly showing on Pong before reverting to "Start"
// when /pausemsg returned 404). Now the button is bound only when
// deviceRole === 'Ping'; on Pong the click is a no-op, and the
// element is hidden by JS as soon as the role is known. The CSS
// rule (body[data-role="pong"] .ping-only{display:none}) is the
// belt-and-suspenders second line of defense.
async function toggleMsgPause(){
  // v5.1.33: ignore clicks until we know the device is Ping. If
  // the click somehow fires (CSS not yet applied, race on page
  // load), this guard prevents the visible Pause→Start flicker.
  if(deviceRole!=='Ping'){
    console.log('[autolink] toggleMsgPause ignored — device role is '+deviceRole+' (not Ping)');
    return;
  }
  msgPaused=!msgPaused;
  applyMsgPauseLabel();
  console.log('[autolink] button: message pause -> '+msgPaused+' (sending /pausemsg)');
  try{
    var r=await tfetch('/pausemsg?p='+(msgPaused?'1':'0'),{method:'POST'},5000);
    if(!r.ok){
      // No optimistic-flip-and-revert anymore: if /pausemsg fails
      // for any reason, log it but don't touch msgPaused. The next
      // /stats poll will reconcile.
      console.warn('[autolink] /pausemsg returned '+r.status+' — local UI shows last-known state until /stats reconciles');
    }
  } catch(e){
    console.warn('[autolink] /pausemsg fetch failed: '+(e&&e.message?e.message:e)+' — local UI shows last-known state until /stats reconciles');
  }
}
function applyMsgPauseLabel(){
  var b=document.getElementById('topPbtn');
  if(b){
    if(msgPaused){b.innerHTML='\u25b6 Start';b.className='btn pause on';}
    else{b.innerHTML='\u25ae\u25ae Pause';b.className='btn pause';}
  }
}

// Log level radio (inline + overlay). The inline row and the
// log-overlay's header both bind to the same name space; clicking
// either sends a POST to /level and updates both groups' highlight.
// The default level is whatever the device was already at; the
// first /stats response reconciles the UI to it.
function bindLvlGroup(name){
  document.querySelectorAll('input[name='+name+']').forEach(function(r){
    r.addEventListener('change',async function(){
      if(!this.checked)return;
      var lv=this.value;
      console.log('[autolink] button: log level change -> lv='+lv);
      try{
        var r2=await tfetch('/level?lv='+encodeURIComponent(lv),{method:'POST'},5000);
        if(r2.ok){
          currentLvl=lv;
          highlightLvl();
          console.log('[autolink] log level set: lv='+lv+' (firmware applied)');
        }else{
          console.warn('[autolink] log level set: firmware rejected lv='+lv+' (http '+(r2.status||'?')+')');
        }
      }catch(e){
        console.warn('[autolink] log level set failed: '+(e&&e.message?e.message:e));
      }
    });
  });
}
bindLvlGroup('lvl');
bindLvlGroup('lvl2');
function highlightLvl(){
  // Highlight the matching label in BOTH radio groups so the inline
  // row and the overlay's header stay in sync.
  document.querySelectorAll('.lvl-group label').forEach(function(l){
    var inp=l.querySelector('input');
    l.classList.toggle('on',inp&&inp.value===currentLvl);
  });
}

// Fill mode (Sequential vs Random). Sends POST /mode?m=seq|rand on
// change. The Ping side has a fill-mode hook registered; the Pong
// side returns 404 and we ignore it. The inline row and the
// overlay's header both bind to the same name space; clicking
// either updates both groups' highlight.
function bindModeGroup(name){
  document.querySelectorAll('input[name='+name+']').forEach(function(r){
    r.addEventListener('change',async function(){
      if(!this.checked)return;
      var m=this.value;
      console.log('[autolink] button: fill mode change -> m='+m);
      try{
        var r2=await tfetch('/mode?m='+encodeURIComponent(m),{method:'POST'},5000);
        if(r2.ok){
          currentMode=m;
          highlightMode();
          console.log('[autolink] fill mode set: m='+m+' (firmware applied)');
        }else{
          console.warn('[autolink] fill mode set: firmware rejected m='+m+' (http '+(r2.status||'?')+', likely Pong side)');
        }
      }catch(e){
        console.warn('[autolink] fill mode set failed: '+(e&&e.message?e.message:e));
      }
    });
  });
}
bindModeGroup('mode');
function highlightMode(){
  document.querySelectorAll('.lvl-group label').forEach(function(l){
    var inp=l.querySelector('input');
    if(inp&&inp.name==='mode')l.classList.toggle('on',inp.value===currentMode);
  });
}

async function poll(){
  if(busy)return;
  busy=true;
  // Wrap the whole body in try/finally so a thrown exception
  // (e.g., DOM access in a removed element, JSON parse error,
  // abort from a slow tab backgrounded) cannot leave busy=true
  // and freeze the dashboard. The original code had busy=false
  // at the very end, but a throw between the two try blocks
  // (or a syntax-level slip) would skip the reset.
  try{
    try{
    var r=await tfetch('/stats',null,5000);
    if(!r.ok)throw 0;
    var d=await r.json();
    if(!msgPaused){
      set('txbps',bps(d.txBps));
      set('txtot','total '+bytes(d.txTotal));
      set('rxbps',bps(d.rxBps));
      set('rxtot','total '+bytes(d.rxTotal));
      set('discon', d.errTotal);
      var lm=d.lostMsgs||0;
      set('lostmsgs', lm + (lm===1?' lost msg':' lost msgs'));
      var ec=d.errCount||0;
      set('errcnt', ec + (ec===1?' frame error':' frame errors'));
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
    }
    setPill(d.state);
    if(d.version){
      document.getElementById('ver').textContent='v'+d.version;
      if(!window._autolinkLoggedVer){
        window._autolinkLoggedVer=true;
        console.log('[autolink] device firmware reports v'+d.version);
      }
    }
    // Update the role pill in the header. Empty role (legacy code
    // that didn't call setRole) hides the pill.
    var rp=document.getElementById('rolePill');
    if(rp){
      if(d.role&&d.role.length){
        rp.textContent=d.role;
        rp.classList.remove('empty');
      }else{
        rp.textContent='';
        rp.classList.add('empty');
      }
    }
    // Show Ping-only controls (Sequential/Random radio, Pause/Start
    // for messages) only on the Ping side. Pong is receive-only and
    // has no fill mode, so the radio is meaningless; the Pause/Start
    // message button is likewise a Ping-side affordance. Log-scroll
    // Pause/Resume is shared (visible on both).
    // Element IDs are tagged with the class .ping-only and toggled
    // by the `data-role` attribute on the body.
    if(d.role==='Ping'){
      deviceRole='Ping';
      document.body.setAttribute('data-role','ping');
    }else if(d.role==='Pong'){
      deviceRole='Pong';
      document.body.setAttribute('data-role','pong');
      // v5.1.33: hide ping-only elements immediately via JS so the
      // Pause/Start button disappears the moment we know it's Pong,
      // not on the next CSS re-layout. Belt-and-suspenders with the
      // CSS rule (body[data-role="pong"] .ping-only{display:none}).
      document.querySelectorAll('.ping-only').forEach(function(el){
        el.style.display='none';
      });
    }
    // Reconcile the level radio to whatever the device is currently at.
    if(d.lvl!==undefined&&d.lvl!==null&&String(d.lvl)!==currentLvl){
      currentLvl=String(d.lvl);
      document.querySelectorAll('input[name=lvl][value="'+currentLvl+'"], input[name=lvl2][value="'+currentLvl+'"]').forEach(function(r){r.checked=true;});
      highlightLvl();
    }
    // Sync the fill-mode radio from /stats so a dashboard opened
    // after the user changed the mode reflects it.
    if(d.mode!==undefined&&d.mode!==null){
      var m=d.mode===1?'rand':'seq';
      if(m!==currentMode){
        currentMode=m;
        var inp=document.querySelector('input[name=mode][value="'+m+'"]');
        if(inp)inp.checked=true;
        highlightMode();
      }
    }
    // v5.1.29: reconcile the pause button label with /stats.msgPaused
    // on every poll. The device is the source of truth — handles
    // refresh-after-pause, race on boot, and any /pausemsg POST that
    // came in from another tab/phone. fieldMissing means no hook
    // registered (Pong or old firmware); leave msgPaused as-is.
    if(d.msgPaused!==undefined&&d.msgPaused!==null){
      var devPaused=d.msgPaused===1;
      if(devPaused!==msgPaused){
        msgPaused=devPaused;
        applyMsgPauseLabel();
        console.log('[autolink] /stats msgPaused='+devPaused+' — reconciled button label');
      }
    }
    fails=0;hide('alert');
    console.log('[autolink] /stats OK state='+d.state+' rxBps='+d.rxBps);
    }catch(e){
      var ec=(e&&e.code)||'';
      var em=(e&&e.message)||String(e);
      console.warn('[autolink] /stats poll failed #'+fails+' code='+ec+' msg='+em);
      if(++fails>=3){
        show('alert');
        if(/^(NETWORKERR|ERR_CONNECTION|ECONNREFUSED|ECONNRESET|ETIMEDOUT)/.test(ec)){
          if(fails>=8){try{location.reload();}catch(_){}}
        }
      }
    }
    try{
    var r2=await tfetch('/logs?since='+lastSeq,null,5000);
    if(!r2.ok)throw new Error('http '+r2.status);
    var d2=await r2.json();
    if(lastSeq===0 && d2.head!==undefined){
      lastSeq=d2.head;
    }else{
      var got=d2.lines?d2.lines.length:0;
      if(got>0)console.log('[autolink] /logs '+got+' new line(s) (since='+lastSeq+')');
      d2.lines.forEach(function(l){appendLog(l.sev,l.seq,l.text);});
    }
    }catch(e){
      console.warn('[autolink] /logs poll failed: '+((e&&e.message)||e));
    }
  }finally{
    busy=false;
  }
}

// Start the poll loop. The first poll kicks off the periodic
// 1 Hz refresh. We also log a startup-OK message after the FIRST
// /stats response so the operator can confirm the dashboard is
// talking to the firmware. The startup banner at the top of the
// script and this startup-OK message are the two anchors an
// operator can grep for in DevTools to confirm the page is alive.
document.addEventListener('visibilitychange',function(){if(!document.hidden){console.log('[autolink] tab visible — resuming poll');poll();}});
setInterval(poll,1000);
poll().then(function(){
  var _role=document.body.getAttribute('data-role')||'unknown';
  var _state=document.getElementById('pill')?document.getElementById('pill').textContent:'?';
  console.log('[autolink] startup OK — dashboard connected to firmware v)HTML" AUTOLINK_VERSION R"HTML( role='+_role+' state='+_state);
}).catch(function(e){
  console.error('[autolink] startup FAILED: '+(e&&e.message?e.message:e), '— dashboard will retry every 1 s');
});
</script>
</body>
</html>)HTML";

} // namespace autolink
