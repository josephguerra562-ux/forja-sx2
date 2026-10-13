#!/usr/bin/env python3
# PS4SX2 Debug Server - live telemetry + FTP file manager + dashboard
# - TCP listener for PS4 telemetry (JSON lines: log, heartbeat, stage, game, hello, bye)
# - Pushes live updates to dashboard via WebSocket/SSE fallback
# - FTP file manager for sending files to PS4 + marker toggles
# - Serves dashboard at http://localhost:<web_port>

import json, re, time, threading, ftplib, io, os, sys, pathlib, socket, collections, traceback, queue, hashlib, base64, struct, select
from http.server import HTTPServer, SimpleHTTPRequestHandler, BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs, unquote
from socketserver import ThreadingMixIn

CFG_PATH = pathlib.Path(__file__).with_name("config.json")
if not CFG_PATH.is_file():
    raise SystemExit("Missing config.json. Copy config.example.json to config.json and set your PS4 IP address and FTP settings.")
with open(CFG_PATH, encoding="utf-8-sig") as f:
    CFG = json.load(f)

PS4 = CFG["ps4"]
WEB_PORT = CFG.get("web_port", 8080)
WEB_HOST = CFG.get("web_host", "127.0.0.1")
TEL_PORT = CFG.get("telemetry_port", 19423)
POLL_MS = CFG.get("poll_interval_ms", 500)
PATHS = CFG["paths"]
LOG_DIR = pathlib.Path(__file__).with_name("logs")
LOG_DIR.mkdir(exist_ok=True)

# ---------------------------------------------------------------------------
# State
# ---------------------------------------------------------------------------
state_lock = threading.Lock()
telemetry_state = {
    "connected": False,          # PS4 telemetry TCP connected
    "ps4_host": "",
    "ps4_port": 0,
    "boot_time": 0,              # server-side epoch when PS4 connected
    "ps4_boot_s": 0,             # PS4-side boot seconds (from hello/heartbeat)
    "execution_s": 0,            # time since PS4 boot (updated on heartbeat)
    "vm_state": -1,
    "menu_open": False,
    "vsyncq": 0,
    "game": "",
    "platform": "",
    "stage": "disconnected",
    "ee_pc": 0, "iop_pc": 0,
    "ee_mcps": 0, "polls": 0, "clock_checks": 0,
    "vm_s": 0,
    "last_heartbeat_ts": 0,
    "last_log_ts": 0,
    "connect_count": 0,
    "disconnect_count": 0,
    "error": "",
}

ftp_state = {
    "connected": False,
    "error": "",
    "markers": {},
    "files": [],  # listing of markers_dir
}

# Ring buffers
LOG_MAX = 2000
log_buffer = collections.deque(maxlen=LOG_MAX)  # each {ts, boot_s, type, msg, raw}
heartbeat_buffer = collections.deque(maxlen=600)  # each heartbeat sample
stage_log = collections.deque(maxlen=100)         # stage transitions
alerts = collections.deque(maxlen=50)

# SSE / WS subscribers: list of queue.Queue
subscribers = []
sub_lock = threading.Lock()

PERF_RE = re.compile(r"Speed_pct=([\d.]+)")
CRASH_PATTERNS = ["Unknown R5900", "Unrecognized op", "Trap exception", "TLB miss", "Bus error"]

session_log_file = None
session_log_path = None

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def broadcast(event_type, data):
    """Push event to all SSE/WS subscribers."""
    msg = json.dumps({"type": event_type, "data": data})
    with sub_lock:
        for q in list(subscribers):
            try: q.put_nowait(msg)
            except: pass

def append_log(entry):
    log_buffer.append(entry)
    broadcast("log", entry)
    # File log
    if session_log_file:
        try:
            session_log_file.write(json.dumps(entry, ensure_ascii=False) + "\n")
            session_log_file.flush()
        except: pass
    # Alerts
    raw = entry.get("raw") or entry.get("msg") or ""
    for pat in CRASH_PATTERNS:
        if pat in raw:
            alert = {"ts": time.time(), "boot_s": entry.get("boot_s", 0), "msg": f"CRASH: {raw[:180]}"}
            alerts.append(alert)
            broadcast("alert", alert)
            break

def rotate_session_log():
    global session_log_file, session_log_path
    if session_log_file:
        try: session_log_file.close()
        except: pass
    ts = time.strftime("%Y%m%d_%H%M%S")
    session_log_path = LOG_DIR / f"session_{ts}.jsonl"
    session_log_file = open(str(session_log_path), "a", encoding="utf-8")
    print(f"[debug-server] Session log: {session_log_path}")

rotate_session_log()

# ---------------------------------------------------------------------------
# FTP helpers
# ---------------------------------------------------------------------------
def ftp_connect():
    ftp = ftplib.FTP()
    ftp.connect(PS4["host"], PS4["port"], timeout=PS4.get("timeout", 5))
    ftp.login(PS4.get("user","anonymous"), PS4.get("pass","anonymous@"))
    ftp.set_pasv(True)
    return ftp

def ftp_poll_loop():
    while True:
        try:
            ftp = ftp_connect()
            files = ftp.nlst(PATHS["markers_dir"])
            basenames = set(os.path.basename(p) for p in files)
            markers = {}
            for mk in CFG.get("markers", []):
                markers[mk] = mk in basenames
            detailed = []
            try:
                # Try MLSD for sizes/times, fallback to nlst
                for name in files:
                    detailed.append(os.path.basename(name))
            except: pass
            ftp.quit()
            with state_lock:
                ftp_state["connected"] = True
                ftp_state["error"] = ""
                ftp_state["markers"] = markers
                ftp_state["files"] = sorted(basenames)
            broadcast("ftp_state", {"connected": True, "markers": markers, "files": sorted(basenames)})
        except Exception as e:
            with state_lock:
                ftp_state["connected"] = False
                ftp_state["error"] = str(e)[:300]
        time.sleep(max(2, POLL_MS / 1000.0 * 4))

# ---------------------------------------------------------------------------
# Telemetry TCP listener
# ---------------------------------------------------------------------------
def handle_telemetry_message(line, addr):
    now = time.time()
    try:
        obj = json.loads(line)
    except:
        # Plain text fallback -> treat as log
        append_log({"ts": now, "boot_s": telemetry_state.get("ps4_boot_s", 0), "type": "log", "msg": line[:2000], "raw": line[:2000]})
        return
    t = obj.get("type", "log")
    boot_s = obj.get("boot_s", 0)

    if t == "hello":
        with state_lock:
            telemetry_state["connected"] = True
            telemetry_state["ps4_host"] = addr[0]
            telemetry_state["ps4_port"] = addr[1]
            telemetry_state["boot_time"] = now
            telemetry_state["ps4_boot_s"] = boot_s
            telemetry_state["stage"] = "connected"
            telemetry_state["connect_count"] = telemetry_state.get("connect_count", 0) + 1
            telemetry_state["error"] = ""
        stage_log.append({"ts": now, "boot_s": boot_s, "stage": "connected", "addr": addr[0]})
        append_log({"ts": now, "boot_s": boot_s, "type": "system", "msg": f"PS4 connected from {addr[0]}:{addr[1]} build={obj.get('build','')} boot_s={boot_s:.2f}", "raw": line[:500]})
        broadcast("telemetry", dict(telemetry_state))

    elif t == "heartbeat":
        hb = {
            "ts": now, "boot_s": boot_s,
            "vm_s": obj.get("vm_s", 0),
            "ee_pc": obj.get("ee_pc", 0), "iop_pc": obj.get("iop_pc", 0),
            "ee_mcps": obj.get("ee_mcps", 0), "polls": obj.get("polls", 0),
            "clock_checks": obj.get("clock_checks", 0),
            "vm_state": obj.get("vm_state", -1), "menu": obj.get("menu", 0),
            "vsyncq": obj.get("vsyncq", 0), "game": obj.get("game", ""),
        }
        heartbeat_buffer.append(hb)
        with state_lock:
            telemetry_state["execution_s"] = boot_s
            telemetry_state["ps4_boot_s"] = boot_s
            telemetry_state["vm_s"] = hb["vm_s"]
            telemetry_state["ee_pc"] = hb["ee_pc"]
            telemetry_state["iop_pc"] = hb["iop_pc"]
            telemetry_state["ee_mcps"] = hb["ee_mcps"]
            telemetry_state["polls"] = hb["polls"]
            telemetry_state["clock_checks"] = hb["clock_checks"]
            telemetry_state["vm_state"] = hb["vm_state"]
            telemetry_state["menu_open"] = bool(hb["menu"])
            telemetry_state["vsyncq"] = hb["vsyncq"]
            if hb["game"]:
                telemetry_state["game"] = hb["game"]
            telemetry_state["last_heartbeat_ts"] = now
            telemetry_state["connected"] = True
        broadcast("heartbeat", hb)
        broadcast("telemetry", dict(telemetry_state))

    elif t == "stage":
        msg = obj.get("msg", "")
        with state_lock:
            telemetry_state["stage"] = msg
            telemetry_state["ps4_boot_s"] = boot_s
        stage_log.append({"ts": now, "boot_s": boot_s, "stage": msg})
        append_log({"ts": now, "boot_s": boot_s, "type": "stage", "msg": f"STAGE: {msg}", "raw": line[:500]})
        broadcast("telemetry", dict(telemetry_state))

    elif t == "game":
        fpath = obj.get("file", "")
        plat = obj.get("platform", "")
        with state_lock:
            telemetry_state["game"] = fpath
            telemetry_state["platform"] = plat
            telemetry_state["ps4_boot_s"] = boot_s
        append_log({"ts": now, "boot_s": boot_s, "type": "game", "msg": f"Game: {fpath} [{plat}]", "raw": line[:500]})
        broadcast("telemetry", dict(telemetry_state))

    elif t == "bye":
        append_log({"ts": now, "boot_s": boot_s, "type": "system", "msg": f"PS4 disconnect boot_s={boot_s:.2f}", "raw": line[:500]})
        with state_lock:
            telemetry_state["stage"] = "disconnected"
        broadcast("telemetry", dict(telemetry_state))

    elif t == "log":
        msg = obj.get("msg", "")
        append_log({"ts": now, "boot_s": boot_s, "type": "log", "msg": msg, "raw": msg[:2000]})
    else:
        append_log({"ts": now, "boot_s": boot_s, "type": t, "msg": obj.get("msg", line[:500]), "raw": line[:2000]})

def telemetry_client_thread(conn, addr):
    print(f"[telemetry] PS4 connected: {addr}")
    conn.settimeout(30)
    buf = b""
    try:
        while True:
            data = conn.recv(8192)
            if not data:
                break
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                if not line.strip():
                    continue
                try:
                    handle_telemetry_message(line.decode("utf-8", errors="ignore"), addr)
                except Exception as e:
                    print(f"[telemetry] handle error: {e}")
                    traceback.print_exc()
            # Prevent unbounded buffer on no-newline garbage
            if len(buf) > 16384:
                buf = b""
    except socket.timeout:
        print(f"[telemetry] timeout: {addr}")
    except Exception as e:
        print(f"[telemetry] client error {addr}: {e}")
    finally:
        try: conn.close()
        except: pass
        with state_lock:
            # Only mark disconnected if this was the active connection
            telemetry_state["connected"] = False
            telemetry_state["disconnect_count"] = telemetry_state.get("disconnect_count", 0) + 1
        print(f"[telemetry] PS4 disconnected: {addr}")
        broadcast("telemetry", dict(telemetry_state))

def telemetry_listener():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        srv.bind(("0.0.0.0", TEL_PORT))
    except OSError as e:
        print(f"[telemetry] FATAL: cannot bind 0.0.0.0:{TEL_PORT}: {e}")
        print(f"[telemetry] Check config.json telemetry_port or kill process using port.")
        return
    srv.listen(8)
    print(f"[telemetry] Listening on 0.0.0.0:{TEL_PORT} (PS4 target: set /data/PS4SX2/telemetry.conf)")
    while True:
        try:
            conn, addr = srv.accept()
            threading.Thread(target=telemetry_client_thread, args=(conn, addr), daemon=True).start()
        except Exception as e:
            print(f"[telemetry] accept error: {e}")
            time.sleep(0.5)

# ---------------------------------------------------------------------------
# Minimal WebSocket (RFC6455) for live push — no external deps
# ---------------------------------------------------------------------------
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

def ws_handshake(path, headers):
    key = headers.get("sec-websocket-key", "")
    if not key:
        return None
    accept = base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
    return (
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Accept: {accept}\r\n\r\n"
    ).encode()

def ws_encode(text):
    data = text.encode("utf-8")
    n = len(data)
    if n < 126:
        hdr = struct.pack("!BB", 0x81, n)
    elif n < 65536:
        hdr = struct.pack("!BBH", 0x81, 126, n)
    else:
        hdr = struct.pack("!BBQ", 0x81, 127, n)
    return hdr + data

# ---------------------------------------------------------------------------
# HTTP handler
# ---------------------------------------------------------------------------
class Handler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        # quiet except errors; print to stderr for visibility
        pass

    def do_GET(self):
        p = urlparse(self.path)
        path = p.path
        qs = parse_qs(p.query)

        # WebSocket upgrade
        if path == "/ws":
            key = self.headers.get("Sec-WebSocket-Key", "")
            if key:
                # Upgrade
                resp = ws_handshake(path, {k.lower(): v for k, v in self.headers.items()})
                if resp:
                    self.request.sendall(resp)
                    q = queue.Queue()
                    with sub_lock:
                        subscribers.append(q)
                    # Send initial state
                    try:
                        self.request.sendall(ws_encode(json.dumps({"type": "hello", "data": {"telemetry": telemetry_state, "ftp": ftp_state}})))
                        # Flush buffered history
                        for entry in list(log_buffer)[-200:]:
                            self.request.sendall(ws_encode(json.dumps({"type": "log", "data": entry})))
                        for hb in list(heartbeat_buffer)[-20:]:
                            self.request.sendall(ws_encode(json.dumps({"type": "heartbeat", "data": hb})))
                    except: pass
                    # Pump
                    self.request.settimeout(60)
                    try:
                        while True:
                            try:
                                msg = q.get(timeout=25)
                                self.request.sendall(ws_encode(msg))
                            except queue.Empty:
                                # ping
                                try:
                                    self.request.sendall(struct.pack("!BB", 0x89, 0))
                                except: break
                            # drain any websocket frames from client (ignore)
                            try:
                                self.request.settimeout(0.01)
                                data = self.request.recv(1024)
                                if not data:
                                    break
                                # handle close frame
                                if data[0] == 0x88:
                                    break
                            except socket.timeout:
                                pass
                            finally:
                                self.request.settimeout(60)
                    except: pass
                    finally:
                        with sub_lock:
                            if q in subscribers: subscribers.remove(q)
                    return
            self.send_response(400); self.end_headers(); return

        # SSE fallback (for browsers without WS or curl)
        if path == "/api/events":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "keep-alive")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            q = queue.Queue()
            with sub_lock:
                subscribers.append(q)
            try:
                # initial
                self.wfile.write(f"data: {json.dumps({'type':'hello','data':{'telemetry': telemetry_state, 'ftp': ftp_state}})}\n\n".encode())
                self.wfile.flush()
                for entry in list(log_buffer)[-100:]:
                    self.wfile.write(f"data: {json.dumps({'type':'log','data': entry})}\n\n".encode())
                self.wfile.flush()
                while True:
                    try:
                        msg = q.get(timeout=20)
                        self.wfile.write(f"data: {msg}\n\n".encode())
                        self.wfile.flush()
                    except queue.Empty:
                        self.wfile.write(b": keepalive\n\n")
                        self.wfile.flush()
            except: pass
            finally:
                with sub_lock:
                    if q in subscribers: subscribers.remove(q)
            return

        if path == "/api/state":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            out = {
                "telemetry": telemetry_state,
                "ftp": ftp_state,
                "heartbeat_history": list(heartbeat_buffer)[-120:],
                "logs": list(log_buffer)[-300:],
                "stages": list(stage_log)[-40:],
                "alerts": list(alerts)[-20:],
                "config": {"ps4": PS4, "telemetry_port": TEL_PORT, "web_port": WEB_PORT},
            }
            self.wfile.write(json.dumps(out, ensure_ascii=False).encode())
            return

        if path == "/api/marker":
            name = qs.get("name", [""])[0]
            action = qs.get("action", ["toggle"])[0]
            if name not in CFG.get("markers", []):
                self.send_response(400); self.end_headers()
                self.wfile.write(b"unknown marker"); return
            try:
                ftp = ftp_connect()
                is_present = ftp_state["markers"].get(name, False)
                if action == "create" or (action == "toggle" and not is_present):
                    ftp.storbinary(f"STOR {PATHS['markers_dir']}/{name}", io.BytesIO(b""))
                    ftp_state["markers"][name] = True
                else:
                    ftp.delete(f"{PATHS['markers_dir']}/{name}")
                    ftp_state["markers"][name] = False
                ftp.quit()
                self.send_response(200); self.send_header("Content-Type","application/json"); self.send_header("Access-Control-Allow-Origin","*"); self.end_headers()
                self.wfile.write(json.dumps({"ok": True, "markers": ftp_state["markers"]}).encode())
                append_log({"ts": time.time(), "boot_s": telemetry_state.get("ps4_boot_s",0), "type":"system", "msg": f"Marker {action} {name} -> {ftp_state['markers'][name]}", "raw": f"marker {name}"})
            except Exception as e:
                self.send_response(500); self.send_header("Content-Type","application/json"); self.end_headers()
                self.wfile.write(json.dumps({"ok": False, "error": str(e)}).encode())
            return

        if path == "/api/ftp/list":
            target = qs.get("path", [PATHS["markers_dir"]])[0]
            # sanitize: only allow /data/PS4SX2/**
            if not target.startswith("/data/PS4SX2"):
                self.send_response(400); self.end_headers(); return
            try:
                ftp = ftp_connect()
                files = []
                try:
                    ftp.retrlines(f"LIST {target}", lambda l: files.append(l))
                except:
                    files = ftp.nlst(target)
                ftp.quit()
                self.send_response(200); self.send_header("Content-Type","application/json"); self.send_header("Access-Control-Allow-Origin","*"); self.end_headers()
                self.wfile.write(json.dumps({"ok": True, "path": target, "files": files}).encode())
            except Exception as e:
                self.send_response(500); self.send_header("Content-Type","application/json"); self.end_headers()
                self.wfile.write(json.dumps({"ok": False, "error": str(e)}).encode())
            return

        if path == "/api/ftp/pull":
            # Download file from PS4 via FTP and return as download
            target = qs.get("path", [""])[0]
            if not target.startswith("/data/PS4SX2"):
                self.send_response(400); self.end_headers(); return
            try:
                ftp = ftp_connect()
                bio = io.BytesIO()
                ftp.retrbinary(f"RETR {target}", bio.write)
                ftp.quit()
                data = bio.getvalue()
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Disposition", f'attachment; filename="{os.path.basename(target)}"')
                self.send_header("Content-Length", str(len(data)))
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                self.wfile.write(data)
            except Exception as e:
                self.send_response(500); self.send_header("Content-Type","application/json"); self.end_headers()
                self.wfile.write(json.dumps({"ok": False, "error": str(e)}).encode())
            return

        if path in ("/", "/index.html"):
            # Serve dashboard
            dash = pathlib.Path(__file__).with_name("index.html")
            if dash.exists():
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.end_headers()
                self.wfile.write(dash.read_bytes())
            else:
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.end_headers()
                self.wfile.write(b"<h1>PS4SX2 Debug Server</h1><p>index.html not found - restore it or open /api/state</p>")
            return

        # Static files from this dir
        safe = pathlib.Path(__file__).parent / unquote(path.lstrip("/"))
        try:
            resolved = safe.resolve()
            base = pathlib.Path(__file__).parent.resolve()
            if resolved.is_file() and str(resolved).startswith(str(base)):
                ctype = "text/plain"
                if resolved.suffix == ".html": ctype = "text/html; charset=utf-8"
                elif resolved.suffix == ".js": ctype = "application/javascript"
                elif resolved.suffix == ".css": ctype = "text/css"
                elif resolved.suffix == ".json": ctype = "application/json"
                self.send_response(200)
                self.send_header("Content-Type", ctype)
                self.end_headers()
                self.wfile.write(resolved.read_bytes())
                return
        except: pass
        self.send_response(404); self.end_headers()

    def do_POST(self):
        p = urlparse(self.path)
        path = p.path
        qs = parse_qs(p.query)

        if path == "/api/ftp/push":
            # Upload file to PS4: multipart or raw body
            target = qs.get("path", [""])[0]
            if not target.startswith("/data/PS4SX2"):
                self.send_response(400); self.end_headers(); return
            length = int(self.headers.get("Content-Length", 0))
            data = self.rfile.read(length) if length else b""
            # Try multipart parse (simple)
            ctype = self.headers.get("Content-Type", "")
            if "multipart/form-data" in ctype and b"filename=" in data:
                # Extract file content between headers and boundary
                # Find double CRLF after Content-Type
                try:
                    header_end = data.index(b"\r\n\r\n")
                    body_start = header_end + 4
                    # Find last boundary
                    boundary = ctype.split("boundary=")[-1].strip()
                    boundary_bytes = ("--" + boundary).encode()
                    body_end = data.rindex(boundary_bytes)
                    # strip trailing CRLF
                    file_data = data[body_start:body_end].rstrip(b"\r\n")
                    data = file_data
                except: pass
            try:
                ftp = ftp_connect()
                # Ensure dir exists
                ftp.storbinary(f"STOR {target}", io.BytesIO(data))
                ftp.quit()
                self.send_response(200); self.send_header("Content-Type","application/json"); self.send_header("Access-Control-Allow-Origin","*"); self.end_headers()
                self.wfile.write(json.dumps({"ok": True, "path": target, "bytes": len(data)}).encode())
                append_log({"ts": time.time(), "boot_s": telemetry_state.get("ps4_boot_s",0), "type":"system", "msg": f"FTP push {target} {len(data)} bytes", "raw": f"push {target}"})
            except Exception as e:
                self.send_response(500); self.send_header("Content-Type","application/json"); self.end_headers()
                self.wfile.write(json.dumps({"ok": False, "error": str(e)}).encode())
            return

        if path == "/api/ftp/delete":
            target = qs.get("path", [""])[0]
            if not target.startswith("/data/PS4SX2"):
                self.send_response(400); self.end_headers(); return
            try:
                ftp = ftp_connect()
                ftp.delete(target)
                ftp.quit()
                self.send_response(200); self.send_header("Content-Type","application/json"); self.send_header("Access-Control-Allow-Origin","*"); self.end_headers()
                self.wfile.write(json.dumps({"ok": True}).encode())
            except Exception as e:
                self.send_response(500); self.send_header("Content-Type","application/json"); self.end_headers()
                self.wfile.write(json.dumps({"ok": False, "error": str(e)}).encode())
            return

        self.send_response(404); self.end_headers()

    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

class ThreadedHTTPServer(ThreadingMixIn, HTTPServer):
    daemon_threads = True
    allow_reuse_address = True

def main():
    os.chdir(pathlib.Path(__file__).parent)
    threading.Thread(target=telemetry_listener, daemon=True).start()
    threading.Thread(target=ftp_poll_loop, daemon=True).start()
    print(f"PS4SX2 Debug Server")
    print(f"  Dashboard : http://localhost:{WEB_PORT}")
    print(f"  Telemetry : 0.0.0.0:{TEL_PORT}  (PS4 -> PC)")
    print(f"  FTP       : {PS4['host']}:{PS4['port']} -> {PATHS['markers_dir']}")
    print(f"  PS4 config: /data/PS4SX2/telemetry.conf  ->  host=YOUR_PC_IP and port={TEL_PORT} on separate lines")
    print(f"  Logs      : {LOG_DIR}")
    try:
        ThreadedHTTPServer((WEB_HOST, WEB_PORT), Handler).serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down...")
        if session_log_file:
            try: session_log_file.close()
            except: pass

if __name__ == "__main__":
    main()
