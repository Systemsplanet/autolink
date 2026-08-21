async function poll(){
  if(busy)return;
  busy=true;

  try{
    try{
    var r=await tfetch('/stats',null,5000);
    if(!r.ok)throw 0;
    var d=await r.json();

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
    setPill(d.state);
    if(d.version){
      document.getElementById('ver').textContent='v'+d.version;
      if(!window._autolinkLoggedVer){
        window._autolinkLoggedVer=true;
        console.log('[autolink] device firmware reports v'+d.version);
      }
    }

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

    if(d.role==='Ping'){
      deviceRole='Ping';
      document.body.setAttribute('data-role','ping');
    }else if(d.role==='Pong'){
      deviceRole='Pong';
      document.body.setAttribute('data-role','pong');

      document.querySelectorAll('.ping-only').forEach(function(el){
        el.style.display='none';
      });
    }

    if(d.lvl!==undefined&&d.lvl!==null&&String(d.lvl)!==currentLvl){
      currentLvl=String(d.lvl);
      document.querySelectorAll('input[name=lvl][value="'+currentLvl+'"], input[name=lvl2][value="'+currentLvl+'"]').forEach(function(r){r.checked=true;});
      highlightLvl();
    }

    if(d.mode!==undefined&&d.mode!==null){
      var m=d.mode===1?'rand':'seq';
      if(m!==currentMode){
        currentMode=m;
        var inp=document.querySelector('input[name=mode][value="'+m+'"]');
        if(inp)inp.checked=true;
        highlightMode();
      }
    }

    if(d.linkModeLabel!==undefined&&d.linkModeLabel!==null){
      if(d.linkModeLabel!==currentLinkMode){
        currentLinkMode=d.linkModeLabel;
        var inp=document.querySelector('input[name=linkMode][value="'+d.linkModeLabel+'"]');
        if(inp){inp.checked=true;highlightLinkMode();}
        console.log('[autolink] /stats linkModeLabel='+d.linkModeLabel+' — reconciled mode radio');
      }
    }

    if(d.msgPaused!==undefined&&d.msgPaused!==null){
      var devPaused=d.msgPaused===1;
      if(devPaused!==msgPaused){
        msgPaused=devPaused;
        applyMsgPauseLabel();
        console.log('[autolink] /stats msgPaused='+devPaused+' — reconciled button label');
      }
    }

    if(d.txDelayMs!==undefined&&d.txDelayMs!==null){
      var sel=document.getElementById('delayMs');
      if(sel){
        var devMs=String(d.txDelayMs);
        // Don't fight the user: if they just clicked the dropdown,
        // /stats will catch up next poll. Only update if it differs
        // and the dropdown isn't currently focused.
        if(devMs!==sel.value&&document.activeElement!==sel){
          sel.value=devMs;
          console.log('[autolink] /stats txDelayMs='+d.txDelayMs+' — synced dropdown (cookie unchanged)');
        }
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

document.addEventListener('visibilitychange',function(){if(!document.hidden){console.log('[autolink] tab visible — resuming poll');poll();}});
setInterval(poll,1000);
poll().then(function(){
  var _role=document.body.getAttribute('data-role')||'unknown';
  var _state=document.getElementById('pill')?document.getElementById('pill').textContent:'?';
  console.log('[autolink] startup OK — dashboard connected to firmware v{{VERSION}} role='+_role+' state='+_state);
  // Rehydrate delay widget: cookie value wins if set, else
  // firmware's current txDelayMs (from /stats). Cookie is the
  // user-sticky default so a reload restores the last choice.
  var _sel=document.getElementById('delayMs');
  if(_sel){
    var _ck=al_cookie_get('al_delay_ms');
    if(_ck!==null){
      _sel.value=_ck;
      onDelayChange(_ck);
      console.log('[autolink] delay widget: rehydrated from cookie='+_ck);
    } else {
      console.log('[autolink] delay widget: no cookie, using firmware default');
    }
  }
}).catch(function(e){
  console.error('[autolink] startup FAILED: '+(e&&e.message?e.message:e), '— dashboard will retry every 1 s');
});
