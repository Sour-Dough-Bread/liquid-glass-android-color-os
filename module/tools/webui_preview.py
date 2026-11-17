#!/usr/bin/env python3
"""Serve the module's WebUI in a desktop browser, driving a real device over adb.

    module/tools/webui_preview.py [--port 8765] [--moddir /data/local/tmp/sgmod] [--serial ID]

KernelSU's WebUI injects a `ksu.exec(cmd, options, callback)` bridge that runs
commands as root. This serves module/webroot/index.html with a stand-in for that
bridge which POSTs each command back here, where it runs through `adb shell`.
So the page exercises the real glassctl, the real glasscheck and the real
engine on the device, not a mock of them.

The device needs `adb root` (an emulator or userdebug build). Pass --serial (or
set ANDROID_SERIAL) if more than one is attached. Commands that name the installed module's path
are rewritten to --moddir, so a module staged by hand is enough:

    adb shell mkdir -p /data/local/tmp/sgmod/bin/x86_64
    adb push module/glassctl /data/local/tmp/sgmod/
    adb push engine/tools/out/x86_64/glasscheck /data/local/tmp/sgmod/bin/x86_64/
    adb shell 'echo x86_64 > /data/local/tmp/sgmod/abi'

Binds to 127.0.0.1 only: anything that can reach it gets a root shell on the
device.
"""
import argparse
import json
import pathlib
import subprocess
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = pathlib.Path(__file__).resolve().parent.parent / "webroot"
INSTALLED = "/data/adb/modules/stockglass"

SHIM = """<script>
window.ksu = {
  exec(cmd, options, cb) {
    fetch('/__exec', { method: 'POST', body: cmd })
      .then((r) => r.json())
      .then((j) => window[cb](j.errno, j.stdout, j.stderr))
      .catch((e) => window[cb](1, '', String(e)));
  },
};
</script>
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--moddir", default="/data/local/tmp/sgmod")
    ap.add_argument("--serial", default=None, help="adb device to drive")
    args = ap.parse_args()
    adb = ["adb"] + (["-s", args.serial] if args.serial else [])

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt, *a):
            pass

        def _send(self, code, body, ctype):
            data = body.encode() if isinstance(body, str) else body
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            if self.path.split("?")[0] not in ("/", "/index.html"):
                self._send(404, "not found", "text/plain")
                return
            html = (ROOT / "index.html").read_text()
            html = html.replace("<head>", "<head>\n" + SHIM, 1)
            self._send(200, html, "text/html; charset=utf-8")

        def do_POST(self):
            if self.path != "/__exec":
                self._send(404, "not found", "text/plain")
                return
            cmd = self.rfile.read(int(self.headers.get("Content-Length", 0))).decode()
            cmd = cmd.replace(INSTALLED, args.moddir)
            print(f"$ {cmd}", flush=True)
            try:
                p = subprocess.run(adb + ["shell", cmd], capture_output=True, text=True,
                                   timeout=30)
                out = {"errno": p.returncode, "stdout": p.stdout, "stderr": p.stderr}
            except subprocess.TimeoutExpired:
                out = {"errno": 124, "stdout": "", "stderr": "timed out"}
            self._send(200, json.dumps(out), "application/json")

    srv = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"WebUI preview on http://127.0.0.1:{args.port}/ (module at {args.moddir})", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
