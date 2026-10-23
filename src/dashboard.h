// ======================================================
// DASHBOARD HTML  (styles and shared helpers come from ui_common.h: /app.css, /app.js)
// ======================================================

#ifndef DASHBOARD_H
#define DASHBOARD_H
#include <Arduino.h>

const char INDEX_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="color-scheme" content="light dark">
<title>Hive Monitor</title>
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
    <a class="tab active" href="/">Dashboard</a>
    <a class="tab" href="/settingsPage">Settings</a>
  </nav>
</header>

<main class="wrap">

  <section class="card">
    <div class="card-h"><h2><svg class="ic"><use href="#i-check"/></svg>Sensor status</h2>
      <span class="hint" id="lastUpd">Waiting for data&hellip;</span></div>
    <div class="chips" id="chips"></div>
  </section>

  <section class="grid" id="kpis"></section>

  <div class="two">
    <section class="card">
      <div class="card-h"><h2><svg class="ic"><use href="#i-cpu"/></svg>Device</h2></div>
      <dl class="kv" id="kvDevice"></dl>
    </section>
    <section class="card">
      <div class="card-h"><h2><svg class="ic"><use href="#i-wifi"/></svg>Connectivity</h2>
        <a class="btn sm" href="/settingsPage#network">Configure</a></div>
      <dl class="kv" id="kvNet"></dl>
    </section>
  </div>

  <section class="card">
    <div class="card-h">
      <h2><svg class="ic"><use href="#i-term"/></svg>Live log</h2>
      <div class="row">
        <div class="seg" id="seg">
          <button data-f="all" class="on">All</button>
          <button data-f="nodbg">No debug</button>
          <button data-f="problems">Problems</button>
        </div>
        <button class="btn sm" id="btnClear">Clear</button>
        <a class="btn sm" href="/events" download="events.log"><svg class="ic"><use href="#i-download"/></svg>Event file</a>
      </div>
    </div>
    <div class="term" id="term">Loading&hellip;</div>
    <p class="hint">Green = success, amber = warning, red = error. Warnings and errors are also saved to the SD card (event file).</p>
  </section>

  <div class="foot">Smart Beehive Monitoring System</div>
</main>

<script src="/app.js"></script>
<script>
(function(){
'use strict';
var KPIS=[
 {k:'temp',    l:'Brood temperature', u:'\u00b0C', d:1, i:'thermo'},
 {k:'humidity',l:'Brood humidity',    u:'%',       d:1, i:'drop'},
 {k:'temp2',   l:'Honey temperature', u:'\u00b0C', d:1, i:'thermo'},
 {k:'humidity2',l:'Honey humidity',   u:'%',       d:1, i:'drop'},
 {k:'co2',     l:'CO\u2082',          u:'ppm',     d:0, i:'wind'},
 {k:'battV',   l:'Battery voltage',   u:'V',       d:2, i:'battery'},
 {k:'battA',   l:'Battery current',   u:'A',       d:3, i:'zap'},
 {k:'solW',    l:'Solar power',       u:'W',       d:2, i:'sun'},
 {k:'audio',   l:'Audio peak',        u:'',        d:0, i:'mic'}
];
var hist={};
var box=$('kpis');
KPIS.forEach(function(c){
  hist[c.k]=[];
  var d=document.createElement('div');d.className='kpi';
  d.innerHTML='<div class="kpi-l">'+icon(c.i)+c.l+'</div>'+
    '<div class="kpi-v" id="v-'+c.k+'">\u2014</div>'+
    '<svg class="spark" viewBox="0 0 100 30" preserveAspectRatio="none"><polyline id="s-'+c.k+'" points=""/></svg>';
  box.appendChild(d);
});

function spark(k){
  var a=hist[k],el=$('s-'+k);
  if(a.length<2){el.setAttribute('points','');return}
  var mn=Math.min.apply(null,a),mx=Math.max.apply(null,a),r=(mx-mn)||1,p=[];
  for(var i=0;i<a.length;i++){
    var x=(i/(a.length-1))*100,y=(mx===mn)?15:28-((a[i]-mn)/r)*26;
    p.push(x.toFixed(1)+','+y.toFixed(1));
  }
  el.setAttribute('points',p.join(' '));
}

function chips(d){
  var list=[['CO\u2082 sensor',d.co2_ok],['Battery monitor',d.batt_ok],['Solar monitor',d.solar_ok,1],
            ['Microphone',d.mic_ok],['SD card',d.sd_ok],['Cloud uploads',d.cloud_ok]];
  var h='';
  list.forEach(function(s){
    var cls=s[1]?'':(s[2]?' na':' err');
    h+='<span class="chip'+cls+'">'+icon(s[1]?'check':'x')+s[0]+(!s[1]&&s[2]?' (not fitted)':'')+'</span>';
  });
  $('chips').innerHTML=h;
}

var busy=false;
async function pollData(){
  if(document.hidden||busy)return;
  busy=true;
  try{
    var d=await api('/data');
    KPIS.forEach(function(c){
      var v=d[c.k],el=$('v-'+c.k);
      if(v===null||v===undefined||isNaN(v)){el.innerHTML='\u2014';return}
      el.innerHTML=Number(v).toFixed(c.d)+(c.u?'<small>'+c.u+'</small>':'');
      var a=hist[c.k];a.push(Number(v));if(a.length>90)a.shift();spark(c.k);
    });
    chips(d);
    $('lastUpd').textContent='Updated '+new Date().toLocaleTimeString();
  }catch(e){$('lastUpd').textContent='No data: device not reachable'}
  busy=false;
}
pollData();setInterval(pollData,3000);
document.addEventListener('visibilitychange',function(){if(!document.hidden)pollData()});

function kv(id,rows){
  var h='';
  rows.forEach(function(r){h+='<dt>'+r[0]+'</dt><dd'+(r[2]?' class="'+r[2]+'"':'')+'>'+r[1]+'</dd>'});
  $(id).innerHTML=h;
}
function esc(s){return String(s).replace(/[&<>"]/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]})}

onInfo(function(i){
  kv('kvDevice',[
    ['Device ID',esc(i.device)],['Hive number',esc(i.hive)],['Firmware',esc(i.fw)],['Chip',esc(i.chip)],
    ['MAC address',esc(i.mac)],['Clock (local)',i.time?esc(i.time):'Not set',i.time?'':'warn-t'],
    ['Uptime',fmtDur(i.uptime)],['Restarts',i.reboots],
    ['Power mode',i.mode==='duty'?'Duty cycle (low power)':'Always on'],
    ['SD card used',i.sd_pct.toFixed(1)+' %'],
    ['Free memory',Math.round(i.heap/1024)+' kB (largest block '+Math.round(i.block/1024)+' kB)']
  ]);
  var link=i.sta==='connected'?'Connected, '+i.rssi+' dBm':'Idle (connects only to upload)';
  var last=i.last==='ok'?['Connected '+fmtAgo(i.lastAgo),'ok-t']:
           i.last==='failed'?['Failed '+fmtAgo(i.lastAgo)+': '+esc(i.reason||'unknown'),'err-t']:['Not tried yet',''];
  kv('kvNet',[
    ['Network (Wi\u2011Fi / MiFi)',esc(i.ssid)+(i.custom?'':' <small>(factory default)</small>')],
    ['Link',link],['Last connection',last[0],last[1]],
    ['Last successful upload',i.uploadAgo<0?'None yet':fmtAgo(i.uploadAgo),i.uploadAgo<0?'':'ok-t'],
    ['Next upload',i.next<=0?'now':'in '+fmtDur(i.next)],
    ['Pending data',i.backlog?'Yes, retrying':'None'],
    ['Dashboard clients',i.apClients]
  ]);
});

// ---- live log ----
var store=new Map(),lastSeq=0,bootId=null,filter='all',KEEP=400;
var term=$('term');
function render(force){
  var stick=force||term.scrollTop+term.clientHeight>=term.scrollHeight-25;
  var seqs=Array.from(store.keys()).sort(function(a,b){return a-b});
  var frag=document.createDocumentFragment(),n=0;
  seqs.forEach(function(s){
    var e=store.get(s);
    if(filter==='nodbg'&&e.l===4)return;
    if(filter==='problems'&&e.l!==2&&e.l!==3)return;
    var ln=document.createElement('div');ln.className='ll l'+e.l;
    var t=document.createElement('span');t.className='lt';t.textContent=e.t;
    var m=document.createElement('span');m.textContent=e.m;   // textContent: never parsed as HTML
    ln.appendChild(t);ln.appendChild(m);frag.appendChild(ln);n++;
  });
  term.innerHTML='';
  if(!n){var em=document.createElement('div');em.className='ll l4';
    em.textContent=filter==='problems'?'No warnings or errors':'No log lines yet';term.appendChild(em)}
  else term.appendChild(frag);
  if(stick)term.scrollTop=term.scrollHeight;
}
function add(list){
  list.forEach(function(e){store.set(e.s,e)});
  if(store.size>KEEP){
    var seqs=Array.from(store.keys()).sort(function(a,b){return a-b});
    seqs.slice(0,store.size-KEEP).forEach(function(s){store.delete(s)});
  }
}
var logBusy=false;
async function pollLogs(){
  if(document.hidden||logBusy)return;
  logBusy=true;
  try{
    var d=await api('/logs?since='+lastSeq);
    if(bootId!==null&&d.boot!==bootId){bootId=d.boot;store.clear();lastSeq=0;logBusy=false;return pollLogs()}
    var first=(bootId===null);
    bootId=d.boot;add(d.logs);lastSeq=d.last;
    if(first){var p=await api('/logs?problems=1');add(p.logs)}   // problems that already left the ring
    render(first);
  }catch(e){console.log(e)}
  logBusy=false;
}
pollLogs();setInterval(pollLogs,3000);
$('seg').addEventListener('click',function(ev){
  var b=ev.target.closest('button');if(!b)return;
  filter=b.getAttribute('data-f');
  Array.prototype.forEach.call($('seg').children,function(x){x.className=(x===b)?'on':''});
  render(true);
});
$('btnClear').addEventListener('click',function(){store.clear();render(true)});
})();
</script>
</body>
</html>
)rawliteral";

#endif
