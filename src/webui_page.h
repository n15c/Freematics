// Single page web UI (German). Served from flash.
#pragma once
#include <pgmspace.h>

static const char WEB_PAGE[] PROGMEM = R"rawliteral(<!doctype html>
<html lang="de"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Freematics Telelogger</title>
<style>
:root{--bg:#f4f6f8;--card:#fff;--fg:#1d2733;--mut:#6b7785;--acc:#0b6bcb;--ok:#1e8e3e;--warn:#c77700;--err:#c5221f;--line:#dde3ea}
@media(prefers-color-scheme:dark){:root{--bg:#12161b;--card:#1b2128;--fg:#e6ebf0;--mut:#93a1b0;--acc:#5aa6ff;--line:#2c353f}}
*{box-sizing:border-box}body{margin:0;font:15px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif;background:var(--bg);color:var(--fg)}
header{display:flex;align-items:center;gap:12px;padding:12px 16px;background:var(--card);border-bottom:1px solid var(--line);position:sticky;top:0;z-index:2}
header h1{font-size:17px;margin:0;flex:1}nav{display:flex;gap:4px;overflow-x:auto;padding:8px 16px;background:var(--card);border-bottom:1px solid var(--line)}
nav button{border:0;background:none;color:var(--mut);padding:8px 12px;border-radius:8px;font:inherit;cursor:pointer;white-space:nowrap}
nav button.on{background:var(--acc);color:#fff}main{max-width:980px;margin:0 auto;padding:16px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:12px}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px 16px;margin-bottom:12px}
.card h2{font-size:14px;text-transform:uppercase;letter-spacing:.04em;color:var(--mut);margin:0 0 10px}
.kv{display:grid;grid-template-columns:auto 1fr;gap:4px 12px}.kv span:nth-child(odd){color:var(--mut)}
.pill{display:inline-block;padding:1px 8px;border-radius:99px;font-size:13px;background:var(--line)}
.ok{background:var(--ok);color:#fff}.warn{background:var(--warn);color:#fff}.err{background:var(--err);color:#fff}
label{display:block;margin:10px 0 4px;color:var(--mut);font-size:13px}
input,select{width:100%;padding:8px 10px;border:1px solid var(--line);border-radius:8px;background:var(--bg);color:var(--fg);font:inherit}
input[type=checkbox]{width:auto;margin-right:6px}.chk{display:flex;align-items:center;margin-top:10px;color:var(--fg)}
.row{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:0 12px}
.btn{border:0;background:var(--acc);color:#fff;padding:9px 16px;border-radius:8px;font:inherit;cursor:pointer}
.btn.sec{background:var(--line);color:var(--fg)}.btn.dan{background:var(--err)}
.bar{display:flex;gap:8px;flex-wrap:wrap;margin-top:12px}
table{width:100%;border-collapse:collapse;font-size:14px}th,td{text-align:left;padding:6px 4px;border-bottom:1px solid var(--line);vertical-align:middle}
th{color:var(--mut);font-weight:500}td input,td select{padding:5px 6px}.tbl{overflow-x:auto}
.hint{color:var(--mut);font-size:13px;margin:6px 0 0}.err-t{color:var(--err)}
#toast{position:fixed;bottom:16px;left:50%;transform:translateX(-50%);background:var(--fg);color:var(--bg);padding:10px 16px;border-radius:8px;display:none}
.hide{display:none}
</style></head><body>
<header><h1>Freematics Telelogger <small id="ver" style="font-weight:400;color:var(--mut)"></small></h1><span id="hdr" class="pill">…</span></header>
<nav id="nav"><button data-t="status" class="on">Status</button><button data-t="server">Server</button><button data-t="net">Netzwerk</button><button data-t="vehicle">Fahrzeug</button><button data-t="can">CAN</button><button data-t="power">Energie</button><button data-t="log">Log</button><button data-t="system">System</button></nav>
<main>
<section id="t-status">
 <div class="grid">
  <div class="card"><h2>Fahrzeug</h2><div class="kv" id="s-veh"></div></div>
  <div class="card"><h2>Position</h2><div class="kv" id="s-gnss"></div></div>
  <div class="card"><h2>Verbindung</h2><div class="kv" id="s-net"></div></div>
 </div>
 <div class="card"><h2>Messwerte</h2><div class="tbl"><table><thead><tr><th>Name</th><th>Wert</th><th>Alter</th></tr></thead><tbody id="s-vals"></tbody></table></div></div>
 <div class="card"><h2>Gerät</h2><div class="kv" id="s-dev"></div></div>
</section>

<section id="t-server" class="hide"><div class="card"><h2>Protokoll (OsmAnd)</h2>
 <label>Server-URL</label><input id="serverUrl" placeholder="http://traccar.example.com:5055/">
 <p class="hint">Traccar: <code>http://&lt;server&gt;:5055/</code> &middot; Home Assistant (Traccar Client Integration): Webhook-URL eintragen. Leer = kein Upload.</p>
 <label>Zusätzliches Live-Ziel (optional, nur aktuellste Position)</label><input id="liveUrl" placeholder="https://homeassistant.local:8123/api/webhook/…">
 <div class="row">
  <div><label>Geräte-ID</label><input id="deviceId" placeholder=""></div>
  <div><label>HTTP-Methode</label><select id="httpMethod"><option value="0">GET (Query)</option><option value="1">POST (Formular)</option></select></div>
 </div>
 <p class="hint">Leere Geräte-ID = <b id="autoId"></b>. Diese ID muss in Traccar als „Identifier“ eingetragen sein.</p>
 <div class="row">
  <div><label>Intervall Fahrt (s)</label><input id="intervalDriving" type="number" min="1"></div>
  <div><label>Intervall Zündung aus (s)</label><input id="intervalIdle" type="number" min="5"></div>
  <div><label>Heartbeat geparkt (min, 0=aus)</label><input id="heartbeatMin" type="number" min="0"></div>
  <div><label>Extra-Punkt bei Kursänderung (°)</label><input id="headingChange" type="number" min="0" max="180"></div>
 </div></div>
 <div class="bar"><button class="btn" onclick="save()">Speichern &amp; neu starten</button></div>
</section>

<section id="t-net" class="hide">
 <div class="card"><h2>WLAN (Client)</h2>
  <div class="row"><div><label>SSID</label><input id="wifiSsid"></div><div><label>Passwort</label><input id="wifiPass" type="password"></div></div>
  <p class="hint">Ist das WLAN erreichbar (z.B. zu Hause), wird darüber gesendet und das Mobilfunkmodem schläft.</p></div>
 <div class="card"><h2>WLAN Access Point (Web UI)</h2>
  <div class="row"><div><label>AP-Name (leer = automatisch)</label><input id="apSsid"></div><div><label>AP-Passwort (min. 8 Zeichen, leer = offen)</label><input id="apPass" type="password"></div>
  <div><label>AP aktiv</label><select id="apMode"><option value="0">immer (wenn wach)</option><option value="1">nur ohne WLAN-Verbindung</option><option value="2">aus</option></select></div>
  <div><label>Web-UI Passwort (Benutzer admin)</label><input id="webPass" type="password"></div></div></div>
 <div class="card"><h2>Mobilfunk</h2>
  <label class="chk"><input type="checkbox" id="cellEnabled">Mobilfunk verwenden</label>
  <div class="row"><div><label>APN (leer = automatisch)</label><input id="apn"></div><div><label>APN Benutzer</label><input id="apnUser"></div><div><label>APN Passwort</label><input id="apnPass" type="password"></div><div><label>SIM-PIN</label><input id="simPin" type="password"></div>
  <div><label>Netz</label><select id="netMode"><option value="0">Automatisch</option><option value="1">nur LTE</option><option value="2">nur 2G</option></select></div>
  <div><label>Modem im Standby</label><select id="modemStandby"><option value="0">ausschalten</option><option value="1">Funk aus (schneller Start)</option><option value="2">online bleiben</option></select></div></div>
  <p class="hint">Roaming wird automatisch unterstützt (LTE/EPS-Registrierung, automatische Netzwahl). Bei IoT-SIMs ggf. APN angeben.</p></div>
 <div class="bar"><button class="btn" onclick="save()">Speichern &amp; neu starten</button></div>
</section>

<section id="t-vehicle" class="hide">
 <div class="card"><h2>OBD-II</h2>
  <label class="chk"><input type="checkbox" id="obdEnabled">OBD-II abfragen</label>
  <label class="chk"><input type="checkbox" id="speedFromObd">Geschwindigkeit aus OBD senden statt GPS (PID 0D muss in der Liste sein; GPS-Wert als gpsSpeed)</label>
  <div class="row"><div><label>Protokoll</label><select id="obdProtocol"><option value="0">Automatisch (letztes merken)</option><option value="6">CAN 11bit 500k</option><option value="8">CAN 29bit 250k</option><option value="7">CAN 29bit 500k</option><option value="9">CAN 11bit 250k</option><option value="3">ISO 9141-2</option><option value="4">KWP2000 5baud</option><option value="5">KWP2000 fast</option></select></div></div>
  <div class="tbl"><table><thead><tr><th>PID (hex)</th><th>Attribut</th><th>Intervall s</th><th></th></tr></thead><tbody id="pids"></tbody></table></div>
  <div class="bar"><button class="btn sec" onclick="addPid()">+ PID</button></div>
  <p class="hint">Attributnamen wie in Traccar: rpm, obdSpeed, coolantTemp, engineLoad, throttle, fuel, obdOdometer (in Metern, wie Traccar es erwartet) … Intervall 0 = jede Runde.</p></div>
 <div class="bar"><button class="btn" onclick="save()">Speichern &amp; neu starten</button></div>
</section>

<section id="t-can" class="hide">
 <div class="card"><h2>CAN-Signale</h2>
  <label class="chk"><input type="checkbox" id="canEnabled">CAN-Signale lesen</label>
  <div class="row"><div><label>CAN-Bus (falls OBD aus)</label><select id="canProtocol"><option value="6">11bit 500k</option><option value="9">11bit 250k</option><option value="7">29bit 500k</option><option value="8">29bit 250k</option></select></div></div>
  <div class="tbl"><table><thead><tr><th>Attribut</th><th>Typ</th><th>CAN-ID</th><th>Sende-ID</th><th>Anfrage</th><th>Startbit</th><th>Bits</th><th>BE</th><th>±</th><th>Faktor</th><th>Offset</th><th>s</th><th></th></tr></thead><tbody id="cans"></tbody></table></div>
  <div class="bar"><button class="btn sec" onclick="addCan()">+ Signal</button></div>
  <p class="hint"><b>Passiv</b>: liest Broadcast-Frames mit der CAN-ID (hex) mit. <b>Anfrage</b>: sendet z.B. <code>22F40D</code> (UDS) an die Sende-ID (z.B. 7E0) und wertet die positive Antwort aus; Startbit zählt ab dem ersten Datenbyte nach SID+DID. <b>BE</b> = Big-Endian (Bit 0 = MSB von Byte 0, Byte n → n·8), sonst Little-Endian/Intel wie in DBC. Wert = Rohwert × Faktor + Offset.</p></div>
 <div class="card"><h2>CAN-Monitor</h2>
  <p class="hint">Zeigt alle empfangenen CAN-IDs (nur mit verbundenem Fahrzeug, Zündung an).</p>
  <div class="bar"><button class="btn sec" onclick="mon()">30 s mitschneiden</button></div>
  <div class="tbl"><table><thead><tr><th>ID</th><th>Daten</th><th>Anzahl</th><th>Alter ms</th></tr></thead><tbody id="frames"></tbody></table></div></div>
 <div class="bar"><button class="btn" onclick="save()">Speichern &amp; neu starten</button></div>
</section>

<section id="t-power" class="hide"><div class="card"><h2>Zündung, Standby &amp; Aufwachen</h2>
 <div class="row">
  <div><label>Zündung an ab Spannung (V)</label><input id="runVoltage" type="number" step="0.1"></div>
  <div><label>Aufwachen bei Spannungsanstieg (V)</label><input id="wakeDelta" type="number" step="0.05"></div>
  <div><label>Bewegungsschwelle (g)</label><input id="motionG" type="number" step="0.01"></div>
  <div><label>Standby nach Zündung aus (s)</label><input id="standbyDelay" type="number" min="10"></div>
  <div><label>Batterieschutz unter (V, 0 = aus)</label><input id="lowVoltage" type="number" step="0.1"></div>
 </div>
 <label class="chk"><input type="checkbox" id="gnssKeepOn">GNSS im Standby eingeschaltet lassen (Fix nach dem Start in Sekunden statt ca. 30–60 s, ca. 25–40 mA)</label>
 <div class="row"><div><label>… für Minuten nach dem Abstellen</label><input id="gnssKeepMin" type="number" min="0" max="1440"></div></div>
 <label class="chk"><input type="checkbox" id="gnssUseModem">GNSS des Modems verwenden, falls kein GNSS-Empfänger vorhanden</label>
 <p class="hint">Batterieschutz: Fällt die Spannung im Standby 1 Minute unter den Wert, werden Modem und GNSS ganz ausgeschaltet und keine Heartbeats mehr gesendet. Ab +0,3 V oder beim Motorstart wird wieder normal gearbeitet. GNSS bleibt höchstens 2 h warm, das Modem höchstens 24 h im Flugmodus. Das Gerät wacht ohne Neustart auf, sobald die Bordspannung über die Schwelle steigt (Motor/Lichtmaschine), sie um den eingestellten Wert ansteigt oder Bewegung erkannt wird.</p></div>
 <div class="bar"><button class="btn" onclick="save()">Speichern &amp; neu starten</button></div>
</section>

<section id="t-log" class="hide"><div class="card"><h2>Log</h2>
 <div class="bar" style="margin-top:0;align-items:center"><label class="chk" style="margin:0"><input type="checkbox" id="logAuto" checked>Live</label><label class="chk" style="margin:0"><input type="checkbox" id="logScroll" checked>Mitscrollen</label>
 <button class="btn sec" onclick="logLoad(true)">Neu laden</button><button class="btn sec" onclick="logSave()">Herunterladen</button></div>
 <p class="hint">Zeiten in UTC (bzw. +Sekunden seit Start, solange die Uhrzeit unbekannt ist). Der Log überlebt Neustarts und Updates, nicht aber das Ausstecken.</p>
 <pre id="logBox" style="height:60vh;overflow:auto;background:var(--bg);border:1px solid var(--line);border-radius:8px;padding:8px;font:12px/1.4 ui-monospace,Menlo,Consolas,monospace;white-space:pre-wrap;word-break:break-all;margin:10px 0 0"></pre></div></section>
<section id="t-system" class="hide">
 <div class="card"><h2>Automatisches Update (GitHub)</h2>
  <label class="chk"><input type="checkbox" id="autoUpdate">Neue Firmware automatisch installieren (nur bei Zündung aus, alle 6 h geprüft)</label>
  <label class="chk"><input type="checkbox" id="otaCellular">Auch über Mobilfunk laden (nie im Roaming, ca. 1,4 MB pro Update)</label>
  <label>Update-Quelle</label><input id="updateUrl" placeholder="https://raw.githubusercontent.com/&lt;user&gt;/&lt;repo&gt;/firmware/">
  <p class="hint">Status: <b id="otaRes">…</b></p>
  <div class="bar"><button class="btn sec" onclick="post('/api/ota/check','Prüfung gestartet…')">Jetzt prüfen &amp; installieren</button><button class="btn" onclick="save()">Speichern &amp; neu starten</button></div></div>
 <div class="card"><h2>Firmware-Update (Datei)</h2><p class="hint">Installiert: <b id="fwcur">…</b></p><input type="file" id="fw" accept=".bin"><div class="bar"><button class="btn" onclick="ota()">Hochladen</button><span id="otap"></span></div></div>
 <div class="card"><h2>Wartung</h2><div class="bar">
  <button class="btn sec" onclick="post('/api/restart','Neustart…')">Neu starten</button>
  <button class="btn sec" onclick="if(confirm('Warteschlange löschen?'))post('/api/queue/clear','Gelöscht')">Warteschlange löschen</button>
  <button class="btn dan" onclick="if(confirm('Alle Einstellungen zurücksetzen?'))post('/api/reset','Zurückgesetzt, Neustart…')">Werkseinstellungen</button></div></div>
 <div class="card"><h2>Konfiguration (JSON)</h2><div class="bar"><button class="btn sec" onclick="exp()">Exportieren</button><input type="file" id="imp" accept=".json" style="width:auto"><button class="btn sec" onclick="impc()">Importieren</button></div></div>
</section>
</main><div id="toast"></div>
<script>
const $=id=>document.getElementById(id);let C={},S={};
const fields=['serverUrl','liveUrl','deviceId','httpMethod','intervalDriving','intervalIdle','heartbeatMin','headingChange','wifiSsid','wifiPass','apSsid','apPass','apMode','webPass','cellEnabled','apn','apnUser','apnPass','simPin','netMode','modemStandby','obdEnabled','speedFromObd','obdProtocol','canEnabled','canProtocol','autoUpdate','otaCellular','updateUrl','runVoltage','wakeDelta','motionG','standbyDelay','lowVoltage','gnssKeepOn','gnssKeepMin','gnssUseModem'];
function toast(t){const e=$('toast');e.textContent=t;e.style.display='block';clearTimeout(e._t);e._t=setTimeout(()=>e.style.display='none',3000)}
document.querySelectorAll('nav button').forEach(b=>b.onclick=()=>{document.querySelectorAll('nav button').forEach(x=>x.classList.toggle('on',x==b));document.querySelectorAll('main section').forEach(s=>s.classList.toggle('hide',s.id!='t-'+b.dataset.t))});
const esc=s=>String(s??'').replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const kv=(el,rows)=>$(el).innerHTML=rows.filter(r=>r).map(r=>`<span>${r[0]}</span><span>${r[1]}</span>`).join('');
const pill=(t,c)=>`<span class="pill ${c||''}">${esc(t)}</span>`;
const hex=(n,w)=>Number(n||0).toString(16).toUpperCase().padStart(w||3,'0');
const dur=s=>s<0?'–':s<60?s+' s':s<3600?Math.round(s/60)+' min':(s/3600).toFixed(1)+' h';
async function status(){try{const r=await fetch('/api/status');S=await r.json();}catch(e){$('hdr').className='pill err';$('hdr').textContent='offline';return}
 const st={driving:['Fahrt','ok'],idle:['Zündung aus','warn'],standby:['Standby',''],boot:['Start','']}[S.state]||[S.state];
 $('hdr').className='pill '+st[1];$('hdr').textContent=st[0];$('ver').textContent='v'+S.fw+' · Build '+S.build;$('fwcur').textContent=S.fw+' Build '+S.build+' ('+S.commit+', '+S.buildTime+', '+S.partition+')';otaCheck();if(S.ota){$('otaRes').textContent=S.ota.result+(S.ota.busy&&S.ota.progress>=0?' '+S.ota.progress+' %':'')}
 const v=S.vehicle,g=S.gnss,n=S.net;
 kv('s-veh',[['Zündung',S.ignition?pill('an','ok'):pill('aus')],['Spannung',S.voltage+' V'],['OBD',v.obd?pill('verbunden','ok'):pill('nein')],v.vin&&['VIN',esc(v.vin)],v.dtc.length&&['Fehlercodes',v.dtc.map(d=>pill(d,'err')).join(' ')],['Bewegung',S.motion?'ja':'nein'],S.standbyBlocker&&!S.ignition&&['Kein Standby weil',esc(S.standbyBlocker)]]);
 kv('s-gnss',[['Fix',g.fix?pill('ja','ok'):pill(g.lat?'veraltet':'nein','warn')],g.lat&&['Position',`<a href="https://www.openstreetmap.org/?mlat=${g.lat}&mlon=${g.lon}#map=15/${g.lat}/${g.lon}" target="_blank">${g.lat}, ${g.lon}</a>`],g.lat&&['Tempo',Math.round(g.speed)+' km/h'],g.lat&&['Satelliten',g.sat+' (HDOP '+(+g.hdop).toFixed(1)+')'],g.lat&&['Alter',dur(g.age)],['Quelle',g.source],g.rejected&&['Verworfen',g.rejected]]);
 kv('s-net',[['Verbindung',n.link=='none'?pill('keine','err'):pill(n.link=='wifi'?'WLAN':'Mobilfunk','ok')],n.modem&&['Modem',esc(n.model)],n.operator&&['Netz',esc(n.operator)+' '+esc(n.netType)+(n.roaming?' '+pill('Roaming','warn'):'')],n.modem&&['Registriert',n.registered?'ja':'nein'],n.rssi&&['Signal',n.rssi+' dBm'],['Gesendet',n.sent+' ('+n.failed+' Fehler)'],['Letzter Upload',dur(n.lastUpload)],['Warteschlange',n.queue+(n.queueFile?' ('+n.queueFile+' im Flash)':'')],n.error&&['Fehler','<span class="err-t">'+esc(n.error)+'</span>']]);
 $('s-vals').innerHTML=v.values.map(x=>`<tr><td>${esc(x.key)}</td><td>${x.key=='obdOdometer'?(x.value/1000).toFixed(1)+' km':+x.value.toFixed(3)}</td><td>${dur(x.age)}</td></tr>`).join('')||'<tr><td colspan=3>–</td></tr>';
 kv('s-dev',[['Geräte-ID',esc(S.id)],['Firmware',S.fw+' Build '+S.build],['Branch',esc(S.branch)],['Commit',esc(S.commit)],['Gebaut',esc(S.buildTime)],['Partition',esc(S.partition)],['Laufzeit',dur(S.uptime)],n.imei&&['IMEI',n.imei],n.iccid&&['ICCID',n.iccid],n.wifiIp&&['WLAN-IP',n.wifiIp],n.apIp&&['AP-IP',n.apIp],n.cellIp&&['Mobil-IP',n.cellIp],['Typ',S.devType],['Temperatur',S.temp+' °C'],['Freier RAM',Math.round(S.heap/1024)+' KB']]);
 $('autoId').textContent=S.id;}
function pidRow(p){const tr=document.createElement('tr');tr.innerHTML=`<td><input value="${hex(p.pid,2)}" size=4></td><td><input value="${esc(p.key)}"></td><td><input type=number value="${p.interval||0}" style="width:70px"></td><td><button class="btn sec" onclick="this.closest('tr').remove()">✕</button></td>`;$('pids').appendChild(tr)}
function addPid(){pidRow({pid:0x0d,key:'',interval:0})}
function canRow(c){const tr=document.createElement('tr');tr.innerHTML=`<td><input value="${esc(c.key)}" style="min-width:90px"></td><td><select><option value=0 ${c.mode==0?'selected':''}>Passiv</option><option value=1 ${c.mode==1?'selected':''}>Anfrage</option></select></td><td><input value="${hex(c.id)}" size=8></td><td><input value="${c.txId?hex(c.txId):''}" size=8></td><td><input value="${esc(c.request)}" size=8></td><td><input type=number value="${c.startBit||0}" style="width:60px"></td><td><input type=number value="${c.bitLen||8}" style="width:55px"></td><td><input type=checkbox ${c.bigEndian?'checked':''}></td><td><input type=checkbox ${c.signed?'checked':''}></td><td><input value="${c.factor??1}" size=6></td><td><input value="${c.offset??0}" size=6></td><td><input type=number value="${c.interval||0}" style="width:55px"></td><td><button class="btn sec" onclick="this.closest('tr').remove()">✕</button></td>`;$('cans').appendChild(tr)}
function addCan(){canRow({key:'',mode:1,id:0x7E8,txId:0x7E0,request:'22',startBit:0,bitLen:8,factor:1,offset:0})}
async function load(){C=await (await fetch('/api/config')).json();for(const f of fields){const e=$(f);if(!e)continue;if(e.type=='checkbox')e.checked=!!C[f];else e.value=C[f]??''}
 $('pids').innerHTML='';C.pids.forEach(pidRow);$('cans').innerHTML='';C.can.forEach(canRow)}
function collect(){const o={};for(const f of fields){const e=$(f);if(!e)continue;o[f]=e.type=='checkbox'?e.checked:(e.type=='number'||e.tagName=='SELECT')?Number(e.value):e.value}
 o.pids=[...$('pids').rows].map(r=>{const i=r.querySelectorAll('input');return{pid:parseInt(i[0].value,16)||0,key:i[1].value.trim(),interval:+i[2].value||0}}).filter(p=>p.pid&&p.key);
 o.can=[...$('cans').rows].map(r=>{const i=r.querySelectorAll('input,select');return{key:i[0].value.trim(),mode:+i[1].value,id:parseInt(i[2].value,16)||0,txId:parseInt(i[3].value,16)||0,request:i[4].value.trim().replace(/\s/g,'').toUpperCase(),startBit:+i[5].value,bitLen:+i[6].value,bigEndian:i[7].checked,signed:i[8].checked,factor:parseFloat(i[9].value)||1,offset:parseFloat(i[10].value)||0,interval:+i[11].value||0}}).filter(c=>c.key);
 return o}
async function save(){const r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(collect())});toast(r.ok?'Gespeichert – Gerät startet neu…':'Fehler beim Speichern');if(r.ok)setTimeout(()=>location.reload(),8000)}
async function post(u,t){await fetch(u,{method:'POST'});toast(t)}
async function mon(){await fetch('/api/can/monitor?seconds=30',{method:'POST'});toast('Monitor läuft 30 s');let n=0;const iv=setInterval(async()=>{const d=await (await fetch('/api/can/frames')).json();$('frames').innerHTML=d.frames.sort((a,b)=>a.id-b.id).map(f=>`<tr><td>${hex(f.id)}</td><td><code>${f.data}</code></td><td>${f.count}</td><td>${f.age}</td></tr>`).join('');if(++n>32)clearInterval(iv)},1000)}
function otaCheck(){let p;try{p=JSON.parse(sessionStorage.getItem('ota')||'null')}catch(e){}if(!p)return;const el=(Date.now()-p.t)/1000;if(el>600){sessionStorage.removeItem('ota');return}if(S.uptime>el+5)return;sessionStorage.removeItem('ota');if(S.build!=p.build||S.commit!=p.commit)toast('Update erfolgreich: Build '+p.build+' → '+S.build+(S.branch?' ('+S.branch+')':''));else alert('Achtung: Nach dem Neustart läuft immer noch Build '+S.build+' ('+S.commit+', Partition '+S.partition+', vorher '+p.part+'). Details im Log unter [OTA] und [SYS].')}
function ota(){const f=$('fw').files[0];if(!f)return;const fd=new FormData();fd.append('fw',f);const x=new XMLHttpRequest();x.upload.onprogress=e=>$('otap').textContent=Math.round(e.loaded*100/e.total)+' %';x.onload=()=>{let r={};try{r=JSON.parse(x.responseText)}catch(e){}if(x.status==200){try{sessionStorage.setItem('ota',JSON.stringify({build:S.build,commit:S.commit,part:S.partition,t:Date.now()}))}catch(e){}toast('Update übertragen – Neustart…');setTimeout(()=>location.reload(),12000)}else alert('Update fehlgeschlagen: '+(r.error||('HTTP '+x.status)))};x.open('POST','/api/update');x.send(fd)}
function exp(){const a=document.createElement('a');a.href=URL.createObjectURL(new Blob([JSON.stringify(collect(),null,1)],{type:'application/json'}));a.download='telelogger-config.json';a.click()}
async function impc(){const f=$('imp').files[0];if(!f)return;const r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:await f.text()});toast(r.ok?'Importiert – Neustart…':'Import fehlgeschlagen')}
let logPos=0,logBusy=false;
async function logLoad(reset){if(logBusy)return;logBusy=true;try{if(reset){logPos=0;$('logBox').textContent=''}const r=await fetch('/api/log?from='+logPos);const t=await r.text();const nx=+r.headers.get('X-Log-Next');if(nx<logPos)$('logBox').textContent='';logPos=nx||logPos;if(t){const b=$('logBox');b.textContent=(b.textContent+t).slice(-200000);if($('logScroll').checked)b.scrollTop=b.scrollHeight}}catch(e){}logBusy=false}
function logSave(){const a=document.createElement('a');a.href=URL.createObjectURL(new Blob([$('logBox').textContent],{type:'text/plain'}));a.download='telelogger-'+(S.id||'log')+'-'+new Date().toISOString().slice(0,19).replace(/:/g,'')+'.txt';a.click()}
setInterval(()=>{if(!$('t-log').classList.contains('hide')&&$('logAuto').checked)logLoad(false)},2000);
document.querySelector('nav button[data-t=log]').addEventListener('click',()=>logLoad(false));
load();status();setInterval(status,2000);
</script></body></html>)rawliteral";
