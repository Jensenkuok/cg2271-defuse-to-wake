/*
 * Defuse to Wake - the device's main web page: the settings page, ONE self-contained document
 * (HTML + CSS + JS inline, system fonts, no external asset, no CDN), served from flash by web_task.cpp
 * at "/". Budget: 20 KB (tools/test_page.cpp enforces it).
 * Cards, top to bottom: next alarm (big time, class line, live countdown, "Demo alarm" badge),
 * "This morning" (briefing + coach from /api/status), settings (NUSMods link, lead time as a typed
 * number kept in sync with a slider and -/+ buttons, difficulty, Save only when valid and changed),
 * the timetable (NUSMods-style week grid from GET /api/timetable, fetched on load, after a Save and
 * when settings.ver moves - never on every poll), and "Try it now" (demo morning alarm through
 * POST /api/demo_alarm, and Ring now, which asks for confirmation because it starts a bomb).
 * It polls /api/status every 2 s (fetch, 2 s timeout; a settings page does not need faster) and
 * shows a "Connection lost" banner when that fails. A poll never overwrites a field being edited.
 * It deliberately has no link to the engineering panel (web_eng_page.h, served at "/eng").
 * Every piece of device text (module codes, venues, briefing, coach, messages) is put in with
 * textContent, never as markup: the strings come from NUSMods and from a language model.
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
:root{--bg:#f6f6f4;--card:#fff;--ink:#1c1c1a;--mute:#6b6b64;--line:#e2e2dc;--tint:#f1f0ea;--acc:#c2410c;--on:#fff;--ok:#15803d;--bad:#b91c1c;--hp:80px}
@media(prefers-color-scheme:dark){:root{--bg:#141413;--card:#1d1d1b;--ink:#efefea;--mute:#a0a098;--line:#33332f;--tint:#262622;--acc:#fb923c;--on:#1a1208;--ok:#4ade80;--bad:#f87171}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);font:16px/1.45 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
main{max-width:760px;margin:0 auto;padding:10px 16px 48px}
h1{font-size:13px;letter-spacing:.14em;text-transform:uppercase;color:var(--mute);margin:8px 0 14px}
h2{font-size:13px;letter-spacing:.1em;text-transform:uppercase;color:var(--mute);margin:0 0 12px}
p{margin:0 0 8px}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:18px 16px;margin-bottom:14px}
.hero{border-left:6px solid var(--acc)}
.lbl,.mute{color:var(--mute);font-size:14px}
.time{font:700 clamp(48px,16vw,72px)/1.05 ui-monospace,Consolas,monospace;font-variant-numeric:tabular-nums;margin:4px 0 6px}
.cd{color:var(--acc);font-weight:700;font-variant-numeric:tabular-nums;min-height:24px}
.badge{margin-left:8px;padding:2px 10px;border-radius:99px;background:var(--acc);color:var(--on);font-size:12px;font-weight:700}
.q{border-left:3px solid var(--acc);background:var(--tint);padding:8px 12px;border-radius:0 8px 8px 0}
label{display:block;font-weight:600;font-size:14px;margin:18px 0 6px}
input[type=text],input[type=number],select{font:inherit;color:var(--ink);background:var(--bg);border:1px solid var(--line);border-radius:10px;min-height:44px;padding:8px 12px}
input[type=text],select{width:100%}
input[aria-invalid=true]{border-color:var(--bad)}
input[type=range]{width:100%;height:44px;margin:2px 0 0;accent-color:var(--acc)}
.num{display:flex;align-items:center;gap:8px}
.num input{width:92px;text-align:center}
.btn{font:inherit;font-weight:600;min-height:44px;padding:0 18px;border-radius:10px;border:1px solid var(--acc);background:none;color:var(--acc);cursor:pointer}
.btn.fill{background:var(--acc);color:var(--on)}
.btn.stp{width:44px;padding:0;font-size:22px;border-color:var(--line);color:var(--ink)}
.btn:disabled{opacity:.45;cursor:default}
:focus-visible{outline:3px solid var(--acc);outline-offset:2px}
.act{display:flex;align-items:center;flex-wrap:wrap;gap:12px;margin-top:20px}
.fe,.msg{font-size:14px;margin-top:4px}
.msg{min-height:20px}
.fe,.err{color:var(--bad)}.ok{color:var(--ok)}
.hint{color:var(--acc);font-size:14px;font-weight:600}
.hd{display:flex;justify-content:space-between;align-items:baseline;gap:8px}
.gw{overflow-x:auto;border:1px solid var(--line);border-radius:10px;margin-bottom:8px}
.dr{display:flex;border-top:1px solid var(--line)}
.dr:first-child{border-top:0}
.dl{flex:none;width:44px;position:sticky;left:0;z-index:2;background:var(--card);border-right:1px solid var(--line);display:flex;align-items:center;justify-content:center;font-size:12px;font-weight:700;color:var(--mute)}
.dt{position:relative;flex:1 0 auto;background-image:linear-gradient(90deg,var(--line) 1px,transparent 1px);background-size:var(--hp) 100%}
.hh{display:flex;flex:1 0 auto}
.hh span{flex:none;width:var(--hp);font-size:11px;color:var(--mute);padding:2px 0 2px 3px}
.blk{position:absolute;overflow:hidden;padding:3px 6px;border-radius:6px;font-size:11px;line-height:1.25;background:hsl(var(--h) 75% 88%);border-left:4px solid hsl(var(--h) 65% 42%);color:#1c1c1a}
.blk b,.blk span,.blk i{display:block;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.blk b{font-size:11px}
.blk.off{opacity:.5;border-left-style:dashed}
@media(prefers-color-scheme:dark){.blk{background:hsl(var(--h) 38% 24%);border-left-color:hsl(var(--h) 70% 65%);color:#efefea}}
hr{border:0;border-top:1px solid var(--line);margin:20px 0 4px}
#lost{position:sticky;top:0;z-index:5;background:var(--bad);color:#fff;text-align:center;font-size:14px;font-weight:600;padding:6px}
[hidden]{display:none!important}
</style></head><body>
<div id="lost" hidden>Connection lost - retrying...</div>
<main><h1>Defuse to Wake</h1>
<section class="card hero"><div class="lbl">Next alarm<span class="badge" id="bdg" hidden>Demo alarm</span></div>
<div class="time" id="al">--</div><div id="al2">&nbsp;</div><div class="cd" id="cd"></div></section>
<section class="card"><h2>This morning</h2>
<p id="bf" hidden></p><div class="q" id="qc" hidden><div class="lbl">Coach</div><div id="co"></div></div>
<p class="mute" id="bfe" hidden>Nothing yet. A briefing is written for each morning.</p></section>
<section class="card"><h2>Settings</h2>
<form id="form" novalidate><label for="lk" style="margin-top:0">NUSMods share link</label>
<input type="text" id="lk" maxlength="512" autocomplete="off" spellcheck="false" placeholder="Paste your NUSMods share link">
<label for="ln">Wake up before first class</label>
<div class="num"><button class="btn stp" type="button" id="lm" aria-label="1 minute less">&minus;</button>
<input type="number" id="ln" min="1" max="180" step="1" value="60" inputmode="numeric" aria-describedby="lne">
<span>min</span><button class="btn stp" type="button" id="lp" aria-label="1 minute more">+</button></div>
<input type="range" id="ld" min="1" max="180" step="1" value="60" aria-label="Lead time slider, 1 to 180 minutes">
<div class="fe" id="lne" role="alert"></div>
<label for="df">Difficulty</label>
<select id="df"><option value="auto">Auto</option><option value="easy">Easy</option><option value="normal">Normal</option><option value="hard">Hard</option></select>
<div class="mute" id="dfn">&nbsp;</div>
<div class="act"><button class="btn fill" id="sv" type="submit" disabled>Save</button><span class="hint" id="uns" hidden>Unsaved changes</span></div>
<div class="msg" id="fmsg" aria-live="polite"></div></form></section>
<section class="card"><div class="hd"><h2>Timetable</h2><span class="mute" id="ttw"></span></div>
<div class="gw" id="gw" role="region" tabindex="0" aria-label="Weekly timetable, scrolls sideways" hidden><div id="tg"></div></div>
<p class="mute" id="tte">Paste your NUSMods link and Save</p><div class="mute" id="ttf"></div></section>
<section class="card"><h2>Try it now</h2>
<label for="dm" style="margin-top:0">Demo morning alarm</label>
<div class="num" id="dmgo"><input type="number" id="dm" min="1" max="30" step="1" value="2" inputmode="numeric" aria-describedby="dme"><span>min from now</span><button class="btn fill" id="dms" type="button">Start</button></div>
<div class="num" id="dmrun" hidden><b id="dmat"></b><button class="btn" id="dmx" type="button">Cancel</button></div>
<div class="fe" id="dme" role="alert"></div><div class="msg" id="dmm" aria-live="polite"></div><div class="fe" id="dmw" aria-live="polite"></div>
<p class="mute">Rings exactly like a real morning alarm: the OLED counts down, then the bomb starts.</p>
<hr><div class="act"><button class="btn" id="ring" type="button">Ring now</button><span class="mute">Starts a bomb right away.</span></div>
<div class="msg" id="ringmsg" aria-live="polite"></div></section>
</main>
<script>
"use strict";
const $=i=>document.getElementById(i),tx=(e,t)=>{e.textContent=t;return e};
const el=(t,c,s)=>{const e=document.createElement(t);e.className=c;if(s!==undefined)e.textContent=s;return e};
const TY={LEC:"Lecture",LAB:"Lab",TUT:"Tutorial",SEM:"Seminar",REC:"Recitation",SEC:"Sectional"};
const DAYS=["Mon","Tue","Wed","Thu","Fri","Sat"],HUES=[12,35,145,175,205,250,290,335],HP=80,LH=50,FIELDS=["lk","ln","ld","df"];
let fails=0,busy=false,cfgBusy=false,saving=false,cfgVer=-1,ttVer=-1,base=null,alarmOn=false,alarmAt=0,week=0,lessons=[],fetched="";

async function api(url,body,ms){
  const ac=new AbortController(),t=setTimeout(()=>ac.abort(),ms||4000);
  try{const o={signal:ac.signal,cache:"no-store",headers:{"X-Requested-With":"defuse"}};
    if(body!==undefined){o.method="POST";o.headers["Content-Type"]="application/x-www-form-urlencoded";o.body=body}
    const r=await fetch(url,o);let j={};try{j=await r.json()}catch(e){}
    return{ok:r.ok,j:j}}
  finally{clearTimeout(t)}
}
function say(id,cls,txt){const m=$(id);m.className="msg "+cls;m.textContent=txt}
function numErr(v,lo,hi){
  if(v==="")return"Enter a number of minutes";
  if(!/^-?\d+$/.test(v))return"Whole minutes only";
  return(+v<lo||+v>hi)?"Use "+lo+" to "+hi+" minutes":"";
}
const leadErr=()=>numErr($("ln").value.trim(),1,180);

/* ---- settings form: Save only when valid and different from what the device stores ---- */
const cur=()=>({lk:$("lk").value.trim(),ld:$("ln").value.trim(),df:$("df").value});
function isDirty(){if(!base)return false;const c=cur();return c.lk!==base.lk||c.ld!==base.ld||c.df!==base.df}
function check(){
  const e=leadErr(),d=isDirty();
  tx($("lne"),e);$("ln").setAttribute("aria-invalid",e?"true":"false");
  $("sv").disabled=!!e||!d||saving;$("uns").hidden=!d;
}
function fill(b){$("lk").value=b.lk;$("ln").value=b.ld;$("ld").value=b.ld;$("df").value=b.df}
const norm=j=>({lk:j.nusmods_link,ld:String(j.lead_min),df:j.difficulty});
function bump(d){
  let n=parseInt($("ln").value,10);if(isNaN(n))n=+$("ld").value;
  n=Math.min(180,Math.max(1,n+d));$("ln").value=n;$("ld").value=n;check();
}
$("lk").oninput=check;$("df").onchange=check;
$("ln").oninput=()=>{if(!leadErr())$("ld").value=$("ln").value;check()};
$("ld").oninput=()=>{$("ln").value=$("ld").value;check()};
$("lm").onclick=()=>bump(-1);$("lp").onclick=()=>bump(1);

async function loadCfg(v){
  const f=document.activeElement;
  if(cfgBusy||isDirty()||(f&&FIELDS.includes(f.id)))return;   /* never overwrite an edit; retried on the next poll */
  cfgBusy=true;
  try{const r=await api("/api/settings");if(!r.ok)throw 0;
    if(!isDirty()){base=norm(r.j);fill(base);cfgVer=v;check()}
  }catch(e){}
  cfgBusy=false;
}
$("form").onsubmit=async e=>{
  e.preventDefault();if($("sv").disabled)return;
  saving=true;check();tx($("sv"),"Saving...");say("fmsg","","");
  try{const c=cur(),b=new URLSearchParams({nusmods_link:c.lk,lead_min:c.ld,difficulty:c.df}).toString();
    const r=await api("/api/settings",b);
    if(r.ok){base=norm(r.j);fill(base);say("fmsg","ok","Saved: "+r.j.lead_min+" min lead, difficulty "+r.j.difficulty);
      [0,5,12].forEach(s=>setTimeout(loadTT,s*1000))}   /* the device re-fetches the timetable a few seconds after a save */
    else say("fmsg","err",r.j.error||"Rejected by the device")
  }catch(x){say("fmsg","err","No answer from the device")}
  saving=false;tx($("sv"),"Save");check();poll();
};

/* ---- next alarm hero + live countdown (ticks every second between polls) ---- */
function tick(){
  let t="";
  if(alarmOn){
    const r=alarmAt?Math.max(0,Math.round((alarmAt-Date.now())/1000)):0;
    const d=Math.floor(r/86400),h=Math.floor(r%86400/3600),m=Math.floor(r%3600/60);
    t=r?"in "+[d&&d+"d",(d||h)&&h+"h",(d||h||m)&&m+"m",r%60+"s"].filter(Boolean).join(" "):"Due now";
  }
  tx($("cd"),t);
}
function renderAlarm(a){
  alarmOn=a.valid;alarmAt=(a.valid&&a.in_s>0)?Date.now()+a.in_s*1000:0;
  $("bdg").hidden=!a.demo;
  tx($("al"),a.valid?a.local.slice(-5):"No alarm set");
  tx($("al2"),a.valid?a.local.slice(0,3)+" · "+(a.demo?"Rehearsal of the morning alarm":a.module+" "+(TY[a.type]||a.type)+" at "+a.class_local.slice(-5)):"Nothing is scheduled yet");
  $("dmgo").hidden=a.demo;$("dmrun").hidden=!a.demo;
  tx($("dmat"),a.demo?"Demo alarm at "+a.local.slice(-5):"");
  tx($("dmw"),a.demo_msg||"");
  tick();
}
function render(s){
  const b=s.brief;
  renderAlarm(s.alarm);
  tx($("bf"),b.briefing);tx($("co"),b.coach);
  $("bf").hidden=!b.briefing;$("qc").hidden=!b.coach;$("bfe").hidden=!!(b.briefing||b.coach);
  tx($("dfn"),"Now: "+s.diff.why);
  if(s.settings.ver!==cfgVer)loadCfg(s.settings.ver);
  if(s.settings.ver!==ttVer){ttVer=s.settings.ver;loadTT()}
}
async function poll(){
  if(busy)return;busy=true;
  try{const r=await api("/api/status",undefined,2000);if(!r.ok)throw 0;fails=0;render(r.j)}
  catch(e){fails++}
  busy=false;$("lost").hidden=fails<2;
}

/* ---- timetable grid: day rows, hours across, blocks placed by time ---- */
const mins=s=>(+s.slice(0,2))*60+(+s.slice(3,5));
function hue(m){let x=0;for(const c of m)x=(x*31+c.charCodeAt(0))>>>0;return HUES[x%HUES.length]}
function weeksText(w){
  const o=[];let i=0;
  while(i<w.length){let j=i;while(j+1<w.length&&w[j+1]===w[j]+1)j++;o.push(j>i?w[i]+"-"+w[j]:""+w[i]);i=j+1}
  return"Wk "+o.join(",");
}
function block(l,lane,h0){
  const off=week>0&&!l.weeks.includes(week),b=el("div","blk"+(off?" off":""));
  b.style.left=(l.s-h0*60)*HP/60+"px";b.style.width=(l.e-l.s)*HP/60-2+"px";
  b.style.top=lane*LH+2+"px";b.style.height=LH-4+"px";b.style.setProperty("--h",hue(l.module));
  b.append(el("b","",l.module+" "+l.type),el("span","",l.venue));
  if(off)b.append(el("i","",weeksText(l.weeks)));
  b.title=l.module+" "+l.type+" "+l.start+"-"+l.end+" "+l.venue+(off?" ("+weeksText(l.weeks)+")":"");
  return b;
}
function drawTT(){
  const L=lessons.map(l=>Object.assign({s:mins(l.start),e:mins(l.end)},l)),g=el("div","");
  tx($("ttw"),week?"Week "+week:"Outside teaching weeks");
  tx($("ttf"),fetched?"Updated "+fetched:"Not fetched yet");
  tx($("tte"),"Paste your NUSMods link and Save");$("tte").hidden=L.length>0;$("gw").hidden=!L.length;$("tg").replaceChildren();
  if(!L.length)return;
  const h0=Math.min(...L.map(l=>Math.floor(l.s/60))),h1=Math.max(...L.map(l=>Math.ceil(l.e/60))),w=(h1-h0)*HP;
  const nd=L.some(l=>l.day===6)?6:5,hh=el("div","hh"),hr=el("div","dr");
  g.style.minWidth=44+w+"px";
  for(let h=h0;h<h1;h++)hh.append(el("span","",(h<10?"0":"")+h+":00"));
  hh.style.minWidth=w+"px";hr.append(el("div","dl"),hh);g.append(hr);
  for(let d=1;d<=nd;d++){
    const ends=[],t=el("div","dt"),row=el("div","dr");
    L.filter(l=>l.day===d).sort((a,b)=>a.s-b.s).forEach(l=>{
      let k=ends.findIndex(x=>x<=l.s);if(k<0){k=ends.length;ends.push(0)}
      ends[k]=l.e;t.append(block(l,k,h0))});
    t.style.minWidth=w+"px";t.style.height=Math.max(1,ends.length)*LH+"px";
    row.append(el("div","dl",DAYS[d-1]),t);g.append(row);
  }
  $("tg").append(g);
}
async function loadTT(){
  try{const r=await api("/api/timetable");if(!r.ok||!r.j.ok)throw 0;
    week=r.j.week;lessons=r.j.lessons;fetched=r.j.fetched_local;drawTT()}
  catch(e){if(!lessons.length)tx($("tte"),"Could not load the timetable");setTimeout(()=>{ttVer=-1},10000)}   /* retry later, not on every poll */
}

/* ---- demo morning alarm + Ring now ---- */
const dmErr=()=>numErr($("dm").value.trim(),1,30);
$("dm").oninput=()=>{const e=dmErr();tx($("dme"),e);$("dm").setAttribute("aria-invalid",e?"true":"false");$("dms").disabled=!!e};
async function demo(m){
  say("dmm","","working...");
  try{const r=await api("/api/demo_alarm","minutes="+encodeURIComponent(m));
    say("dmm",r.ok&&r.j.ok?"ok":"err",r.j.msg||r.j.error||"failed")}
  catch(x){say("dmm","err","No answer from the device")}
  poll();
}
$("dms").onclick=()=>{if(!dmErr())demo($("dm").value.trim())};
$("dmx").onclick=()=>demo("0");
$("ring").onclick=async()=>{
  if(!window.confirm("Ring now starts a real bomb on the device. Start it?"))return;
  say("ringmsg","","ringing...");
  try{const r=await api("/api/ring","");say("ringmsg",r.ok?"ok":"err",r.j.msg||r.j.error||(r.ok?"done":"failed"))}
  catch(x){say("ringmsg","err","No answer from the device")}
  poll();
};
setInterval(poll,2000);setInterval(tick,1000);poll();
</script></body></html>)HTML";

#endif /* WEB_PAGE_H */
