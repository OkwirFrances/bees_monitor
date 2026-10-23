// ======================================================
// SETTINGS HTML  (styles and shared helpers come from ui_common.h: /app.css, /app.js)
// ======================================================

#ifndef SETTINGS_H
#define SETTINGS_H
#include <Arduino.h>

const char SETTINGS_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="color-scheme" content="light dark">
<title>Settings &middot; Hive Monitor</title>
<link rel="stylesheet" href="/app.css">
</head>
<body>

<header class="topbar">
  <div class="wrap bar">
    <div class="brand">
      <div class="logo"><svg class="ic"><use href="#i-hive"/></svg></div>
      <div style="min-width:0">
        <div class="brand-t">Smart Beehive Monitor</div>
        <div class="brand-s" id="hdrSub">Connecting&hellip;</div>
      </div>
    </div>
    <div class="pill" id="hdrPill"><span class="dot"></span><span id="hdrPillTxt">&hellip;</span></div>
  </div>
  <nav class="wrap tabs">
    <a class="tab" href="/">Dashboard</a>
    <a class="tab active" href="/settingsPage">Settings</a>
  </nav>
</header>

<main class="wrap">

  <!-- ============ NETWORK ============ -->
  <section class="card" id="network">
    <div class="card-h"><h2><svg class="ic"><use href="#i-wifi"/></svg>Internet connection (Wi&#8209;Fi / MiFi)</h2></div>

    <div class="note" id="netNote">Loading&hellip;</div>

    <div class="field">
      <label for="ssid">Network name (SSID)</label>
      <div class="with-btn">
        <input class="input" id="ssid" maxlength="32" autocomplete="off" autocapitalize="none" spellcheck="false" placeholder="e.g. my MiFi name">
        <button class="btn" id="btnScan" type="button"><svg class="ic"><use href="#i-search"/></svg>Scan</button>
      </div>
      <div class="hint">Scanning briefly pauses this page (the device radio changes channel). 2.4&nbsp;GHz networks only.</div>
    </div>
    <div class="nets" id="nets"></div>

    <div class="field">
      <label for="pass">Password</label>
      <div class="with-btn">
        <input class="input" id="pass" type="password" maxlength="63" autocomplete="new-password" placeholder="Password">
        <button class="btn" id="btnEye" type="button" title="Show or hide"><svg class="ic"><use href="#i-eye"/></svg></button>
      </div>
    </div>
    <label class="check"><input type="checkbox" id="open"> Open network (no password)</label>

    <div class="row">
      <button class="btn primary" id="btnSaveNet" type="button"><svg class="ic"><use href="#i-save"/></svg>Save and connect</button>
      <button class="btn danger" id="btnFactoryNet" type="button">Restore factory network</button>
    </div>
    <p class="hint" style="margin-top:12px">To save power the device joins this network only briefly to upload data, then disconnects. After saving, it tries to connect within a few seconds and the result appears above.</p>
  </section>

  <!-- ============ SCHEDULE ============ -->
  <section class="card" id="schedule">
    <div class="card-h"><h2><svg class="ic"><use href="#i-sliders"/></svg>Schedule &amp; power</h2></div>

    <div class="presets">
      <span>Presets</span>
      <button class="btn sm" type="button" data-p="bench">Bench test</button>
      <button class="btn sm" type="button" data-p="default">Factory default</button>
      <button class="btn sm" type="button" data-p="field">Field (battery saver)</button>
    </div>

    <div class="fields">
      <div class="field">
        <label for="sensor">Sensor reading every (seconds)</label>
        <input class="input" id="sensor" type="number" inputmode="numeric" min="10" max="3600">
        <div class="hint" id="h-sensor"></div>
      </div>
      <div class="field">
        <label for="record">Audio recording every (seconds)</label>
        <input class="input" id="record" type="number" inputmode="numeric" min="30" max="86400">
        <div class="hint" id="h-record"></div>
      </div>
      <div class="field">
        <label for="upload">Upload to the cloud every (seconds)</label>
        <input class="input" id="upload" type="number" inputmode="numeric" min="30" max="86400">
        <div class="hint" id="h-upload"></div>
      </div>
    </div>

    <div class="switch-row">
      <div>
        <b>Duty cycle (low power)</b>
        <span class="hint">The processor sleeps between jobs. In this mode the dashboard Wi&#8209;Fi is reachable only for about 30 seconds after each wake-up (and stays on while a phone is connected).</span>
      </div>
      <label class="switch"><input type="checkbox" id="fullDuty"><i></i></label>
    </div>

    <div class="row" style="margin-top:6px">
      <button class="btn primary" id="btnSaveSched" type="button"><svg class="ic"><use href="#i-save"/></svg>Save schedule</button>
    </div>
  </section>

  <!-- ============ MAINTENANCE ============ -->
  <section class="card">
    <div class="card-h"><h2><svg class="ic"><use href="#i-cpu"/></svg>Maintenance</h2></div>
    <div class="row">
      <button class="btn" id="btnNow" type="button"><svg class="ic"><use href="#i-refresh"/></svg>Upload now</button>
      <a class="btn" href="/events" download="events.log"><svg class="ic"><use href="#i-download"/></svg>Download event file</a>
      <button class="btn danger" id="btnReboot" type="button"><svg class="ic"><use href="#i-power"/></svg>Restart device</button>
    </div>
    <p class="hint" style="margin-top:12px">The event file lists warnings, errors and restarts saved on the SD card, which is useful to find out what happened while nobody was at the hive.</p>
  </section>

  <div class="foot">Smart Beehive Monitoring System</div>
</main>

<script src="/app.js"></script>
<script>
(function(){
'use strict';
var LIM={sensor:[10,3600],record:[30,86400],upload:[30,86400]};
var PRESETS={
  bench:  {sensor:60, record:60,   upload:120,  duty:false},
  default:{sensor:60, record:900,  upload:300,  duty:false},
  field:  {sensor:300,record:1800, upload:3600, duty:true}
};
function sleep(ms){return new Promise(function(r){setTimeout(r,ms)})}
function esc(s){return String(s).replace(/[&<>"]/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]})}

// ---------- schedule ----------
function hints(){
  Object.keys(LIM).forEach(function(k){
    var v=parseInt($(k).value,10),h=$('h-'+k);
    if(isNaN(v)){h.textContent='';return}
    var bad=v<LIM[k][0]||v>LIM[k][1];
    h.textContent='= '+fmtDur(v)+(bad?'  (allowed: '+fmtDur(LIM[k][0])+' to '+fmtDur(LIM[k][1])+')':'');
    h.className='hint'+(bad?' err-t':'');
  });
}
Object.keys(LIM).forEach(function(k){$(k).addEventListener('input',hints)});

async function loadSched(){
  try{
    var s=await api('/settings');
    $('sensor').value=s.sensor;$('record').value=s.record;$('upload').value=s.upload;
    $('fullDuty').checked=!!s.fullDuty;hints();
  }catch(e){toast('Could not load settings','err')}
}
loadSched();

document.querySelector('.presets').addEventListener('click',function(ev){
  var b=ev.target.closest('button[data-p]');if(!b)return;
  var p=PRESETS[b.getAttribute('data-p')];
  $('sensor').value=p.sensor;$('record').value=p.record;$('upload').value=p.upload;
  $('fullDuty').checked=p.duty;hints();
  toast('Preset loaded. Press "Save schedule" to apply.');
});

$('btnSaveSched').addEventListener('click',async function(){
  var o={fullDuty:$('fullDuty').checked?1:0};
  for(var k in LIM){
    var v=parseInt($(k).value,10);
    if(isNaN(v)||v<LIM[k][0]||v>LIM[k][1]){toast('Check the '+k+' interval','err');return}
    o[k]=v;
  }
  try{var r=await post('/setDuty',o);toast(r.msg||'Saved','ok');pollInfo()}
  catch(e){toast(e.message,'err')}
});

// ---------- network ----------
var filled=false;
onInfo(function(i){
  if(!filled){$('ssid').value=i.ssid;filled=true;
    $('pass').placeholder=i.passSet?'Leave empty to keep the saved password':'Password'}
  var n=$('netNote'),t,c='note';
  if(i.sta==='connected'){t='Connected to <b>'+esc(i.ssid)+'</b> ('+i.rssi+' dBm).';c+=' ok'}
  else if(i.last==='failed'){t='Last attempt to join <b>'+esc(i.ssid)+'</b> failed '+fmtAgo(i.lastAgo)+': '+esc(i.reason||'no reason')+'. Check the name and password.';c+=' err'}
  else if(i.last==='ok'){t='Joined <b>'+esc(i.ssid)+'</b> '+fmtAgo(i.lastAgo)+'. The device connects only while it uploads.';c+=' ok'}
  else{t='Configured network: <b>'+esc(i.ssid)+'</b>'+(i.custom?'':' (factory default)')+'. No connection attempt yet.'}
  n.className=c;n.innerHTML=t;
});

$('btnEye').addEventListener('click',function(){
  var p=$('pass');p.type=p.type==='password'?'text':'password';
});
$('open').addEventListener('change',function(){$('pass').disabled=this.checked;if(this.checked)$('pass').value=''});

$('btnScan').addEventListener('click',async function(){
  var b=this,box=$('nets');
  b.disabled=true;
  box.innerHTML='<div class="hint">Scanning&hellip;</div>';
  var res=null;
  try{
    await api('/wifiscan?start=1');
    for(var n=0;n<14&&!res;n++){
      await sleep(1500);
      try{var r=await api('/wifiscan');if(r.state==='done')res=r}catch(e){/* page pauses while the radio scans */}
    }
  }catch(e){}
  b.disabled=false;
  if(!res){box.innerHTML='<div class="hint err-t">Scan failed. Try again.</div>';return}
  if(!res.nets.length){box.innerHTML='<div class="hint">No networks found.</div>';return}
  box.innerHTML='';
  res.nets.forEach(function(w){
    var el=document.createElement('button');el.type='button';el.className='net';
    var a=document.createElement('span');a.textContent=w.s;
    var s=document.createElement('small');s.textContent=w.r+' dBm'+(w.o?'  \u00b7  open':'  \u00b7  secured');
    el.appendChild(a);el.appendChild(s);
    el.addEventListener('click',function(){
      $('ssid').value=w.s;$('open').checked=!!w.o;$('pass').disabled=!!w.o;
      if(w.o)$('pass').value='';else $('pass').focus();
    });
    box.appendChild(el);
  });
});

function watchConnection(){[3000,8000,14000,22000,32000].forEach(function(ms){setTimeout(pollInfo,ms)})}

$('btnSaveNet').addEventListener('click',async function(){
  var ssid=$('ssid').value.trim();
  if(!ssid){toast('Enter the network name','err');return}
  var o={ssid:ssid};
  if($('open').checked)o.open='1';else if($('pass').value)o.pass=$('pass').value;
  try{
    var r=await post('/wifi',o);
    toast(r.msg||'Saved','ok');
    $('pass').value='';
    watchConnection();
  }catch(e){toast(e.message,'err')}
});

$('btnFactoryNet').addEventListener('click',async function(){
  if(!confirm('Go back to the network that was built into the firmware?'))return;
  try{
    var r=await post('/wifi',{reset:'1'});
    toast(r.msg||'Done','ok');filled=false;pollInfo();watchConnection();
  }catch(e){toast(e.message,'err')}
});

// ---------- maintenance ----------
$('btnNow').addEventListener('click',async function(){
  try{var r=await post('/api/uploadNow');toast(r.msg||'Started','ok')}catch(e){toast(e.message,'err')}
});
$('btnReboot').addEventListener('click',async function(){
  if(!confirm('Restart the device now? Recording stops for about 20 seconds.'))return;
  try{await post('/api/reboot')}catch(e){}
  toast('Restarting\u2026 this page reloads in 25 s','ok');
  setTimeout(function(){location.reload()},25000);
});
})();
</script>
</body>
</html>
)rawliteral";

#endif
