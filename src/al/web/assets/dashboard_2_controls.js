// mode to /mode?m=SYNC|ASYNC; the firmware persists
// the choice to NVS and a reboot is required for it
// to take effect on the wire (mode changes the buffer
// floor and SYNC wait logic — both are init-time).
// After a successful /mode POST we kick off the same
// reboot path the Reboot button uses. /stats reconciles
// the radio from linkModeLabel on the next poll.
async function onLinkModeChange(val){
  if(!val)return;
  if(currentLinkMode===val){
    console.log('[autolink] link mode: no change ('+val+')');
    return;
  }
  var prev=currentLinkMode;
  if(!confirm('Switch link mode to '+val+'? The device will reboot for the new mode to take effect on the wire.')){
    var prevInp=document.querySelector('input[name=linkMode][value="'+(prev||'SYNC')+'"]');
    if(prevInp)prevInp.checked=true;
    return;
  }
  console.log('[autolink] button: link mode radio -> '+val+' (sending /mode?m='+val+')');
  try{
    var r=await tfetch('/mode?m='+encodeURIComponent(val),{method:'POST'},5000);
    if(r.ok){
      var body=await r.text();
      console.log('[autolink] /mode ack: '+body+' (was '+prev+', now '+val+') — rebooting to apply');
      reboot(true);
    }else{
      console.warn('[autolink] /mode returned '+r.status+' — reverting radio to '+prev);
      // Revert the radio selection to the previous
      // value so the UI reflects the actual link mode.
      var inp=document.querySelector('input[name=linkMode][value="'+(prev||'SYNC')+'"]');
      if(inp)inp.checked=true;
    }
  }catch(e){
    console.warn('[autolink] /mode fetch failed: '+(e&&e.message?e.message:e)+' — reverting radio to '+prev);
    var inp=document.querySelector('input[name=linkMode][value="'+(prev||'SYNC')+'"]');
    if(inp)inp.checked=true;
  }
}
function highlightLinkMode(){
  document.querySelectorAll('.lvl-group label').forEach(function(l){
    var inp=l.querySelector('input');
    if(inp&&inp.name==='linkMode')l.classList.toggle('on',inp.value===currentLinkMode);
  });
}

// Cookie helpers: persist the last delay-ms
// choice across reloads.
function al_cookie_set(name, val, days){
  var d=new Date();
  d.setTime(d.getTime()+(days||365)*24*60*60*1000);
  document.cookie=name+'='+encodeURIComponent(val)+
    ';expires='+d.toUTCString()+';path=/;SameSite=Lax';
}
function al_cookie_get(name){
  var m=document.cookie.match(new RegExp('(?:^|; )'+name+'=([^;]*)'));
  return m?decodeURIComponent(m[1]):null;
}

async function onDelayChange(val){
  var ms=parseInt(val,10);
  if(isNaN(ms)||ms<0)ms=0;
  al_cookie_set('al_delay_ms', String(ms));
  console.log('[autolink] delay widget: txDelayMs='+ms+' (sending /delay, cookie saved)');
  try{
    var r=await tfetch('/delay?ms='+ms,{method:'POST'},5000);
    if(!r.ok){
      console.warn('[autolink] /delay returned '+r.status+' — local UI shows last-known state until /stats reconciles');
    }
  }catch(e){
    console.warn('[autolink] /delay fetch failed: '+(e&&e.message?e.message:e)+' — local UI shows last-known state until /stats reconciles');
  }
}

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
bindLinkModeGroup('linkMode');
function highlightLvl(){
  document.querySelectorAll('.lvl-group label').forEach(function(l){
    var inp=l.querySelector('input');
    l.classList.toggle('on',inp&&inp.value===currentLvl);
  });
}

function bindModeGroup(name){
  document.querySelectorAll('input[name='+name+']').forEach(function(r){
    r.addEventListener('change',async function(){
      if(!this.checked)return;
      var m=this.value;
      console.log('[autolink] button: fill mode change -> m='+m);
      try{
        // fill-mode route renamed /mode -> /fillmode
        // (the live /mode link-mode toggle takes /mode now).
        var r2=await tfetch('/fillmode?m='+encodeURIComponent(m),{method:'POST'},5000);
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

function bindLinkModeGroup(name){
  document.querySelectorAll('input[name='+name+']').forEach(function(r){
    r.addEventListener('change',async function(){
      if(!this.checked)return;
      await onLinkModeChange(this.value);
    });
  });
}

