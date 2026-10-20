/*
 * Defuse to Wake - the device's web page: ONE self-contained document (HTML + CSS + JS inline, system
 * fonts, no external asset, no CDN), served from flash by web_task.cpp at "/". Budget: 20 KB
 * (tools/test_page.cpp enforces it). It polls /api/status every 500 ms (fetch, 2 s timeout) and
 * shows a "Connection lost" banner when that fails.
 * Every piece of device text (LLM puzzles, module codes, venues) is put in with textContent, never
 * as markup: the strings come from a language model and from NUSMods.
 */
#ifndef WEB_PAGE_H
#define WEB_PAGE_H

#ifndef PROGMEM
#define PROGMEM
#endif

#define WEB_PAGE_MAX_BYTES 20480u

static const char WEB_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Defuse to Wake</title>
<style>
:root{--bg:#f3efe6;--card:#fffdf8;--ink:#1d1b16;--mute:#6f6a5d;--line:#ddd5c3;--acc:#c2410c;--ok:#15803d;--warn:#b45309;--bad:#b91c1c;--hot:#7e22ce;--tint:#f7f1e3}
@media(prefers-color-scheme:dark){:root{--bg:#15130f;--card:#1f1c16;--ink:#f1ece0;--mute:#a39c8a;--line:#383327;--acc:#fb923c;--ok:#4ade80;--warn:#fbbf24;--bad:#f87171;--hot:#c084fc;--tint:#27231b}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.45 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
main{max-width:880px;margin:0 auto;padding:0 16px 80px}
header{display:flex;align-items:baseline;justify-content:space-between;gap:12px;padding:18px 0 6px}
h1{font-size:20px;margin:0;letter-spacing:.04em;text-transform:uppercase}
h1 b{color:var(--acc)}
h2{font-size:12px;text-transform:uppercase;letter-spacing:.12em;color:var(--mute);margin:0 0 10px}
.sub{color:var(--mute);font-size:13px}
nav{display:flex;gap:4px;margin:10px 0 16px;border-bottom:2px solid var(--line)}
nav button{flex:1;background:none;border:0;padding:10px 8px;font:inherit;font-weight:600;color:var(--mute);cursor:pointer;border-bottom:3px solid transparent;margin-bottom:-2px}
nav button[aria-selected=true]{color:var(--ink);border-bottom-color:var(--acc)}
nav button:focus-visible,button:focus-visible,input:focus-visible{outline:3px solid var(--acc);outline-offset:2px}
.card{background:var(--card);border:1px solid var(--line);border-radius:6px;padding:14px 16px;margin-bottom:14px}
.card.hero{border-left:6px solid var(--acc)}
.grid{display:grid;gap:14px;grid-template-columns:repeat(auto-fit,minmax(280px,1fr))}
.big{font:700 44px/1.1 ui-monospace,Consolas,monospace;font-variant-numeric:tabular-nums}
.timer{font:700 76px/1 ui-monospace,Consolas,monospace;font-variant-numeric:tabular-nums;letter-spacing:.02em}
.chip{display:inline-block;padding:1px 9px;border-radius:99px;font-size:12px;font-weight:700;border:1px solid var(--line);background:var(--tint)}
.chip.ok{color:var(--ok)}.chip.warn{color:var(--warn)}.chip.bad{color:var(--bad)}.chip.hot{color:var(--hot)}
.pip{display:inline-block;width:14px;height:14px;border-radius:50%;border:2px solid var(--bad);margin-right:5px}
.pip.on{background:var(--bad)}
.txt{background:var(--tint);border-left:3px solid var(--acc);padding:8px 10px;margin:8px 0;font:14px/1.4 ui-monospace,Consolas,monospace;word-break:break-word}
.hint{color:var(--warn);font-weight:600}
table{width:100%;border-collapse:collapse;font-size:13px}
th{text-align:left;font-size:11px;text-transform:uppercase;letter-spacing:.08em;color:var(--mute);padding:4px 6px}
td{padding:4px 6px;border-top:1px solid var(--line);font-variant-numeric:tabular-nums}
tr.warn td{background:rgba(180,83,9,.16);background:color-mix(in srgb,var(--warn) 16%,transparent)}
tr.bad td{background:rgba(185,28,28,.16);background:color-mix(in srgb,var(--bad) 16%,transparent);font-weight:600}
tr.fault td{background:rgba(126,34,206,.18);background:color-mix(in srgb,var(--hot) 18%,transparent);font-weight:600}
.tiles{display:grid;grid-template-columns:repeat(auto-fit,minmax(64px,1fr));gap:6px;margin-bottom:10px}
.tile{background:var(--tint);padding:6px 8px;border-radius:4px}
.tile b{display:block;font:700 18px ui-monospace,Consolas,monospace}
.tile span{font-size:11px;color:var(--mute);text-transform:uppercase}
.pz{border-top:1px solid var(--line);padding:8px 0}
.pz:first-of-type{border-top:0}
.row{display:flex;flex-wrap:wrap;gap:8px;align-items:center}
form label{display:block;font-weight:600;margin:10px 0 4px}
input[type=text],input[type=number]{width:100%;padding:9px 10px;font:inherit;color:var(--ink);background:var(--bg);border:1px solid var(--line);border-radius:4px}
.btn{font:inherit;font-weight:700;padding:10px 16px;border-radius:4px;border:2px solid var(--ink);background:var(--ink);color:var(--bg);cursor:pointer}
.btn.alt{background:none;color:var(--ink)}
.btn:disabled{opacity:.5;cursor:default}
.fi{display:grid;gap:4px;margin-bottom:12px}
.fi small{color:var(--mute)}
.sc{overflow-x:auto}
#lost{position:sticky;top:0;z-index:5;background:var(--bad);color:#fff;text-align:center;font-weight:700;padding:8px}
#toast{position:fixed;left:50%;bottom:18px;transform:translateX(-50%);background:var(--ink);color:var(--bg);padding:10px 16px;border-radius:6px;max-width:92vw;z-index:6}
.ok{color:var(--ok)}.err{color:var(--bad)}
[hidden]{display:none!important}
</style></head><body>
<div id="lost" hidden>Connection lost - retrying...</div>
<main>
<header><h1>Defuse <b>to</b> Wake</h1><span class="sub" id="clock">connecting...</span></header>
<nav role="tablist"><button role="tab" id="t-m" aria-selected="true">My mornings</button><button role="tab" id="t-e" aria-selected="false">Engineering</button></nav>

<section id="p-m">
<div class="card hero"><h2>Next alarm</h2><div class="big" id="al">--</div><div id="al2" class="sub"></div></div>
<div class="grid">
<div class="card"><h2>Today's classes</h2><div id="today" class="sub">--</div></div>
<div class="card"><h2>Tonight's bomb</h2><div id="diff">--</div><div class="sub" id="diffy"></div><button class="btn" id="ring" style="margin-top:12px">Ring now</button> <span class="sub" id="ringmsg"></span></div>
</div>
<div class="card"><h2>Briefing &amp; coach</h2><div id="brief">--</div><div id="coach" class="hint"></div></div>
<div class="card"><h2>Last 14 mornings <span class="chip" id="streak">streak 0</span></h2>
<div class="sc"><table><thead><tr><th>When</th><th>Time</th><th>Strikes</th><th>Snoozes</th><th>Boom</th><th>Class</th></tr></thead><tbody id="hist"></tbody></table></div></div>
<div class="card"><h2>Settings</h2>
<form id="form"><label for="lk">NUSMods share link</label><input type="text" id="lk" maxlength="512" autocomplete="off" spellcheck="false">
<label for="ld">Wake-up lead before the first class (minutes, 15-180)</label><input type="number" id="ld" min="15" max="180" step="1">
<p><button class="btn" type="submit">Save</button> <span id="fmsg"></span></p></form></div>
</section>

<section id="p-e" hidden>
<div class="grid">
<div class="card hero"><h2>Bomb <span class="chip" id="st">-</span></h2>
<div class="timer" id="tm">--:--</div><div id="pips"></div>
<div class="sub" id="mod" style="margin-top:6px"></div><div class="txt" id="pz">-</div><div class="hint" id="hint"></div><div class="sub" id="val"></div></div>
<div class="card"><h2>Link to the MCX <span class="chip" id="lk2">-</span></h2><div class="tiles" id="stats"></div>
<div class="sc"><table><thead><tr><th>Ago</th><th>Dir</th><th>Type</th><th>Seq</th><th>Len</th><th>Result</th></tr></thead><tbody id="log"></tbody></table></div></div>
</div>
<div class="card"><h2>AI puzzles</h2><div class="sub" id="aisum">--</div><div id="ai"></div></div>
<div class="grid">
<div class="card"><h2>Firebase &amp; system</h2><div id="sys" class="sub">--</div></div>
<div class="card"><h2>Fault injection</h2>
<div class="fi"><button class="btn alt" id="f1">Corrupt next packet</button><small>Flips the CRC of the next reliable frame to the MCX: it NACKs, the link retransmits, the bomb is unaffected. Try it, then press Ring now.</small></div>
<div class="fi"><button class="btn alt" id="f2">Simulate internet outage</button><small id="f2s">60 s: LLM, NUSMods and Firebase calls fail fast; Wi-Fi and this page stay up.</small></div>
<div class="fi"><button class="btn alt" id="f3">Regenerate puzzles</button><small>Runs the setter/checker pipeline now (with no internet it falls back to the built-in bank).</small></div></div>
</div>
</section>
</main>
<div id="toast" hidden></div>
<script>
"use strict";
const $=i=>document.getElementById(i);
const el=(t,c,x)=>{const e=document.createElement(t);if(c)e.className=c;if(x!=null)e.textContent=x;return e};
const put=(n,...k)=>{while(n.firstChild)n.removeChild(n.firstChild);k.forEach(c=>n.appendChild(c))};
const mmss=s=>Math.floor(s/60)+":"+String(s%60).padStart(2,"0");
const dur=s=>s>=3600?Math.floor(s/3600)+" h "+Math.floor(s%3600/60)+" min":s>=60?Math.floor(s/60)+" min":s+" s";
let S=null,fails=0,busy=false,dirty=false,toastT=0,cfgVer=-1;

function toast(m){const t=$("toast");t.textContent=m;t.hidden=false;clearTimeout(toastT);toastT=setTimeout(()=>{t.hidden=true},4000)}
function tab(m){$("p-m").hidden=!m;$("p-e").hidden=m;$("t-m").setAttribute("aria-selected",m);$("t-e").setAttribute("aria-selected",!m);location.hash=m?"":"e"}
$("t-m").onclick=()=>tab(true);$("t-e").onclick=()=>tab(false);
if(location.hash==="#e")tab(false);

async function api(url,body,ms){
  const ac=new AbortController(),t=setTimeout(()=>ac.abort(),ms||4000);
  try{const o={signal:ac.signal,cache:"no-store",headers:{"X-Requested-With":"defuse"}};
    if(body!==undefined){o.method="POST";o.headers["Content-Type"]="application/x-www-form-urlencoded";o.body=body}
    const r=await fetch(url,o);let j={};try{j=await r.json()}catch(e){}
    return{ok:r.ok,j:j}}
  finally{clearTimeout(t)}
}
async function poll(){
  if(busy)return;busy=true;
  try{const r=await api("/api/status",undefined,2000);if(!r.ok)throw 0;S=r.j;fails=0;render()}
  catch(e){fails++}
  busy=false;$("lost").hidden=fails<2;
}
async function act(url,body,after){
  try{const r=await api(url,body);toast(r.j.msg||r.j.error||(r.ok?"done":"failed"));if(after)after(r);poll()}
  catch(e){toast("No answer from the device")}
}
$("ring").onclick=()=>act("/api/ring","",r=>{$("ringmsg").textContent=r.j.msg||r.j.error||""});
$("f1").onclick=()=>{if(confirm("Corrupt the CRC of the next reliable frame sent to the MCX?\nThe MCX should NACK it and the link retransmit. The bomb is unaffected."))act("/api/fault/corrupt","")};
$("f2").onclick=()=>{if(confirm("Simulate an internet outage for 60 s?\nLLM, NUSMods and Firebase calls will fail; Wi-Fi stays up and the alarm is unaffected."))act("/api/fault/outage","")};
$("f3").onclick=()=>{if(confirm("Regenerate tonight's puzzles now?\nThis can take a few minutes and never runs during a bomb."))act("/api/puzzles","")};
$("lk").oninput=$("ld").oninput=()=>{dirty=true};
$("form").onsubmit=async e=>{
  e.preventDefault();const m=$("fmsg");m.className="";m.textContent="saving...";
  try{const b=new URLSearchParams({nusmods_link:$("lk").value.trim(),lead_min:$("ld").value}).toString();
    const r=await api("/api/settings",b);
    if(r.ok){dirty=false;m.className="ok";m.textContent="Saved: "+r.j.lead_min+" min lead; timetable refresh started"}
    else{m.className="err";m.textContent=r.j.error||"rejected"}
  }catch(x){m.className="err";m.textContent="No answer from the device"}
  poll();
};

function td(tr,t,c){const d=el("td",c,t);tr.appendChild(d);return d}
function chip(t,c){return el("span","chip "+(c||""),t)}
const STC={IDLE:"",ARMED:"warn",BOMB_ACTIVE:"bad",DEFUSED:"ok",EXPLODED:"hot"};

function renderMornings(s){
  const a=s.alarm;
  $("al").textContent=a.valid?a.local.slice(-5):"No class soon";
  $("al2").textContent=a.valid?(a.local.slice(0,14)+" - "+a.module+" "+a.type+" @ "+a.venue+" - rings in "+dur(a.in_s)+", "+s.settings.lead_min+" min before class"):"The MCX stays idle until a class is found in the next 7 days.";
  const t=s.today,tb=$("today");
  if(!t.classes.length)put(tb,el("div",0,(t.date||"Today")+": no classes"));
  else put(tb,...t.classes.map(c=>el("div",0,c.start+"  "+c.module+" "+c.type+"  "+c.venue)));
  $("diff").textContent=s.diff.modules+" modules, "+mmss(s.diff.time_s);
  $("diffy").textContent=s.diff.why;
  $("brief").textContent=s.brief.briefing;$("coach").textContent=s.brief.coach;
  $("streak").textContent="streak "+s.history.streak;
  put($("hist"),...s.history.records.map(r=>{const tr=el("tr");td(tr,r.when);td(tr,mmss(r.dur));td(tr,r.strikes);td(tr,r.snoozes);td(tr,r.expl);td(tr,r.module||"-");return tr}));
  if(!s.history.records.length)put($("hist"),(()=>{const tr=el("tr");td(tr,"No mornings recorded yet").colSpan=6;return tr})());
  if(s.settings.ver!==cfgVer)loadCfg(s.settings.ver);
}
async function loadCfg(v){
  cfgVer=v;
  try{const r=await api("/api/settings");if(!r.ok)throw 0;
    if(!dirty&&document.activeElement!==$("lk")&&document.activeElement!==$("ld")){$("lk").value=r.j.nusmods_link;$("ld").value=r.j.lead_min}
  }catch(e){cfgVer=-1}
}

function renderBomb(s){
  const g=s.game,v=g.value||{};
  const c=$("st");c.textContent=g.valid?g.state:"no MCX data";c.className="chip "+(STC[g.state]||"");
  $("tm").textContent=g.state==="BOMB_ACTIVE"?g.timer:"--:--";
  put($("pips"),...Array.from({length:g.max_strikes},(_,i)=>el("span","pip"+(i<g.strikes?" on":""))));
  $("mod").textContent=g.module_id?(g.module+" - module "+(g.idx+1)+" of "+(g.total||"?")+" - "+g.progress_pct+"% cleared"):"";
  $("pz").textContent=g.puzzle||"-";$("hint").textContent=g.hint||"";
  let d="";
  if(g.state!=="BOMB_ACTIVE")d="";
  else if(g.module==="DIAL")d="digit "+(v.digit+1)+" of 4, dial shows "+v.value;
  else if(g.module==="WIRES"){const cut=[];for(let i=0;i<8;i++)if(v.cut_mask>>i&1)cut.push(i+1);d="cursor on wire "+(v.cursor+1)+" of "+v.wires+", cut: "+(cut.join(",")||"none")}
  else if(g.module==="TRIPWIRE")d=v.cm+" cm (band "+v.min+"-"+v.max+"), hold "+v.hold_pct+"%";
  $("val").textContent=d;
}

function renderLink(s){
  const l=s.link,st=l.stats;
  const c=$("lk2");c.textContent=l.up?"UP":"DOWN";c.className="chip "+(l.up?"ok":"bad");
  put($("stats"),...[["tx",st.tx],["rx",st.rx],["retx",st.retx],["fail",st.fail],["crc",st.crc],["nack",st.nack],["dup",st.dup]].map(a=>{const t=el("div","tile");t.appendChild(el("b",0,a[1]));t.appendChild(el("span",0,a[0]));return t}));
  const cls=o=>o==="NACK"||o==="CRC_ERR"||o==="SEND_FAIL"?"bad":o==="RETX"?"warn":o.indexOf("fault")>=0?"fault":"";
  put($("log"),...l.log.map(e=>{const tr=el("tr",cls(e.out));td(tr,(e.ago/1000).toFixed(1)+" s");td(tr,e.dir);td(tr,e.type);td(tr,e.seq===255?"-":e.seq);td(tr,e.len);td(tr,e.out);return tr}));
  if(!l.log.length)put($("log"),(()=>{const tr=el("tr");td(tr,"Nothing yet: periodic frames are not listed").colSpan=6;return tr})());
}

function renderAi(s){
  const a=s.ai,n=s.net;if(!a)return;
  let h=a.gen?("Set "+a.gen+", "+a.ai_count+" of "+a.total+" AI-verified"+(a.generated?", made "+a.generated:"")):"Built-in bank puzzles";
  if(a.busy)h+=" - run in progress ("+a.progress+"/"+a.total+")";else if(a.queued)h+=" - run queued";
  if(a.last.valid)h+=" - last run: "+a.last.ai+" AI-verified, "+(a.last.installed?"new set installed":"none verified, bank fallback, previous set kept");
  if(n.outage_s>0)h+=" - SIMULATED OUTAGE "+n.outage_s+" s";
  $("aisum").textContent=h;
  put($("ai"),...a.puzzles.map(p=>{
    const d=el("div","pz"),r=el("div","row");
    r.appendChild(el("b",0,p.id));r.appendChild(chip(p.src,p.src==="AI"?"ok":"warn"));
    r.appendChild(chip(p.ok?"verified":(p.setter?"rejected":"unchecked"),p.ok?"ok":"bad"));
    if(p.active)r.appendChild(chip("ACTIVE","hot"));
    d.appendChild(r);d.appendChild(el("div","txt",p.text));
    d.appendChild(el("div","sub",(p.setter?"setter "+p.setter+" ("+p.s_ms+" ms) -> checker "+(p.checker||"-")+" ("+p.c_ms+" ms) said "+(p.said||"-")+(p.ok?" OK":" X")+", tries "+p.tries:"template, no model involved")+" | answer: "+(p.answer==="hidden"?"hidden while armed/active":p.answer+(p.wires?" of "+p.wires:""))));
    return d}));
}

function renderSys(s){
  const y=s.sys,k=y.stack;
  $("sys").textContent="Firebase pending: "+s.fb.pending+" | heap free "+y.heap+" (min "+y.heap_min+") | stack free link "+k.link+", gyro "+k.gyro+", display "+k.display+", net "+k.net+", web "+k.web+" bytes | up "+dur(s.up_s);
  const o=s.net.outage_s;
  $("f2s").textContent=o>0?("Outage active: "+o+" s left"):"60 s: LLM, NUSMods and Firebase calls fail fast; Wi-Fi and this page stay up.";
  $("f1").textContent=s.link.corrupt_armed?"Corrupt next packet (armed...)":"Corrupt next packet";
  $("clock").textContent=(s.net.time_valid?s.net.now_local:"clock not set")+" | "+(s.net.wifi?s.net.ip:"no Wi-Fi");
}

function render(){renderMornings(S);renderBomb(S);renderLink(S);renderAi(S);renderSys(S)}
setInterval(poll,500);poll();
</script></body></html>)HTML";

#endif /* WEB_PAGE_H */
