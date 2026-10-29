#pragma once

// Wi-Fi setup page served by the captive portal (and at /wifi on the home network).
// Plain HTML + JS, no external resources: the phone has no internet while on the portal.

namespace sb {
namespace web {

const char kSetupPage[] = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Smart Bins Wi-Fi setup</title>
<style>
:root{--bg:#f6f7f9;--card:#fff;--text:#1d2330;--muted:#667085;--accent:#2563eb;--border:#d0d5dd;--ok:#15803d;--err:#b42318}
@media (prefers-color-scheme:dark){:root{--bg:#111418;--card:#1b2027;--text:#e6e8eb;--muted:#9aa4b2;--accent:#60a5fa;--border:#333a44;--ok:#4ade80;--err:#f87171}}
*{box-sizing:border-box}body{margin:0;font:16px/1.4 system-ui,sans-serif;background:var(--bg);color:var(--text)}
main{max-width:420px;margin:0 auto;padding:16px}h1{font-size:20px;margin:8px 0 16px}
.card{background:var(--card);border:1px solid var(--border);border-radius:10px;padding:16px;margin-bottom:12px}
ul{list-style:none;margin:0;padding:0}li{display:flex;justify-content:space-between;padding:10px 4px;border-bottom:1px solid var(--border);cursor:pointer}
li:last-child{border:0}li small{color:var(--muted)}label{display:block;margin:10px 0 4px;color:var(--muted);font-size:14px}
input{width:100%;padding:10px;border:1px solid var(--border);border-radius:8px;background:var(--bg);color:var(--text);font-size:16px}
button{margin-top:14px;width:100%;padding:12px;border:0;border-radius:8px;background:var(--accent);color:#fff;font-size:16px}
#status{font-size:14px;color:var(--muted)}.ok{color:var(--ok)!important}.err{color:var(--err)!important}
</style></head><body><main>
<h1>Smart Bins Wi-Fi setup</h1>
<div class="card"><div id="status">Checking status...</div></div>
<div class="card"><ul id="nets"><li><small>Scanning for networks...</small></li></ul></div>
<form class="card" id="form">
<label for="ssid">Network name</label><input id="ssid" name="ssid" maxlength="32" required autocomplete="off">
<label for="pass">Password</label><input id="pass" name="pass" type="password" maxlength="63" autocomplete="off">
<button type="submit">Connect</button>
</form>
</main><script>
const $=id=>document.getElementById(id);
function bars(r){return r>-55?'▂▄▆█':r>-67?'▂▄▆':r>-78?'▂▄':'▂'}
async function scan(){
  try{const d=await (await fetch('/api/wifi/scan')).json();
    if(d.scanning){setTimeout(scan,1500);return}
    const ul=$('nets');ul.innerHTML='';
    if(!d.networks.length){ul.innerHTML='<li><small>No networks found</small></li>'}
    d.networks.sort((a,b)=>b.rssi-a.rssi).forEach(n=>{
      const li=document.createElement('li');li.textContent=n.ssid+(n.secure?' 🔒':'');
      const s=document.createElement('small');s.textContent=bars(n.rssi);li.appendChild(s);
      li.onclick=()=>{$('ssid').value=n.ssid;$('pass').focus()};ul.appendChild(li)});
  }catch(e){setTimeout(scan,3000)}
}
async function status(){
  try{const d=await (await fetch('/api/wifi/status')).json();const el=$('status');el.className='';
    if(d.state==='connected'){el.className='ok';el.textContent='Connected to "'+d.ssid+'". Open http://'+d.host+'/ or http://'+d.ip+'/ on your home network.'}
    else if(d.state==='connecting'){el.textContent='Connecting to "'+d.ssid+'"...'}
    else{el.textContent=d.ssid?'Could not join "'+d.ssid+'". Pick a network and try again.':'Not connected. Pick your network below.'}
  }catch(e){}
  setTimeout(status,2000);
}
$('form').onsubmit=async e=>{e.preventDefault();
  const body=new URLSearchParams(new FormData($('form')));
  const r=await fetch('/api/wifi',{method:'POST',body});
  $('status').className=r.ok?'':'err';$('status').textContent=r.ok?'Saved. Connecting...':'Error: '+(await r.text());
};
scan();status();
</script></body></html>)HTML";

}  // namespace web
}  // namespace sb
