// ======================================================
// DASHBOARD HTML
// ======================================================

#ifndef DASHBOARD_H
#define DASHBOARD_H
#include <Arduino.h>

const char INDEX_HTML[] PROGMEM = R"rawliteral(

<!DOCTYPE html>

<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">

<title>Smart Beehive Monitor</title>

<style>

*{
    margin:0;
    padding:0;
    box-sizing:border-box;
}

body{
    font-family:Arial, sans-serif;
    background:#f3f4f6;
    padding:10px;
}

.container{
    max-width:900px;
    margin:auto;
}

.header{
    background:#f59e0b;
    color:white;
    text-align:center;
    padding:15px;
    border-radius:12px;
    margin-bottom:10px;
}

.header h1{
    font-size:26px;
}

.header p{
    font-size:14px;
    margin-top:5px;
}

.status-bar{
    display:flex;
    flex-wrap:wrap;
    justify-content:center;
    gap:6px;
    margin-bottom:10px;
}

.badge{
    padding:6px 10px;
    border-radius:20px;
    color:white;
    font-size:12px;
    font-weight:bold;
}

.ok{
    background:#10b981;
}

.err{
    background:#ef4444;
}

.grid{
    display:grid;
    grid-template-columns:repeat(3,1fr);
    gap:8px;
}

.card{
    background:white;
    border-radius:12px;
    padding:12px;
    text-align:center;
    box-shadow:0 2px 6px rgba(0,0,0,0.1);
}

.icon{
    font-size:24px;
}

.value{
    font-size:22px;
    font-weight:bold;
    margin-top:5px;
    color:#111827;
}

.label{
    font-size:12px;
    color:#6b7280;
    margin-top:3px;
}

.audio-card{
    grid-column:span 3;
}

.footer{
    background:white;
    margin-top:10px;
    border-radius:12px;
    padding:10px;
    text-align:center;
    font-size:14px;
    box-shadow:0 2px 6px rgba(0,0,0,0.1);
}

.logHead{
    display:flex;
    flex-wrap:wrap;
    align-items:center;
    justify-content:space-between;
    gap:6px;
}

.logHead h3{
    font-size:16px;
}

.logTools{
    display:flex;
    flex-wrap:wrap;
    gap:4px;
}

.logTools button{
    border:1px solid #d1d5db;
    background:white;
    color:#374151;
    padding:4px 9px;
    border-radius:6px;
    font-size:12px;
    cursor:pointer;
}

.logTools button.active{
    background:#f59e0b;
    border-color:#f59e0b;
    color:white;
}

#logBox{
    background:#111827;
    border-radius:8px;
    margin-top:10px;
    padding:8px;
    height:320px;
    overflow-y:auto;
    text-align:left;
    font-family:ui-monospace,Consolas,Menlo,monospace;
    font-size:12px;
    line-height:1.45;
}

.logLine{
    padding:1px 0;
    white-space:pre-wrap;
    word-break:break-word;
}

.logTime{
    color:#6b7280;
    margin-right:6px;
}

.lv0{ color:#e5e7eb; }
.lv1{ color:#34d399; }
.lv2{ color:#fbbf24; }
.lv3{ color:#f87171; font-weight:bold; }
.lv4{ color:#9ca3af; }

.logLegend{
    margin-top:6px;
    font-size:11px;
    color:#6b7280;
    text-align:left;
}

@media(max-width:600px)
{
    .grid{
        grid-template-columns:repeat(2,1fr);
    }

    .audio-card{
        grid-column:span 2;
    }

    .header h1{
        font-size:20px;
    }

    .value{
        font-size:18px;
    }
}

</style>

</head>

<body>

<div class="container">

<div class="header">
    <h1>🐝 Smart Beehive Monitor</h1>
    <p>Hive 12 Live Dashboard</p>
</div>

<div id="statusBar" class="status-bar"></div>

<div class="grid">

    <div class="card">
        <div class="icon">🌡️</div>
        <div id="temp" class="value">--</div>
        <div class="label">Temperature (°C)</div>
    </div>

    <div class="card">
        <div class="icon">💧</div>
        <div id="hum" class="value">--</div>
        <div class="label">Humidity (%)</div>
    </div>

    <div class="card">
        <div class="icon">💨</div>
        <div id="co2" class="value">--</div>
        <div class="label">CO₂ (ppm)</div>
    </div>

    <div class="card">
        <div class="icon">🔋</div>
        <div id="battery" class="value">--</div>
        <div class="label">Battery (V)</div>
    </div>

    <div class="card">
        <div class="icon">⚡</div>
        <div id="current" class="value">--</div>
        <div class="label">Current (A)</div>
    </div>

    <div class="card">
        <div class="icon">☀️</div>
        <div id="solar" class="value">--</div>
        <div class="label">Solar Power (W)</div>
    </div>

    <div class="card audio-card">
        <div class="icon">🎤</div>
        <div id="audio" class="value">--</div>
        <div class="label">Audio Peak</div>
    </div>

</div>
<div style="text-align:center;margin-top:15px">

<a href="/settingsPage">
<button
style="
padding:10px 20px;
background:#f59e0b;
color:white;
border:none;
border-radius:8px;">
⚙ Settings
</button>
</a>

</div>

<div class="card" style="margin-top:10px">

    <div class="logHead">
        <h3>📟 Live Log</h3>

        <div class="logTools">
            <button id="fAll" class="active" onclick="setFilter('all')">All</button>
            <button id="fNoDbg" onclick="setFilter('nodbg')">No debug</button>
            <button id="fProblems" onclick="setFilter('problems')">Problems</button>
            <button onclick="clearLogs()">Clear</button>
        </div>
    </div>

    <div id="logBox">Loading...</div>

    <div class="logLegend">
        <span style="color:#10b981">&#9632; success</span>
        <span style="color:#f59e0b">&#9632; warning</span>
        <span style="color:#ef4444">&#9632; error</span>
        <span style="color:#9ca3af">&#9632; debug</span>
    </div>
</div>

<div class="footer">
    Last Update:
    <span id="lastUpdated">--</span>
</div>


</div>

<script>
const LOG_KEEP = 400;
const store = new Map();     // seq -> {s,t,l,m}
let lastSeq = 0;
let bootId = null;
let logFilter = "all";       // all | nodbg | problems

function setFilter(f)
{
    logFilter = f;
    document.getElementById("fAll").className      = f === "all"      ? "active" : "";
    document.getElementById("fNoDbg").className    = f === "nodbg"    ? "active" : "";
    document.getElementById("fProblems").className = f === "problems" ? "active" : "";
    renderLogs(true);
}

function clearLogs()
{
    store.clear();      // lastSeq is kept, so old lines do not come back
    renderLogs(true);
}

function renderLogs(forceBottom)
{
    const box = document.getElementById("logBox");
    const atBottom = forceBottom ||
        box.scrollTop + box.clientHeight >= box.scrollHeight - 25;

    const seqs = Array.from(store.keys()).sort((a, b) => a - b);
    const frag = document.createDocumentFragment();

    seqs.forEach(s =>
    {
        const e = store.get(s);

        if (logFilter === "nodbg" && e.l === 4) return;
        if (logFilter === "problems" && e.l !== 2 && e.l !== 3) return;

        const line = document.createElement("div");
        line.className = "logLine lv" + e.l;

        const time = document.createElement("span");
        time.className = "logTime";
        time.textContent = e.t;

        const msg = document.createElement("span");
        msg.textContent = e.m;          // textContent: log text is never parsed as HTML

        line.appendChild(time);
        line.appendChild(msg);
        frag.appendChild(line);
    });

    box.innerHTML = "";
    if (!frag.childNodes.length)
    {
        const empty = document.createElement("div");
        empty.className = "logLine lv4";
        empty.textContent = logFilter === "problems" ? "No warnings or errors" : "No log lines yet";
        box.appendChild(empty);
    }
    else
    {
        box.appendChild(frag);
    }

    if (atBottom) box.scrollTop = box.scrollHeight;
}

function addLogs(list)
{
    list.forEach(e => store.set(e.s, e));

    if (store.size > LOG_KEEP)
    {
        const seqs = Array.from(store.keys()).sort((a, b) => a - b);
        seqs.slice(0, store.size - LOG_KEEP).forEach(s => store.delete(s));
    }
}

async function updateLogs()
{
    try
    {
        const r = await fetch('/logs?since=' + lastSeq);
        const d = await r.json();

        if (bootId !== null && d.boot !== bootId)
        {
            // device rebooted: its line numbers started again from 1
            bootId = d.boot;
            store.clear();
            lastSeq = 0;
            return updateLogs();
        }

        const firstLoad = (bootId === null);
        bootId = d.boot;

        addLogs(d.logs);
        lastSeq = d.last;

        if (firstLoad)
        {
            // warnings and errors that already scrolled out of the main buffer
            const p = await fetch('/logs?problems=1');
            const pd = await p.json();
            addLogs(pd.logs);
        }

        renderLogs(firstLoad);
    }
    catch (err)
    {
        console.log(err);
    }
}

async function updateData()
{
    try
    {
        const response = await fetch('/data');
        const data = await response.json();

        document.getElementById("temp").innerHTML =
            Number(data.temp).toFixed(1);

        document.getElementById("hum").innerHTML =
            Number(data.humidity).toFixed(1);

        document.getElementById("co2").innerHTML =
            data.co2;

        document.getElementById("battery").innerHTML =
            Number(data.battV).toFixed(2);

        document.getElementById("current").innerHTML =
            Number(data.battA).toFixed(3);

        document.getElementById("solar").innerHTML =
            Number(data.solW).toFixed(2);

        document.getElementById("audio").innerHTML =
            data.audio;

        const statusBar =
            document.getElementById("statusBar");

        statusBar.innerHTML = "";

        const sensors = [
            ["CO₂", data.co2_ok],
            ["Battery", data.batt_ok],
            ["Solar", data.solar_ok],
            ["Mic", data.mic_ok],
            ["SD", data.sd_ok],
            ["Cloud", data.cloud_ok]
        ];

        sensors.forEach(sensor =>
        {
            const badge =
                document.createElement("span");

            badge.className =
                "badge " +
                (sensor[1] ? "ok" : "err");

            badge.innerHTML =
                (sensor[1] ? "✓ " : "✗ ") +
                sensor[0];

            statusBar.appendChild(badge);
        });

        document.getElementById("lastUpdated")
            .innerHTML =
            new Date().toLocaleTimeString();
    }
    catch(err)
    {
        console.log(err);
    }
}

updateData();
setInterval(updateData, 2000);
updateLogs();
setInterval(updateLogs, 2000);
</script>

</body>
</html>
)rawliteral";

#endif