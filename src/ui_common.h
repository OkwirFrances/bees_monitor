// ======================================================
// SHARED WEB UI ASSETS  (served as /app.css and /app.js, cached by the browser)
// ======================================================
#ifndef UI_COMMON_H
#define UI_COMMON_H
#include <Arduino.h>

const char APP_CSS[] PROGMEM = R"rawliteral(
*{box-sizing:border-box;margin:0;padding:0}
:root{
 --bg:#f4f5f7;--card:#fff;--card2:#f8f9fb;--line:#e4e7ec;--text:#101828;--mute:#667085;
 --brand:#d97706;--brandd:#b45309;--brandbg:#fff4de;
 --ok:#0e9f6e;--okbg:#e4f7ee;--warn:#c27803;--warnbg:#fff4de;--err:#d92d20;--errbg:#fdecea;
 --sh:0 1px 2px rgba(16,24,40,.05),0 1px 3px rgba(16,24,40,.08);
}
@media(prefers-color-scheme:dark){:root{
 --bg:#0b0d12;--card:#151922;--card2:#1b202b;--line:#262c39;--text:#e7eaf0;--mute:#98a2b3;
 --brand:#f5a524;--brandd:#fbbf4a;--brandbg:#2b2210;
 --ok:#34d399;--okbg:#0f2a1d;--warn:#fbbf24;--warnbg:#2c2210;--err:#f87171;--errbg:#2e1513;
 --sh:0 1px 2px rgba(0,0,0,.4);
}}
html{-webkit-text-size-adjust:100%}
body{font:14px/1.45 system-ui,-apple-system,"Segoe UI",Roboto,Helvetica,Arial,sans-serif;background:var(--bg);color:var(--text)}
.wrap{max-width:1040px;margin:0 auto;padding:0 16px}
.ic{width:16px;height:16px;fill:none;stroke:currentColor;stroke-width:2;stroke-linecap:round;stroke-linejoin:round;flex:none}

.topbar{background:var(--card);border-bottom:1px solid var(--line);position:sticky;top:0;z-index:20}
.bar{display:flex;align-items:center;justify-content:space-between;gap:12px;height:60px}
.brand{display:flex;align-items:center;gap:10px;min-width:0}
.logo{width:36px;height:36px;border-radius:10px;background:var(--brand);color:#fff;display:grid;place-items:center;flex:none}
.logo .ic{width:20px;height:20px}
.brand-t{font-weight:700;font-size:16px;letter-spacing:-.01em}
.brand-s{color:var(--mute);font-size:12px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.pill{display:inline-flex;align-items:center;gap:7px;padding:5px 11px;border-radius:999px;font-size:12px;font-weight:600;background:var(--card2);border:1px solid var(--line);color:var(--mute);white-space:nowrap}
.dot{width:8px;height:8px;border-radius:50%;background:var(--mute)}
.pill.ok{background:var(--okbg);color:var(--ok);border-color:transparent}.pill.ok .dot{background:var(--ok)}
.pill.warn{background:var(--warnbg);color:var(--warn);border-color:transparent}.pill.warn .dot{background:var(--warn)}
.pill.err{background:var(--errbg);color:var(--err);border-color:transparent}.pill.err .dot{background:var(--err)}
.tabs{display:flex;gap:4px}
.tab{padding:10px 14px;color:var(--mute);text-decoration:none;font-weight:600;border-bottom:2px solid transparent;margin-bottom:-1px}
.tab:hover{color:var(--text)}
.tab.active{color:var(--brandd);border-color:var(--brand)}

main{padding:18px 0 40px;display:grid;gap:16px}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;box-shadow:var(--sh);padding:16px}
.card-h{display:flex;align-items:center;justify-content:space-between;gap:10px;margin-bottom:12px;flex-wrap:wrap}
.card-h h2{font-size:12px;font-weight:700;text-transform:uppercase;letter-spacing:.07em;color:var(--mute);display:flex;align-items:center;gap:8px}
.hint{color:var(--mute);font-size:12px;margin-top:4px}
.two{display:grid;grid-template-columns:1fr 1fr;gap:16px}
@media(max-width:760px){.two{grid-template-columns:1fr}}

.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(190px,1fr));gap:12px}
.kpi{background:var(--card);border:1px solid var(--line);border-radius:14px;box-shadow:var(--sh);padding:14px 14px 10px}
.kpi-l{display:flex;align-items:center;gap:7px;color:var(--mute);font-size:12px;font-weight:600}
.kpi-l .ic{color:var(--brand)}
.kpi-v{font-size:28px;font-weight:700;letter-spacing:-.02em;margin-top:6px;font-variant-numeric:tabular-nums}
.kpi-v small{font-size:13px;font-weight:600;color:var(--mute);margin-left:3px}
.spark{display:block;width:100%;height:30px;margin-top:6px}
.spark polyline{fill:none;stroke:var(--brand);stroke-width:1.6;stroke-linejoin:round;stroke-linecap:round;vector-effect:non-scaling-stroke}

.chips{display:flex;flex-wrap:wrap;gap:8px}
.chip{display:inline-flex;align-items:center;gap:6px;padding:5px 10px;border-radius:999px;font-size:12px;font-weight:600;background:var(--okbg);color:var(--ok)}
.chip.err{background:var(--errbg);color:var(--err)}
.chip.na{background:var(--card2);color:var(--mute)}
.chip .ic{width:13px;height:13px}

.kv{display:grid;grid-template-columns:auto 1fr;gap:9px 16px;font-size:13px}
.kv dt{color:var(--mute)}
.kv dd{text-align:right;font-weight:600;word-break:break-word;font-variant-numeric:tabular-nums}
.ok-t{color:var(--ok)}.warn-t{color:var(--warn)}.err-t{color:var(--err)}

.btn{display:inline-flex;align-items:center;justify-content:center;gap:7px;padding:9px 14px;border-radius:9px;border:1px solid var(--line);background:var(--card);color:var(--text);font:inherit;font-weight:600;cursor:pointer;text-decoration:none;white-space:nowrap}
.btn:hover{background:var(--card2)}
.btn:disabled{opacity:.55;cursor:not-allowed}
.btn.primary{background:var(--brand);border-color:var(--brand);color:#fff}
.btn.primary:hover{background:var(--brandd);border-color:var(--brandd)}
.btn.danger{color:var(--err)}
.btn.sm{padding:5px 10px;font-size:12px;border-radius:8px}
.row{display:flex;flex-wrap:wrap;gap:10px;align-items:center}

.field{display:grid;gap:5px;margin-bottom:14px}
.field label{font-weight:600;font-size:13px}
.input{width:100%;padding:10px 12px;border:1px solid var(--line);border-radius:9px;background:var(--card);color:var(--text);font:inherit;outline:none}
.input:focus{border-color:var(--brand);box-shadow:0 0 0 3px var(--brandbg)}
.with-btn{display:flex;gap:8px}
.fields{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:0 16px}
.check{display:flex;align-items:center;gap:8px;font-size:13px;margin:-4px 0 14px;color:var(--mute)}
.switch-row{display:flex;justify-content:space-between;align-items:center;gap:14px;padding:12px 0;border-top:1px solid var(--line)}
.switch-row b{display:block}
.switch{position:relative;width:46px;height:26px;flex:none}
.switch input{opacity:0;width:100%;height:100%;position:absolute;inset:0;cursor:pointer;margin:0;z-index:1}
.switch i{position:absolute;inset:0;border-radius:99px;background:var(--line);transition:.2s}
.switch i:before{content:"";position:absolute;left:3px;top:3px;width:20px;height:20px;border-radius:50%;background:#fff;transition:.2s;box-shadow:0 1px 2px rgba(0,0,0,.3)}
.switch input:checked+i{background:var(--ok)}
.switch input:checked+i:before{transform:translateX(20px)}
.presets{display:flex;flex-wrap:wrap;gap:8px;margin-bottom:14px;align-items:center}
.presets span{color:var(--mute);font-size:12px;font-weight:600}

.nets{display:grid;gap:6px;margin:-4px 0 14px}
.net{display:flex;justify-content:space-between;align-items:center;gap:10px;padding:9px 12px;border:1px solid var(--line);border-radius:9px;background:var(--card2);cursor:pointer;font:inherit;color:var(--text);text-align:left}
.net:hover{border-color:var(--brand)}
.net small{color:var(--mute)}
.note{padding:10px 12px;border-radius:9px;background:var(--card2);border:1px solid var(--line);font-size:13px;margin-bottom:14px}
.note.ok{background:var(--okbg);border-color:transparent;color:var(--ok)}
.note.warn{background:var(--warnbg);border-color:transparent;color:var(--warn)}
.note.err{background:var(--errbg);border-color:transparent;color:var(--err)}

.term{background:#0d1117;border-radius:10px;padding:10px 12px;height:340px;overflow-y:auto;font:12px/1.5 ui-monospace,SFMono-Regular,Consolas,Menlo,monospace}
.ll{white-space:pre-wrap;word-break:break-word;padding:1px 0}
.lt{color:#6b7280;margin-right:8px}
.l0{color:#e5e7eb}.l1{color:#34d399}.l2{color:#fbbf24}.l3{color:#f87171;font-weight:700}.l4{color:#8b95a5}
.seg{display:inline-flex;border:1px solid var(--line);border-radius:9px;overflow:hidden}
.seg button{border:0;background:var(--card);color:var(--mute);padding:5px 11px;font:inherit;font-size:12px;font-weight:600;cursor:pointer}
.seg button.on{background:var(--brand);color:#fff}

.toast{position:fixed;left:50%;bottom:22px;transform:translateX(-50%) translateY(20px);opacity:0;pointer-events:none;background:#101828;color:#fff;padding:10px 16px;border-radius:10px;font-weight:600;box-shadow:0 8px 24px rgba(0,0,0,.25);transition:.25s;z-index:50;max-width:90vw}
.toast.show{opacity:1;transform:translateX(-50%) translateY(0)}
.toast.ok{background:#067647}.toast.err{background:#b42318}
.foot{text-align:center;color:var(--mute);font-size:12px;padding:6px 0 0}
)rawliteral";

const char APP_JS[] PROGMEM = R"rawliteral(
(function(){
'use strict';
var $=function(id){return document.getElementById(id)};
window.$=$;

window.api=async function(url,opt){
  var r=await fetch(url,opt),j=null;
  try{j=await r.json()}catch(e){}
  if(!r.ok||(j&&j.ok===false))throw new Error((j&&j.msg)||('HTTP '+r.status));
  return j;
};
window.post=function(url,obj){
  return window.api(url,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(obj||{})});
};

var tt=null;
window.toast=function(msg,kind){
  var t=$('toast');if(!t)return;
  t.textContent=msg;t.className='toast show '+(kind||'');
  clearTimeout(tt);tt=setTimeout(function(){t.className='toast'},3200);
};

window.fmtDur=function(s){
  s=Math.max(0,Math.round(s));
  if(s<60)return s+' s';
  if(s<3600){var m=Math.floor(s/60),r=s%60;return m+' min'+(r?' '+r+' s':'')}
  if(s<86400){var h=Math.floor(s/3600),m2=Math.floor((s%3600)/60);return h+' h'+(m2?' '+m2+' min':'')}
  var d=Math.floor(s/86400),h2=Math.floor((s%86400)/3600);return d+' d'+(h2?' '+h2+' h':'');
};
window.fmtAgo=function(s){return(s==null||s<0)?'never':(s<5?'just now':window.fmtDur(s)+' ago')};

var I={
 hive:'<path d="M21 16V8a2 2 0 0 0-1-1.73l-7-4a2 2 0 0 0-2 0l-7 4A2 2 0 0 0 3 8v8a2 2 0 0 0 1 1.73l7 4a2 2 0 0 0 2 0l7-4A2 2 0 0 0 21 16z"/><polyline points="3.27 6.96 12 12.01 20.73 6.96"/><line x1="12" y1="22.08" x2="12" y2="12"/>',
 thermo:'<path d="M14 14.76V3.5a2.5 2.5 0 0 0-5 0v11.26a4.5 4.5 0 1 0 5 0z"/>',
 drop:'<path d="M12 2.69l5.66 5.66a8 8 0 1 1-11.31 0z"/>',
 wind:'<path d="M9.59 4.59A2 2 0 1 1 11 8H2m10.59 11.41A2 2 0 1 0 14 16H2m15.73-8.27A2.5 2.5 0 1 1 19.5 12H2"/>',
 battery:'<rect x="1" y="6" width="18" height="12" rx="2" ry="2"/><line x1="23" y1="13" x2="23" y2="11"/>',
 zap:'<polygon points="13 2 3 14 12 14 11 22 21 10 12 10 13 2"/>',
 sun:'<circle cx="12" cy="12" r="5"/><path d="M12 1v2M12 21v2M4.22 4.22l1.42 1.42M18.36 18.36l1.42 1.42M1 12h2M21 12h2M4.22 19.78l1.42-1.42M18.36 5.64l1.42-1.42"/>',
 mic:'<path d="M12 1a3 3 0 0 0-3 3v8a3 3 0 0 0 6 0V4a3 3 0 0 0-3-3z"/><path d="M19 10v2a7 7 0 0 1-14 0v-2M12 19v4M8 23h8"/>',
 wifi:'<path d="M5 12.55a11 11 0 0 1 14.08 0M1.42 9a16 16 0 0 1 21.16 0M8.53 16.11a6 6 0 0 1 6.95 0"/><line x1="12" y1="20" x2="12.01" y2="20"/>',
 cpu:'<rect x="4" y="4" width="16" height="16" rx="2"/><rect x="9" y="9" width="6" height="6"/><path d="M9 1v3M15 1v3M9 20v3M15 20v3M20 9h3M20 14h3M1 9h3M1 14h3"/>',
 sliders:'<path d="M4 21v-7M4 10V3M12 21v-9M12 8V3M20 21v-5M20 12V3M1 14h6M9 8h6M17 16h6"/>',
 refresh:'<polyline points="23 4 23 10 17 10"/><polyline points="1 20 1 14 7 14"/><path d="M3.51 9a9 9 0 0 1 14.85-3.36L23 10M1 14l4.64 4.36A9 9 0 0 0 20.49 15"/>',
 power:'<path d="M18.36 6.64a9 9 0 1 1-12.73 0"/><line x1="12" y1="2" x2="12" y2="12"/>',
 download:'<path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="7 10 12 15 17 10"/><line x1="12" y1="15" x2="12" y2="3"/>',
 search:'<circle cx="11" cy="11" r="8"/><line x1="21" y1="21" x2="16.65" y2="16.65"/>',
 eye:'<path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"/><circle cx="12" cy="12" r="3"/>',
 term:'<polyline points="4 17 10 11 4 5"/><line x1="12" y1="19" x2="20" y2="19"/>',
 info:'<circle cx="12" cy="12" r="10"/><line x1="12" y1="16" x2="12" y2="12"/><line x1="12" y1="8" x2="12.01" y2="8"/>',
 check:'<polyline points="20 6 9 17 4 12"/>',
 x:'<line x1="18" y1="6" x2="6" y2="18"/><line x1="6" y1="6" x2="18" y2="18"/>',
 save:'<path d="M19 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h11l5 5v11a2 2 0 0 1-2 2z"/><polyline points="17 21 17 13 7 13 7 21"/><polyline points="7 3 7 8 15 8"/>'
};
window.icon=function(n){return'<svg class="ic"><use href="#i-'+n+'"/></svg>'};
var sp='<svg xmlns="http://www.w3.org/2000/svg" width="0" height="0" style="position:absolute">';
for(var k in I)sp+='<symbol id="i-'+k+'" viewBox="0 0 24 24">'+I[k]+'</symbol>';
document.body.insertAdjacentHTML('afterbegin',sp+'</svg>');
document.body.insertAdjacentHTML('beforeend','<div id="toast" class="toast"></div>');

// ---- header: device name + overall cloud status, refreshed from /info ----
var listeners=[];
window.onInfo=function(fn){listeners.push(fn);if(window.hiveInfo)fn(window.hiveInfo)};
function pill(cls,txt){var p=$('hdrPill');if(!p)return;p.className='pill '+cls;$('hdrPillTxt').textContent=txt}
async function pollInfo(){
  if(document.hidden)return;
  try{
    var i=await window.api('/info');
    window.hiveInfo=i;
    var s=$('hdrSub');
    if(s)s.textContent=i.device+' \u00b7 Hive '+i.hive+' \u00b7 v'+i.fw;
    if(i.cloud==='ok')pill('ok','Cloud online');
    else if(i.cloud==='retry')pill('warn','Upload retrying');
    else pill('','Waiting for first upload');
    listeners.forEach(function(f){try{f(i)}catch(e){console.log(e)}});
  }catch(e){pill('err','Device unreachable')}
}
window.pollInfo=pollInfo;
pollInfo();
setInterval(pollInfo,10000);
document.addEventListener('visibilitychange',function(){if(!document.hidden)pollInfo()});
})();
)rawliteral";

#endif
