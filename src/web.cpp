// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andres Leroi
#include "web.h"

#include <ESPmDNS.h>
#include <LilyGoLib.h>
#include <SD.h>
#include <WebServer.h>
#include <WiFi.h>

#include <ft8_adif.h>

#include "assets/assets.h"
#include "engine.h"
#include "logbook.h"
#include "schedule.h"
#include "settings.h"
#include "timebase.h"
#include "ui.h"

namespace web {

const char kSsid[] = "DIGIWAVE";

namespace {

WebServer server(80);
bool active = false;
char addr[32] = "";

// Home network: joined besides the access point when settings.home_ssid is set
char home_addr[16] = "";
uint32_t join_ms = 0;      // last attempt to join
bool rejoin = false;       // the home network was changed on the web page, join it in loop()
constexpr uint32_t kJoinMs = 20000;  // an attempt that has not joined by then has failed
// A failed attempt scans all channels and takes a phone on the access point
// with it, so it is only repeated while no phone is connected
constexpr uint32_t kRejoinMs = 60000;

const char kPage[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>DIGIWAVE</title>
<link rel="icon" href="/icon.png"><link rel="apple-touch-icon" href="/icon.png">
<style>
:root{color-scheme:dark}
body{margin:0;font:15px/1.35 system-ui,sans-serif;background:#030513;color:#d8dade}
header{position:sticky;top:0;background:#070a1e;padding:10px 12px;border-bottom:1px solid #202d7a}
#top{display:flex;gap:10px;align-items:baseline;flex-wrap:wrap}
#utc{font-size:20px;font-variant-numeric:tabular-nums}
#state{margin-top:4px;min-height:1.35em}
.tx{color:#ff1ed7}.dim{color:#8a8f99}
#bar{height:4px;background:#2c2f36;margin-top:8px;border-radius:2px;overflow:hidden}
#bar div{height:100%;background:#00f5f8;width:0}
.btns{display:flex;gap:8px;margin-top:10px;flex-wrap:wrap}
button{font:inherit;padding:9px 14px;border-radius:8px;border:1px solid #3a3e47;background:#23262d;color:inherit}
button.on{background:#6a1560;border-color:#ff1ed7}
button.sel{background:#202d7a;border-color:#00f5f8}
#modes button{padding:6px 10px}
pre{white-space:pre-wrap;word-break:break-word;font:14px/1.4 ui-monospace,monospace;background:#16181c;border-radius:8px;padding:10px;min-height:8em;margin:0}
.hide{display:none}
main{padding:0 12px 24px}
h2{font-size:14px;color:#8a8f99;margin:18px 0 6px;font-weight:600}
table{width:100%;border-collapse:collapse;font-variant-numeric:tabular-nums}
td{padding:7px 4px;border-bottom:1px solid #22252b;white-space:nowrap}
td.m{white-space:normal;width:100%}
tr.cq td.m{color:#00f5f8}tr.me td.m{color:#ff1ed7}tr.own td{color:#ffd23c;background:#2a2408}tr.w2 td.m{color:#5c5c5c}tr.w1 td.m{color:#7fa0b8}
td.n{text-align:right;color:#8a8f99}
label{display:block;margin:8px 0 2px;color:#8a8f99;font-size:13px}
input[type=text],input[type=number],select{font:inherit;width:100%;box-sizing:border-box;padding:8px;border-radius:8px;border:1px solid #3a3e47;background:#16181c;color:inherit}
.row{display:flex;gap:10px}.row>div{flex:1}
a{color:#00f5f8}
#msg{color:#e0a020;min-height:1.35em;margin-top:6px}
</style></head><body>
<header>
<div id="top"><span id="utc">--:--:--</span><span id="src" class="dim"></span><span id="me" class="dim"></span></div>
<div id="state"></div>
<div id="bar"><div></div></div>
<div class="btns" id="modes"></div>
<div class="btns">
<button id="btx" class="slot" onclick="cmd(S.txen?'txoff':'txon')">TX off</button>
<button class="slot" onclick="cmd('cq')">Call CQ</button>
<button onclick="cmd('halt')">Halt</button>
</div>
<div id="msg"></div>
</header>
<main>
<section id="pslot">
<h2>Decodes &ndash; tap a station to call it</h2>
<table id="list"></table>
</section>
<section id="ptext">
<h2>Received text</h2>
<pre id="text"></pre>
<form id="sendform" onsubmit="sendText(event)"><div class="row"><div><input type="text" id="tosend" maxlength="78" autocapitalize="characters" placeholder="Text to send"></div>
<div style="flex:0"><button type="submit">Send</button></div></div></form>
</section>
<section id="pfax">
<h2 id="faxhead">Pictures on the SD card</h2>
<div id="faxlist" class="dim">none yet</div>
<canvas id="faxcv" class="hide" style="width:100%;margin-top:8px;image-rendering:pixelated"></canvas>
</section>
<section id="psched">
<h2><span id="schedname">WEFAX</span> schedule <span id="schedsrc" class="dim"></span></h2>
<p class="dim">One chart per line: start in UTC, then what it is, for example <code>22:00 Surface analysis</code>.
Lines with # are notes; <code>Station:</code> and <code>Frequencies:</code> name the sender.</p>
<textarea id="sched" rows="12" spellcheck="false" style="width:100%;box-sizing:border-box;font:13px/1.4 ui-monospace,monospace;background:#16181c;color:inherit;border:1px solid #3a3e47;border-radius:8px;padding:8px"></textarea>
<div class="btns"><button onclick="saveSched()">Save schedule</button><button onclick="loadSched()">Reload</button>
<button onclick="if(confirm('Use the built-in DWD plan again?'))post('/api/schedule',{p:schedPlan,reset:1}).then(loadSched)">Built-in plan</button></div>
</section>
<h2>Station</h2>
<form onsubmit="save(event)">
<div class="row"><div><label>Callsign</label><input type="text" id="call" maxlength="13" autocapitalize="characters"></div>
<div><label>Locator</label><input type="text" id="grid" maxlength="4" autocapitalize="characters"></div></div>
<label><input type="checkbox" id="gridauto"> Locator from GPS</label>
<div class="row"><div><label>Dial frequency (kHz)</label><input type="number" id="dial" min="0" max="450000" step="0.1"></div>
<div><label>TX audio (Hz)</label><input type="number" id="txhz" min="300" max="2800" step="50"></div></div>
<div class="btns"><button type="submit">Save</button></div>
</form>
<h2>Logbook</h2>
<p><span id="qsos">0</span> QSOs &middot; <a href="/log.adi" download="digiwave.adi">Download ADIF file</a></p>
<h2 id="home">Home network</h2>
<p>Joined besides the access point, so that CBScope on a computer at home finds the pager as digiwave.local and takes over the logbook.</p>
<p><b id="homestate"></b></p>
<form onsubmit="saveHome(event)">
<label>Network</label>
<select id="nets" onchange="if(this.value)$('homessid').value=this.value"><option value="">Searching for networks&hellip;</option></select>
<div class="row"><div><label>Network name</label><input type="text" id="homessid" maxlength="32"></div>
<div><label>Password</label><input type="text" id="homepass" maxlength="63" placeholder="unchanged" autocapitalize="off" autocorrect="off" spellcheck="false"></div></div>
<p class="dim">The password can be copied in the Wi-Fi settings of the phone and pasted here.</p>
<div class="btns"><button type="button" onclick="scan()">Search again</button><button type="submit">Save</button><button type="button" onclick="forgetHome()">Forget</button></div>
</form>
</main>
<script>
let S={},filled=false;
const $=id=>document.getElementById(id);
function note(t){$('msg').textContent=t;setTimeout(()=>{if($('msg').textContent==t)$('msg').textContent=''},4000)}
async function post(url,data){
  const r=await fetch(url,{method:'POST',body:new URLSearchParams(data)});
  const t=await r.text();note(t);poll();
}
function cmd(c){post('/api/cmd',{c})}
function setMode(m){faxAt=0;$('faxcv').className='hide';post('/api/mode',{m})}
function sendText(e){e.preventDefault();const t=$('tosend').value;if(t){post('/api/send',{t});$('tosend').value=''}}
let faxAt=0;
const picDir=()=>S&&S.mode==6?'sstv':'wefax';
async function loadFax(){
  try{const l=await (await fetch('/api/fax?d='+picDir())).json();const d=$('faxlist');
    $('faxhead').textContent=(picDir()=='sstv'?'SSTV':'WEFAX')+' pictures on the SD card';
    if(!l.length){d.textContent='none yet';return}
    d.textContent='';d.className='';
    for(const n of l.sort().reverse()){const p=document.createElement('p');
      const v=document.createElement('button');v.textContent=n;v.onclick=()=>showFax(n);p.appendChild(v);
      const a=document.createElement('a');a.href='/fax?d='+picDir()+'&f='+encodeURIComponent(n);a.textContent=' download ';a.download=n;p.appendChild(a);
      const x=document.createElement('button');x.textContent='Delete';x.onclick=async()=>{if(confirm('Delete '+n+'?')){await post('/api/faxdelete',{f:n,d:picDir()});$('faxcv').className='hide';loadFax()}};p.appendChild(x);
      d.appendChild(p)}
  }catch(e){}
}
// The picture is drawn here from its bytes, so that it shows whatever the browser thinks of BMP files
async function showFax(n){
  const b=new Uint8Array(await (await fetch('/fax?d='+picDir()+'&f='+encodeURIComponent(n))).arrayBuffer());
  const dv=new DataView(b.buffer),off=dv.getUint32(10,true),w=dv.getInt32(18,true),bpp=dv.getUint16(28,true);
  const h=Math.floor((b.length-off)/(w*bpp/8));if(!(w>0&&h>0))return;
  const c=$('faxcv');c.width=w;c.height=h;const g=c.getContext('2d'),im=g.createImageData(w,h);
  if(bpp==24){ // colour, rows from the bottom up, blue first
    for(let y=0;y<h;y++)for(let x=0;x<w;x++){const s=off+((h-1-y)*w+x)*3,t=4*(y*w+x);im.data[t]=b[s+2];im.data[t+1]=b[s+1];im.data[t+2]=b[s];im.data[t+3]=255}
  }else for(let i=0;i<w*h;i++){const v=b[off+i];im.data[4*i]=im.data[4*i+1]=im.data[4*i+2]=v;im.data[4*i+3]=255}
  g.putImageData(im,0,0);c.className='';c.scrollIntoView()
}
let schedPlan='';
async function loadSched(){try{const r=await fetch('/api/schedule?p='+schedPlan);$('sched').value=await r.text();
  $('schedname').textContent=schedPlan.toUpperCase();
  $('schedsrc').textContent=r.headers.get('X-Source')=='card'?'(from the SD card)':'(built in)'}catch(e){}}
function saveSched(){post('/api/schedule',{p:schedPlan,text:$('sched').value}).then(loadSched)}
function save(e){e.preventDefault();post('/api/settings',{call:$('call').value,grid:$('grid').value,
  gridauto:$('gridauto').checked?1:0,dial:$('dial').value,txhz:$('txhz').value})}
function saveHome(e){e.preventDefault();post('/api/home',{ssid:$('homessid').value,pass:$('homepass').value});$('homepass').value=''}
function forgetHome(){$('homessid').value='';$('homepass').value='';post('/api/home',{ssid:'',pass:''})}
// The pager scans in the background; ask again until the list is there
async function scan(again){
  try{const r=await (await fetch('/api/scan'+(again?'':'?new=1'))).json();
    if(r.scanning){setTimeout(()=>scan(true),1500);return}
    const s=$('nets');s.textContent='';
    const o=document.createElement('option');o.value='';o.textContent=r.nets.length?'Choose a network ('+r.nets.length+' found)':'No network found';s.appendChild(o);
    for(const n of r.nets){const e=document.createElement('option');e.value=n.s;e.textContent=n.s+(n.o?'  (open)':'')+'  '+n.r+' dBm';
      if(n.s==S.homessid)e.selected=true;s.appendChild(e)}
  }catch(x){setTimeout(()=>scan(true),3000)}
}
scan();
function answer(h){if(confirm('Call '+h.de+'?'))post('/api/answer',{slot:h.slot,f:h.f})}
function render(){
  const ms=$('modes');
  if(!ms.children.length)S.modes.forEach((n,i)=>{const b=document.createElement('button');b.textContent=n;b.onclick=()=>setMode(i);ms.appendChild(b)});
  [...ms.children].forEach((b,i)=>b.className=i==S.mode?'sel':'');
  const slot=S.mode<2,text=S.mode>=2&&S.mode<=4,fax=S.mode>=5;
  $('pslot').className=slot?'':'hide';$('ptext').className=text?'':'hide';$('pfax').className=fax?'':'hide';
  document.querySelectorAll('.slot').forEach(b=>b.style.display=slot?'':'none');
  $('sendform').className=S.cansend?'':'hide';
  if(text)$('text').textContent=S.text;
  if(fax&&Date.now()-faxAt>10000){faxAt=Date.now();loadFax()}
  // The WEFAX charts and the RTTY programme each have their plan
  const plan=S.mode==5?'wefax':(S.mode==3?'rtty':'');
  $('psched').className=plan?'':'hide';
  if(plan&&plan!=schedPlan){schedPlan=plan;loadSched()}
  $('utc').textContent=S.utc;$('src').textContent=S.src;
  $('me').textContent=(S.call||'no callsign')+' '+(S.grid||'');
  const st=$('state');st.className=S.tx?'tx':'';
  st.textContent=S.modes[S.mode]+(S.tx?'  TX  '+(slot?S.txtext:''):'  RX  '+(slot?S.qso+(S.dx?' '+S.dx:''):S.info));
  document.querySelector('#bar div').style.width=S.slot+'%';
  const b=$('btx');b.textContent=S.txen?'TX ON':'TX off';b.className=S.txen?'on':'';
  $('qsos').textContent=S.qsos;
  $('homestate').textContent={joined:'Joined '+S.homessid+', address '+S.home,joining:'Joining '+S.homessid+'\u2026',
    notfound:S.homessid+' not found - is it in reach?',failed:'Could not join '+S.homessid+' - password wrong?'}[S.homestate]||'No home network set';
  if(!filled){filled=true;$('call').value=S.call;$('grid').value=S.grid;$('gridauto').checked=S.gridauto;
    $('dial').value=S.dial;$('txhz').value=S.txhz;$('homessid').value=S.homessid}
  const tb=$('list');tb.textContent='';
  for(const h of S.hist.slice().reverse()){
    const tr=document.createElement('tr');tr.className=h.own?'own':(h.me?'me':(h.w?'w'+h.w:(h.cq?'cq':'')));
    for(const [c,v] of [['',h.t],['n',h.own?'TX':(h.snr>=0?'+':'')+h.snr],['n',h.f],['m',h.m],['n',(h.g+(h.c?' ('+h.c+')':'')).trim()]]){
      const td=document.createElement('td');td.className=c;td.textContent=v;tr.appendChild(td)}
    if(h.de&&!h.own)tr.onclick=()=>answer(h);
    tb.appendChild(tr)}
}
// Without GPS the device takes the phone's time. Like NTP: the difference is measured
// a few times, and the measurement with the shortest round trip is the most exact.
let synced=0;
async function syncTime(){
  synced=Date.now();let best=null;
  for(let i=0;i<5;i++){try{const t0=Date.now();const d=(await (await fetch('/api/time',{cache:'no-store'})).json()).t;const t1=Date.now();
    if(!best||t1-t0<best.rtt)best={rtt:t1-t0,adj:Math.round((t0+t1)/2-d)}}catch(e){}}
  if(best&&best.rtt<400)post('/api/time',{adj:best.adj,rtt:best.rtt})
}
async function poll(){try{S=await (await fetch('/api/state')).json();render();
  if(S.src!='GPS'&&S.src!='GPS+'&&Date.now()-synced>600000)syncTime()}catch(e){}}
setInterval(poll,2000);poll();
</script></body></html>
)HTML";

void appendJsonString(String &out, const char *s)
{
    out += '"';
    for (; *s; ++s) {
        if (*s == '"' || *s == '\\') {
            out += '\\';
        }
        if (*s == '\n') {
            out += "\\n";
        } else if ((unsigned char)*s >= 0x20) {
            out += *s;
        }
    }
    out += '"';
}

void handleState()
{
    static engine::Entry hist[engine::kHistory];
    engine::Status st = engine::status();
    int n = engine::history(hist, engine::kHistory);
    int64_t now = timebase::utcMs();
    uint32_t secs = (uint32_t)(now / 1000);

    int slot_ms = st.mode == engine::Mode::FT4 ? 7500 : 15000;

    String out;
    out.reserve(8000);
    char buf[160];
    snprintf(buf, sizeof(buf), "{\"utc\":\"%02u:%02u:%02u\",\"src\":\"%s\",\"slot\":%d,\"tx\":%s,\"txen\":%s,\"level\":%d,",
             (unsigned)(secs / 3600 % 24), (unsigned)(secs / 60 % 60), (unsigned)(secs % 60), timebase::sourceName(),
             (int)((now % slot_ms) * 100 / slot_ms), st.transmitting ? "true" : "false", st.tx_enabled ? "true" : "false", st.level);
    out += buf;
    snprintf(buf, sizeof(buf), "\"mode\":%d,\"cansend\":%s,\"modes\":[", (int)st.mode,
             engine::canSendText(st.mode) ? "true" : "false");
    out += buf;
    for (int i = 0; i < engine::kModes; ++i) {
        out += i ? ",\"" : "\"";
        out += engine::modeName((engine::Mode)i);
        out += '"';
    }
    out += "],\"text\":";
    appendJsonString(out, engine::isTextMode(st.mode) ? ui::recentText() : "");
    // One line of receiver state for the modes without a QSO sequence
    if (st.mode == engine::Mode::CW) {
        snprintf(buf, sizeof(buf), "%d WPM  %d Hz  %s", st.wpm, st.tune_hz, st.locked ? "signal" : "no signal");
    } else if (st.mode == engine::Mode::RTTY) {
        snprintf(buf, sizeof(buf), "%d/%d Hz%s  %s", settings.rtty_mark, settings.rtty_mark + settings.rtty_shift,
                 st.option ? " rev" : "", st.locked ? "signal" : "no signal");
    } else if (st.mode == engine::Mode::NAVTEX) {
        snprintf(buf, sizeof(buf), "centre %d Hz%s  %s", settings.navtex_center, st.option ? " rev" : "",
                 st.locked ? "in step" : "searching");
    } else {
        buf[0] = '\0';
    }
    out += ",\"info\":";
    appendJsonString(out, buf);
    out += ",\"txtext\":";
    appendJsonString(out, st.tx_text);
    out += ",\"qso\":";
    appendJsonString(out, st.qso_state);
    out += ",\"dx\":";
    appendJsonString(out, st.dx_call);
    out += ",\"call\":";
    appendJsonString(out, settings.call);
    out += ",\"grid\":";
    appendJsonString(out, settings.grid);
    out += ",\"homessid\":";
    appendJsonString(out, settings.home_ssid);
    out += ",\"home\":";
    appendJsonString(out, home_addr);
    out += ",\"homestate\":";
    appendJsonString(out, homeState());
    // The frequency of the mode in use, in kHz
    snprintf(buf, sizeof(buf), ",\"gridauto\":%s,\"dial\":%.1f,\"txhz\":%d,\"qsos\":%d,\"hist\":[",
             settings.grid_auto ? "true" : "false", settings.dialHz((int)engine::mode()) / 1000.0, settings.tx_hz,
             logbook::total());
    out += buf;
    const char *band_name = ft8::bandName(settings.dialHz((int)st.mode) / 1000);
    const char *band = band_name ? band_name : "";
    for (int i = 0; i < n; ++i) {
        const engine::Entry &e = hist[i];
        uint32_t t = e.slot_utc;
        snprintf(buf, sizeof(buf), "%s{\"t\":\"%02u%02u%02u\",\"slot\":%u,\"snr\":%d,\"f\":%d,\"cq\":%s,\"me\":%s,\"own\":%s,\"w\":%d,\"de\":",
                 i ? "," : "", (unsigned)(t / 3600 % 24), (unsigned)(t / 60 % 60), (unsigned)(t % 60), (unsigned)t, e.msg.snr,
                 (int)e.msg.freq, e.msg.is_cq ? "true" : "false", e.to_me ? "true" : "false", e.own ? "true" : "false",
                 e.own || e.msg.call_de[0] == '\0' ? 0 : logbook::worked(e.msg.call_de, band));
        out += buf;
        appendJsonString(out, e.msg.call_de);
        out += ",\"g\":";
        appendJsonString(out, e.grid);
        out += ",\"c\":";
        appendJsonString(out, e.country);
        out += ",\"m\":";
        appendJsonString(out, e.msg.text);
        out += '}';
    }
    out += "]}";
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", out);
}

void handleTimeGet()
{
    char buf[40];
    snprintf(buf, sizeof(buf), "{\"t\":%lld}", (long long)timebase::utcMs());
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", buf);
}

/// The page measures how far the clock is from the phone's and sends the difference
void handleTimePost()
{
    int64_t adjust = strtoll(server.arg("adj").c_str(), nullptr, 10);
    int rtt = server.arg("rtt").toInt();
    if (!timebase::setFromPhone(adjust)) {
        server.send(200, "text/plain", "Clock runs on GPS");
        return;
    }
    // With the exact time the manual correction for the RTC is no longer right
    if (settings.time_offset_ds != 0) {
        settings.time_offset_ds = 0;
        settings.save();
        timebase::setOffsetMs(0);
    }
    Serial.printf("[web] clock set from the phone, corrected by %lld ms (round trip %d ms)\n", (long long)adjust, rtt);
    char buf[64];
    snprintf(buf, sizeof(buf), "Clock set from the phone (%+.1f s)", adjust / 1000.0);
    server.send(200, "text/plain", buf);
}

bool stationConfigured()
{
    // CB messages carry no locator
    return settings.call[0] != '\0' && (settings.cb_mode || strlen(settings.grid) >= 4);
}

void handleCmd()
{
    String c = server.arg("c");
    if (c == "txoff") {
        engine::setTxEnabled(false);
        server.send(200, "text/plain", "TX off");
    } else if (c == "halt") {
        engine::halt();
        server.send(200, "text/plain", "Halted");
    } else if (!stationConfigured()) {
        server.send(200, "text/plain", "Set callsign and locator first");
    } else if (c == "txon") {
        if (!timebase::valid()) {
            server.send(200, "text/plain", "No time source - TX stays off");
            return;
        }
        engine::setTxEnabled(true);
        server.send(200, "text/plain", "TX enabled");
    } else if (c == "cq") {
        engine::startCq();
        server.send(200, "text/plain", engine::status().tx_enabled ? "Calling CQ" : "CQ armed - TX is off");
    } else {
        server.send(400, "text/plain", "Unknown command");
    }
}

void handleAnswer()
{
    static engine::Entry hist[engine::kHistory];
    if (!stationConfigured()) {
        server.send(200, "text/plain", "Set callsign and locator first");
        return;
    }
    uint32_t slot = (uint32_t)strtoul(server.arg("slot").c_str(), nullptr, 10);
    int freq = server.arg("f").toInt();
    int n = engine::history(hist, engine::kHistory);
    for (int i = 0; i < n; ++i) {
        const engine::Entry &e = hist[i];
        if (e.slot_utc == slot && (int)e.msg.freq == freq && e.msg.call_de[0] != '\0' && strcmp(e.msg.call_de, settings.call) != 0) {
            // As on the device: answering switches TX on
            if (!timebase::valid()) {
                server.send(200, "text/plain", "No time source - cannot transmit");
                return;
            }
            engine::answer(e);
            engine::setTxEnabled(true);
            server.send(200, "text/plain", String("Calling ") + e.msg.call_de + " - TX is on");
            return;
        }
    }
    server.send(200, "text/plain", "Message no longer in the list");
}

bool validCall(const String &s)
{
    if (s.length() < 3 || s.length() > 13) {
        return false;
    }
    for (char c : s) {
        if (!isalnum((unsigned char)c) && c != '/') {
            return false;
        }
    }
    return true;
}

bool validGrid(const String &s)
{
    return s.length() == 4 && s[0] >= 'A' && s[0] <= 'R' && s[1] >= 'A' && s[1] <= 'R' && isdigit((unsigned char)s[2]) &&
           isdigit((unsigned char)s[3]);
}

void handleSettings()
{
    String call = server.arg("call");
    String grid = server.arg("grid");
    call.trim();
    grid.trim();
    call.toUpperCase();
    grid.toUpperCase();
    bool grid_auto = server.arg("gridauto").toInt() != 0;
    if (!validCall(call)) {
        server.send(200, "text/plain", "Callsign not valid - nothing saved");
        return;
    }
    // With the locator taken from GPS the field may be empty until there is a fix
    if (!validGrid(grid) && !((grid_auto || settings.cb_mode) && grid.length() == 0)) {
        server.send(200, "text/plain", "Locator must look like JO62 - nothing saved");
        return;
    }
    strncpy(settings.call, call.c_str(), sizeof(settings.call) - 1);
    if (grid.length() == 4) {
        strncpy(settings.grid, grid.c_str(), sizeof(settings.grid) - 1);
    }
    settings.grid_auto = grid_auto;
    settings.dialHz((int)engine::mode()) = (uint32_t)(constrain(server.arg("dial").toDouble(), 0.0, 450000.0) * 1000 + 0.5);
    settings.tx_hz = constrain((int)server.arg("txhz").toInt(), 300, 2800);
    settings.save();
    engine::applySettings();
    server.send(200, "text/plain", "Saved");
}

void handleHome()
{
    String ssid = server.arg("ssid");
    String pass = server.arg("pass");
    ssid.trim();
    // An empty password keeps the one stored for the same network
    if (pass.length() > 0 || ssid != settings.home_ssid) {
        strncpy(settings.home_pass, pass.c_str(), sizeof(settings.home_pass) - 1);
        settings.home_pass[sizeof(settings.home_pass) - 1] = '\0';
    }
    strncpy(settings.home_ssid, ssid.c_str(), sizeof(settings.home_ssid) - 1);
    settings.home_ssid[sizeof(settings.home_ssid) - 1] = '\0';
    settings.save();
    server.send(200, "text/plain", ssid.length() ? "Saved - joining, the phone may drop out for a moment" : "Home network removed");
    // Joining here would hold up the answer
    rejoin = true;
}

/// Networks in reach, for choosing the home network instead of typing its name.
/// The scan runs in the background: the first call starts it, later ones fetch the result.
void handleScan()
{
    server.sendHeader("Cache-Control", "no-store");
    int n = WiFi.scanComplete();
    if (n != WIFI_SCAN_RUNNING && (n < 0 || server.hasArg("new"))) {
        // new=1: a result left over from earlier may be old
        WiFi.scanDelete();
        WiFi.scanNetworks(true);
        n = WIFI_SCAN_RUNNING;
    }
    if (n == WIFI_SCAN_RUNNING) {
        server.send(200, "application/json", "{\"scanning\":true}");
        return;
    }
    // The strongest first, each name once (a mesh or repeater sends it several times)
    int order[32];
    int count = 0;
    for (int i = 0; i < n && count < 32; ++i) {
        if (WiFi.SSID(i).length() == 0) {
            continue;  // hidden network
        }
        bool seen = false;
        for (int j = 0; j < count && !seen; ++j) {
            seen = WiFi.SSID(order[j]) == WiFi.SSID(i);
        }
        if (!seen) {
            order[count++] = i;
        }
    }
    for (int a = 1; a < count; ++a) {
        for (int b = a; b > 0 && WiFi.RSSI(order[b]) > WiFi.RSSI(order[b - 1]); --b) {
            int t = order[b];
            order[b] = order[b - 1];
            order[b - 1] = t;
        }
    }
    String out = "{\"nets\":[";
    for (int k = 0; k < count; ++k) {
        int i = order[k];
        out += k ? ",{\"s\":" : "{\"s\":";
        appendJsonString(out, WiFi.SSID(i).c_str());
        out += ",\"r\":";
        out += WiFi.RSSI(i);
        out += WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? ",\"o\":true}" : ",\"o\":false}";
    }
    out += "]}";
    WiFi.scanDelete();
    server.send(200, "application/json", out);
}

void handleMode()
{
    int m = server.arg("m").toInt();
    if (m < 0 || m >= engine::kModes) {
        server.send(400, "text/plain", "Unknown mode");
        return;
    }
    ui::selectMode(m);
    server.send(200, "text/plain", engine::modeName((engine::Mode)m));
}

void handleSend()
{
    String t = server.arg("t");
    t.trim();
    server.send(200, "text/plain", ui::submitText(t.c_str()) ? "Sending" : "This mode cannot send text");
}

/// The pictures of WEFAX, or with d=sstv those of SSTV.
const char *picDir()
{
    return server.arg("d") == "sstv" ? ui::kSstvDir : ui::kFaxDir;
}

void handleFaxList()
{
    String out = "[";
    instance.lockSPI();
    if (SD.cardType() != CARD_NONE) {
        File dir = SD.open(picDir());
        if (dir && dir.isDirectory()) {
            for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
                if (!f.isDirectory() && out.length() < 4000) {
                    if (out.length() > 1) {
                        out += ',';
                    }
                    appendJsonString(out, f.name());
                }
                f.close();
            }
        }
    }
    instance.unlockSPI();
    out += "]";
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", out);
}

void handleFax()
{
    String name = server.arg("f");
    // Only plain file names from the picture directory
    if (name.length() == 0 || name.indexOf('/') >= 0 || name.indexOf("..") >= 0) {
        server.send(400, "text/plain", "Bad file name");
        return;
    }
    String path = String(picDir()) + "/" + name;
    instance.lockSPI();
    File f;
    if (SD.cardType() != CARD_NONE && SD.exists(path)) {
        f = SD.open(path, FILE_READ);
    }
    if (f) {
        server.streamFile(f, "image/bmp");
        f.close();
        instance.unlockSPI();
        return;
    }
    instance.unlockSPI();
    server.send(404, "text/plain", "No such picture");
}

void handleFaxDelete()
{
    String name = server.arg("f");
    if (name.length() == 0 || name.indexOf('/') >= 0 || name.indexOf("..") >= 0) {
        server.send(400, "text/plain", "Bad file name");
        return;
    }
    String path = String(picDir()) + "/" + name;
    instance.lockSPI();
    bool ok = SD.cardType() != CARD_NONE && SD.remove(path);
    instance.unlockSPI();
    server.send(200, "text/plain", ok ? "Deleted" : "No such picture");
}

void handleScheduleGet()
{
    schedule::Plan p;
    if (!schedule::fromId(server.arg("p").c_str(), &p)) {
        server.send(404, "text/plain", "No such plan");
        return;
    }
    server.sendHeader("X-Source", schedule::fromCard(p) ? "card" : "builtin");
    server.send(200, "text/plain; charset=utf-8", schedule::text(p));
}

void handleSchedulePost()
{
    schedule::Plan p;
    if (!schedule::fromId(server.arg("p").c_str(), &p)) {
        server.send(404, "text/plain", "No such plan");
        return;
    }
    if (server.hasArg("reset")) {
        server.send(200, "text/plain", schedule::restoreDefault(p));
        return;
    }
    bool ok = false;
    const char *msg = schedule::save(p, server.arg("text"), &ok);
    server.send(ok ? 200 : 400, "text/plain", msg);
}

void handleLog()
{
    // The card shares the SPI bus with the display, which is drawn from this same thread
    instance.lockSPI();
    File f;
    if (SD.cardType() != CARD_NONE && SD.exists(logbook::kPath)) {
        f = SD.open(logbook::kPath, FILE_READ);
    }
    if (f) {
        server.sendHeader("Content-Disposition", "attachment; filename=\"digiwave.adi\"");
        server.streamFile(f, "text/plain");
        f.close();
        instance.unlockSPI();
        return;
    }
    instance.unlockSPI();
    server.send(404, "text/plain", "No logbook on the SD card yet");
}

/// Size of the logbook file, so a computer polling for new QSOs fetches the file only when it grew.
void handleLogInfo()
{
    instance.lockSPI();
    long size = -1;
    if (SD.cardType() != CARD_NONE && SD.exists(logbook::kPath)) {
        File f = SD.open(logbook::kPath, FILE_READ);
        if (f) {
            size = (long)f.size();
            f.close();
        }
    }
    instance.unlockSPI();
    String out = "{\"device\":\"DIGIWAVE\",\"call\":";
    appendJsonString(out, settings.call);
    char buf[48];
    snprintf(buf, sizeof(buf), ",\"qsos\":%d,\"size\":%ld}", logbook::total(), size);
    out += buf;
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", out);
}

void joinHome()
{
    WiFi.begin(settings.home_ssid, settings.home_pass);
    join_ms = millis();
}

void start()
{
    // The driver must not keep its settings in flash: the settings area is
    // shared with the other firmwares on the pager and may be full
    WiFi.persistent(false);
    bool home = settings.home_ssid[0] != '\0';
    WiFi.mode(home ? WIFI_AP_STA : WIFI_AP);
    if (!WiFi.softAP(kSsid, password())) {
        Serial.println("[web] access point did not start");
        WiFi.mode(WIFI_OFF);
        return;
    }
    snprintf(addr, sizeof(addr), "http://%s", WiFi.softAPIP().toString().c_str());
    server.on("/", HTTP_GET, []() { server.send_P(200, "text/html", kPage); });
    server.on("/icon.png", HTTP_GET, []() {
        server.sendHeader("Cache-Control", "max-age=86400");
        server.send_P(200, "image/png", (const char *)web_icon_png, web_icon_png_size);
    });
    server.on("/api/state", HTTP_GET, handleState);
    server.on("/api/time", HTTP_GET, handleTimeGet);
    server.on("/api/time", HTTP_POST, handleTimePost);
    server.on("/api/cmd", HTTP_POST, handleCmd);
    server.on("/api/answer", HTTP_POST, handleAnswer);
    server.on("/api/settings", HTTP_POST, handleSettings);
    server.on("/log.adi", HTTP_GET, handleLog);
    server.on("/api/log", HTTP_GET, handleLogInfo);
    server.on("/api/home", HTTP_POST, handleHome);
    server.on("/api/scan", HTTP_GET, handleScan);
    server.on("/api/schedule", HTTP_GET, handleScheduleGet);
    server.on("/api/schedule", HTTP_POST, handleSchedulePost);
    server.on("/api/mode", HTTP_POST, handleMode);
    server.on("/api/send", HTTP_POST, handleSend);
    server.on("/api/fax", HTTP_GET, handleFaxList);
    server.on("/fax", HTTP_GET, handleFax);
    server.on("/api/faxdelete", HTTP_POST, handleFaxDelete);
    server.begin();
    active = true;
    Serial.printf("[web] access point %s, %s\n", kSsid, addr);
    if (home) {
        // Rejoining is done in loop(), where it can wait for the phone to leave
        WiFi.setAutoReconnect(false);
        joinHome();
    }
    if (MDNS.begin("digiwave")) {
        MDNS.addService("http", "tcp", 80);
    }
}

void stop()
{
    MDNS.end();
    server.stop();
    WiFi.disconnect(true);
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    active = false;
    addr[0] = '\0';
    home_addr[0] = '\0';
}

}  // namespace

void apply()
{
    if (settings.wifi_on && !active) {
        start();
    } else if (!settings.wifi_on && active) {
        stop();
    } else if (settings.wifi_on && active) {
        // The password may have changed
        stop();
        start();
    }
}

void loop()
{
    if (!active) {
        return;
    }
    server.handleClient();
    if (rejoin) {
        // The access point stays up; only the connection to the home network changes
        rejoin = false;
        home_addr[0] = '\0';
        WiFi.disconnect();
        if (settings.home_ssid[0] == '\0') {
            WiFi.mode(WIFI_AP);
            return;
        }
        WiFi.mode(WIFI_AP_STA);
        WiFi.setAutoReconnect(false);
        joinHome();
        return;
    }
    if (settings.home_ssid[0] == '\0') {
        return;
    }
    if (WiFi.status() == WL_CONNECTED) {
        if (home_addr[0] == '\0') {
            strncpy(home_addr, WiFi.localIP().toString().c_str(), sizeof(home_addr) - 1);
            Serial.printf("[web] joined %s as %s\n", settings.home_ssid, home_addr);
        }
    } else {
        home_addr[0] = '\0';
        if (millis() - join_ms >= kRejoinMs && WiFi.softAPgetStationNum() == 0) {
            joinHome();
        }
    }
}

bool running()
{
    return active;
}

int clients()
{
    return active ? WiFi.softAPgetStationNum() : 0;
}

void shutdown()
{
    if (active) {
        stop();
    }
}

const char *address()
{
    return addr;
}

void printDiagnostics()
{
    Serial.printf("[web] %s, mode %d, access point %s channel %d, %d phone(s)\n", active ? "on" : "off", (int)WiFi.getMode(),
                  WiFi.softAPIP().toString().c_str(), (int)WiFi.channel(), active ? WiFi.softAPgetStationNum() : 0);
    Serial.printf("[web] home \"%s\": %s, status %d, address %s; internal free %u (lowest %u), largest block %u\n", settings.home_ssid,
                  homeState(), (int)WiFi.status(), home_addr, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

const char *homeAddress()
{
    return home_addr;
}

const char *homeState()
{
    if (!active || settings.home_ssid[0] == '\0') {
        return "";
    }
    if (home_addr[0] != '\0') {
        return "joined";
    }
    if (millis() - join_ms < kJoinMs) {
        return "joining";
    }
    return WiFi.status() == WL_NO_SSID_AVAIL ? "notfound" : "failed";
}

const char *password()
{
    // WPA2 needs at least 8 characters; a shorter password would open the network
    return strlen(settings.wifi_pass) >= 8 ? settings.wifi_pass : "digiwave";
}

}  // namespace web
