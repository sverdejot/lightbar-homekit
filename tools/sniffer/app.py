#!/usr/bin/env python3
"""Tiny local web UI for the USB lightbar controller (tools/sniffer firmware).

Run:  ./app.py   then open http://localhost:8765
"""
import glob
import json
import threading
import time
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import serial

PORT = 8765
_lock = threading.Lock()
_ser = None


def get_serial():
    global _ser
    if _ser and _ser.is_open:
        return _ser
    ports = glob.glob("/dev/cu.usbmodem*")
    if not ports:
        raise RuntimeError("board not found (no /dev/cu.usbmodem*)")
    _ser = serial.Serial(ports[0], 115200, timeout=0.3)
    time.sleep(0.3)
    _ser.reset_input_buffer()
    return _ser


def run(cmd: str) -> list[str]:
    sniff = cmd.strip() == "sniff"
    with _lock:
        try:
            s = get_serial()
            s.reset_input_buffer()
            s.write((cmd.strip() + "\n").encode())
            out, quiet_since = [], time.time()
            deadline = time.time() + (60 if sniff else 6)
            while time.time() < deadline:
                line = s.readline().decode(errors="replace").strip()
                if line:
                    out.append(line)
                    quiet_since = time.time()
                    if sniff and "learned" in line:
                        break
                elif not sniff and time.time() - quiet_since > 0.8:
                    break
            if sniff:
                s.write(b"x\n")
            return out or ["(no response)"]
        except Exception as e:  # serial unplugged, etc.
            global _ser
            _ser = None
            return [f"error: {e}"]


PAGE = r"""<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Lightbar</title>
<style>
:root{--bg:#f6f5f2;--card:#fff;--fg:#1d1d1f;--mut:#6e6e73;--line:#e3e1dc;--acc:#e8a33d;--acc2:#4a90d9;--btn:#efede8}
@media (prefers-color-scheme:dark){:root{--bg:#141414;--card:#1f1f1f;--fg:#f2f2f2;--mut:#9a9a9a;--line:#2e2e2e;--btn:#2a2a2a}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.4 -apple-system,system-ui,sans-serif}
main{max-width:520px;margin:0 auto;padding:24px 16px}
h1{font-size:20px;margin:0 0 4px}.sub{color:var(--mut);font-size:13px;margin-bottom:20px}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:16px;margin-bottom:14px}
.card h2{font-size:13px;text-transform:uppercase;letter-spacing:.06em;color:var(--mut);margin:0 0 12px;font-weight:600}
button{font:inherit;border:0;border-radius:10px;padding:10px 14px;background:var(--btn);color:var(--fg);cursor:pointer}
button:hover{filter:brightness(.95)}button:active{transform:scale(.97)}button:disabled{opacity:.5}
.power{width:100%;padding:18px;font-size:17px;font-weight:600;background:var(--acc);color:#1d1d1f}
.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}.row+.row{margin-top:10px}
.grow{flex:1}input[type=range]{width:100%}
.lbl{display:flex;justify-content:space-between;font-size:13px;color:var(--mut);margin-bottom:4px}
.bri{accent-color:var(--acc)}.ct{accent-color:var(--acc2)}
input[type=text],input[type=number]{font:inherit;padding:9px 10px;border-radius:10px;border:1px solid var(--line);background:var(--bg);color:var(--fg);width:110px}
pre{margin:0;max-height:200px;overflow:auto;font:12px/1.5 ui-monospace,Menlo,monospace;color:var(--mut);white-space:pre-wrap}
.ctbar{height:6px;border-radius:3px;background:linear-gradient(90deg,#ffb347,#fff4e0,#cfe3ff);margin-top:6px}
</style></head><body><main>
<h1>Xiaomi Lightbar</h1><div class="sub" id="status">nice!nano · nRF52840</div>

<div class="card"><button class="power" onclick="send('on')">⏻ On / Off</button></div>

<div class="card"><h2>Absolute values</h2>
  <div class="lbl"><span>Brightness</span><span id="briv">8</span></div>
  <input class="bri" type="range" min="0" max="15" value="8" id="bri"
    oninput="briv.textContent=this.value" onchange="send('bri '+this.value)">
  <div style="height:14px"></div>
  <div class="lbl"><span>Color temperature (warm → cold)</span><span id="ctv">0</span></div>
  <input class="ct" type="range" min="0" max="15" value="0" id="ct"
    oninput="ctv.textContent=this.value" onchange="send('ct '+this.value)">
  <div class="ctbar"></div>
</div>

<div class="card"><h2>Relative steps</h2>
  <div class="lbl"><span>Step size</span><span id="stepv">3</span></div>
  <input type="range" min="1" max="15" value="3" id="step" oninput="stepv.textContent=this.value">
  <div class="row" style="margin-top:12px">
    <button class="grow" onclick="step('down')">🔅 Dimmer</button>
    <button class="grow" onclick="step('up')">🔆 Brighter</button>
  </div>
  <div class="row">
    <button class="grow" onclick="step('warm')">🟠 Warmer</button>
    <button class="grow" onclick="step('cool')">🔵 Cooler</button>
  </div>
  <div class="row"><button class="grow" onclick="send('reset')">↺ Reset (medium, warm)</button></div>
</div>

<div class="card"><h2>Settings</h2>
  <div class="row"><input type="text" id="rid" placeholder="remote id (hex)"><button onclick="send('id '+rid.value)">Set id</button>
    <button onclick="send('info')">Info</button></div>
  <div class="row"><input type="number" id="reps" min="1" max="100" value="20"><button onclick="send('reps '+reps.value)">Repetitions</button></div>
  <div class="row"><button onclick="sniff()" id="sniffb">📡 Sniff remote (60 s)</button>
    <button onclick="send('pair')" title="Power-cycle the bar, then press within 20 s">🔗 Pair</button></div>
</div>

<div class="card"><h2>Log</h2><pre id="log"></pre></div>
</main>
<script>
const log = document.getElementById('log');
function addLog(lines){ const t=new Date().toLocaleTimeString(); log.textContent = lines.map(l=>t+'  '+l).join('\n')+'\n'+log.textContent; }
async function send(cmd){
  addLog(['> '+cmd]);
  document.querySelectorAll('button').forEach(b=>b.disabled=true);
  try{
    const r = await fetch('/cmd',{method:'POST',body:JSON.stringify({cmd})});
    const j = await r.json(); addLog(j.out);
    const m = j.out.join(' ').match(/id=([0-9A-F]{6}).*reps=(\d+)/);
    if(m){ rid.value=m[1]; reps.value=m[2]; }
  }catch(e){ addLog(['error: '+e]); }
  document.querySelectorAll('button').forEach(b=>b.disabled=false);
}
function step(c){ send(c+' '+document.getElementById('step').value); }
function sniff(){ addLog(['hold the remote next to the board and turn the knob…']); send('sniff'); }
send('info');
</script></body></html>
"""


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, body, ctype):
        data = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        self._send(200, PAGE, "text/html; charset=utf-8")

    def do_POST(self):
        if self.path != "/cmd":
            return self._send(404, "{}", "application/json")
        body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or "{}")
        cmd = str(body.get("cmd", "")).replace("\n", " ")[:40]
        self._send(200, json.dumps({"out": run(cmd)}), "application/json")


if __name__ == "__main__":
    srv = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    url = f"http://localhost:{PORT}"
    print(f"Lightbar UI at {url}  (Ctrl+C to quit)")
    threading.Timer(0.5, lambda: webbrowser.open(url)).start()
    srv.serve_forever()
