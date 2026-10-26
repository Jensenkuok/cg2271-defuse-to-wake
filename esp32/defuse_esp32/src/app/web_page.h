/*
 * Defuse to Wake - the device's main web page: the minimal settings page, ONE self-contained document
 * (HTML + CSS + JS inline, system fonts, no external asset, no CDN), served from flash by web_task.cpp
 * at "/". Budget: 20 KB (tools/test_page.cpp enforces it). It polls /api/status every 2 s (fetch, 2 s
 * timeout; a settings page does not need faster) and shows a "Connection lost" banner when that fails.
 * It deliberately has no link to the engineering panel (web_eng_page.h, served at "/eng").
 * Every piece of device text (venues, module codes) is put in with textContent, never as markup: the
 * strings come from NUSMods.
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
:root{--bg:#f6f6f4;--card:#fff;--ink:#1c1c1a;--mute:#74746d;--line:#e2e2dc;--acc:#c2410c;--on:#fff;--ok:#15803d;--bad:#b91c1c}
@media(prefers-color-scheme:dark){:root{--bg:#141413;--card:#1d1d1b;--ink:#efefea;--mute:#9a9a92;--line:#33332f;--acc:#fb923c;--on:#1a1208;--ok:#4ade80;--bad:#f87171}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);font:16px/1.45 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
main{max-width:420px;margin:0 auto;padding:32px 16px 48px}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:24px 20px}
.lbl{font-size:13px;color:var(--mute)}
.time{font:600 56px/1.1 ui-monospace,Consolas,monospace;font-variant-numeric:tabular-nums;margin:4px 0 6px}
.mute{color:var(--mute);font-size:14px}
label{display:block;font-weight:600;font-size:14px;margin:28px 0 8px}
input[type=text]{width:100%;padding:11px 12px;font:inherit;color:var(--ink);background:var(--bg);border:1px solid var(--line);border-radius:12px}
.rng{display:flex;align-items:center;gap:14px}
.rng input{flex:1;accent-color:var(--acc);margin:0}
.rng output{min-width:64px;text-align:right;font-variant-numeric:tabular-nums;font-weight:600}
.btn{display:block;width:100%;font:inherit;font-weight:600;padding:12px;border-radius:12px;border:1px solid var(--acc);cursor:pointer}
.btn.fill{background:var(--acc);color:var(--on);margin-top:28px}
.btn.line{background:none;color:var(--acc);margin-top:12px}
.btn:disabled{opacity:.5;cursor:default}
.btn:focus-visible,input:focus-visible{outline:3px solid var(--acc);outline-offset:2px}
.msg{min-height:20px;margin:10px 0 0;font-size:14px;text-align:center}
.ok{color:var(--ok)}.err{color:var(--bad)}
#lost{position:sticky;top:0;z-index:5;background:var(--bad);color:#fff;text-align:center;font-size:14px;font-weight:600;padding:6px}
[hidden]{display:none!important}
</style></head><body>
<div id="lost" hidden>Connection lost - retrying...</div>
<main><div class="card">
<div class="lbl">Next alarm</div><div class="time" id="al">--</div><div class="mute" id="al2">&nbsp;</div>
<form id="form"><label for="lk">NUSMods share link</label>
<input type="text" id="lk" maxlength="512" autocomplete="off" spellcheck="false">
<label for="ld">Wake up before first class</label>
<div class="rng"><input type="range" id="ld" min="15" max="180" step="5" value="60"><output id="ldv" for="ld">60 min</output></div>
<button class="btn fill" type="submit">Save</button><div class="msg" id="fmsg"></div></form>
<button class="btn line" id="ring" type="button">Ring now</button><div class="msg" id="ringmsg"></div>
</div></main>
<script>
"use strict";
const $=i=>document.getElementById(i);
let fails=0,busy=false,dirty=false,cfgVer=-1;

async function api(url,body,ms){
  const ac=new AbortController(),t=setTimeout(()=>ac.abort(),ms||4000);
  try{const o={signal:ac.signal,cache:"no-store",headers:{"X-Requested-With":"defuse"}};
    if(body!==undefined){o.method="POST";o.headers["Content-Type"]="application/x-www-form-urlencoded";o.body=body}
    const r=await fetch(url,o);let j={};try{j=await r.json()}catch(e){}
    return{ok:r.ok,j:j}}
  finally{clearTimeout(t)}
}
function say(id,cls,txt){const m=$(id);m.className="msg "+cls;m.textContent=txt}
function readout(){$("ldv").textContent=$("ld").value+" min"}
async function poll(){
  if(busy)return;busy=true;
  try{const r=await api("/api/status",undefined,2000);if(!r.ok)throw 0;fails=0;render(r.j)}
  catch(e){fails++}
  busy=false;$("lost").hidden=fails<2;
}
function render(s){
  const a=s.alarm;
  $("al").textContent=a.valid?a.local.slice(-5):"No alarm set";
  $("al2").textContent=a.valid?(a.local.slice(0,3)+" · "+a.module+" "+a.type+" at "+a.class_local.slice(-5)):" ";
  if(s.settings.ver!==cfgVer)loadCfg(s.settings.ver);
}
async function loadCfg(v){
  cfgVer=v;
  try{const r=await api("/api/settings");if(!r.ok)throw 0;
    if(!dirty&&document.activeElement!==$("lk")&&document.activeElement!==$("ld")){$("lk").value=r.j.nusmods_link;$("ld").value=r.j.lead_min;readout()}
  }catch(e){cfgVer=-1}
}
$("lk").oninput=()=>{dirty=true};
$("ld").oninput=()=>{dirty=true;readout()};
$("form").onsubmit=async e=>{
  e.preventDefault();say("fmsg","","saving...");
  try{const b=new URLSearchParams({nusmods_link:$("lk").value.trim(),lead_min:$("ld").value}).toString();
    const r=await api("/api/settings",b);
    if(r.ok){dirty=false;say("fmsg","ok","Saved: "+r.j.lead_min+" min lead; timetable refresh started")}
    else say("fmsg","err",r.j.error||"rejected")
  }catch(x){say("fmsg","err","No answer from the device")}
  poll();
};
$("ring").onclick=async()=>{
  say("ringmsg","","ringing...");
  try{const r=await api("/api/ring","");say("ringmsg",r.ok?"ok":"err",r.j.msg||r.j.error||(r.ok?"done":"failed"))}
  catch(x){say("ringmsg","err","No answer from the device")}
  poll();
};
setInterval(poll,2000);poll();
</script></body></html>)HTML";

#endif /* WEB_PAGE_H */
