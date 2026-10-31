#pragma once

// Wi-Fi setup page served by the captive portal (and at /wifi on the home network).
// Plain HTML + JS, no external resources: the phone has no internet while on the portal.

namespace sb {
namespace web {

const char kSetupPage[] = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Smart Bins Wi-Fi setup</title>
<script>try{var t=localStorage.getItem('theme');if(t==='light'||t==='dark')document.documentElement.dataset.theme=t}catch(e){}</script>
<style>
:root{--bg:#f6f7f9;--card:#fff;--text:#1d2330;--muted:#667085;--accent:#2563eb;--border:#d0d5dd;--ok:#15803d;--err:#b42318}
@media (prefers-color-scheme:dark){:root:not([data-theme=light]){--bg:#111418;--card:#1b2027;--text:#e6e8eb;--muted:#9aa4b2;--accent:#60a5fa;--border:#333a44;--ok:#4ade80;--err:#f87171}}
:root[data-theme=dark]{--bg:#111418;--card:#1b2027;--text:#e6e8eb;--muted:#9aa4b2;--accent:#60a5fa;--border:#333a44;--ok:#4ade80;--err:#f87171}
*{box-sizing:border-box}body{margin:0;font:16px/1.4 system-ui,sans-serif;background:var(--bg);color:var(--text)}
main{max-width:420px;margin:0 auto;padding:16px}h1{font-size:20px;margin:8px 0 16px}
.card{background:var(--card);border:1px solid var(--border);border-radius:10px;padding:16px;margin-bottom:12px}
ul{list-style:none;margin:0;padding:0}li{display:flex;justify-content:space-between;align-items:center;gap:8px;padding:10px 4px;border-bottom:1px solid var(--border);cursor:pointer}
li.cur{font-weight:600}li.cur small{color:var(--ok);font-weight:400}
.wifi{display:inline-flex;align-items:flex-end;gap:2px;height:14px;flex:none}.wifi i{width:3px;border-radius:1px;background:var(--border)}
.wifi i:nth-child(1){height:25%}.wifi i:nth-child(2){height:50%}.wifi i:nth-child(3){height:75%}.wifi i:nth-child(4){height:100%}.wifi i.on{background:var(--text)}
.end{display:flex;align-items:center;gap:8px}
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
let current='';  // network the board is connected to, from /api/wifi/status
// Signal as four bars, drawn and graded like the indicator in the main page's
// top bar (same thresholds), with the dBm value as a tooltip.
function bars(r){const n=r>=-55?4:r>=-65?3:r>=-75?2:1,s=document.createElement('span');s.className='wifi';s.title=r+' dBm';
  for(let i=0;i<4;i++){const b=document.createElement('i');if(i<n)b.className='on';s.appendChild(b)}return s}
async function scan(){
  try{const d=await (await fetch('/api/wifi/scan')).json();
    if(d.scanning){setTimeout(scan,1500);return}
    const ul=$('nets');ul.innerHTML='';
    if(!d.networks.length){ul.innerHTML='<li><small>No networks found</small></li>'}
    d.networks.sort((a,b)=>b.rssi-a.rssi).forEach(n=>{
      const li=document.createElement('li');li.dataset.ssid=n.ssid;
      const name=document.createElement('span');name.textContent=n.ssid+(n.secure?' 🔒':'');li.appendChild(name);
      const end=document.createElement('span');end.className='end';
      const mark=document.createElement('small');mark.textContent='connected';end.appendChild(mark);
      end.appendChild(bars(n.rssi));li.appendChild(end);
      li.onclick=()=>{$('ssid').value=n.ssid;$('pass').focus()};ul.appendChild(li)});
    markCurrent();
  }catch(e){setTimeout(scan,3000)}
}
// Highlights the network the board is on; the others hide their "connected" mark.
function markCurrent(){document.querySelectorAll('#nets li[data-ssid]').forEach(li=>{
  const on=li.dataset.ssid===current;li.classList.toggle('cur',on);li.querySelector('small').hidden=!on})}
async function status(){
  try{const d=await (await fetch('/api/wifi/status')).json();const el=$('status');el.className='';
    current=d.state==='connected'?d.ssid:'';markCurrent();
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
