
"""

 Requires:
   * Python 3.8+
   * `sdcc`  on PATH  (comes with the SDCC installer)
   * PC's WiFi is connected to FOTA-Gateway 
================================================================================
"""

import json
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib import error as uerror
from urllib import request as urequest

# ---------------------------------------------------------------- configuration
# Port 8080 on Windows is frequently reserved by Hyper-V/Docker/WSL — use 5555.
# Override via  `python server.py <port>`  if this one is also taken.
import sys as _sys
try:
    LISTEN_PORT = int(_sys.argv[1]) if len(_sys.argv) > 1 else 5555
except ValueError:
    LISTEN_PORT = 5555
ESP32_UPLOAD_URL = "http://192.168.4.1/upload"
STATIC_DIR = Path(__file__).parent
LIB_DIR = STATIC_DIR / "lib"
SDCC = shutil.which("sdcc") or "sdcc"
MAKEBIN = shutil.which("makebin") or "makebin"
APP_BASE = 0x8400
APP_MAX = 16 * 1024

LIBRARIES = {
    # Custom libraries
    "stm8_lcd.h": "stm8_lcd.c",
    # STM8L15x Standard Peripheral Library headers
    "stm8l15x.h": "",  # Core header (header-only)
    "stm8l15x_adc.h": "stm8l15x_adc.c",
    "stm8l15x_aes.h": "stm8l15x_aes.c",
    "stm8l15x_beep.h": "stm8l15x_beep.c",
    "stm8l15x_clk.h": "stm8l15x_clk.c",
    "stm8l15x_comp.h": "stm8l15x_comp.c",
    "stm8l15x_dac.h": "stm8l15x_dac.c",
    "stm8l15x_dma.h": "stm8l15x_dma.c",
    "stm8l15x_exti.h": "stm8l15x_exti.c",
    "stm8l15x_flash.h": "stm8l15x_flash.c",
    "stm8l15x_gpio.h": "stm8l15x_gpio.c",
    "stm8l15x_i2c.h": "stm8l15x_i2c.c",
    "stm8l15x_irtim.h": "stm8l15x_irtim.c",
    "stm8l15x_itc.h": "stm8l15x_itc.c",
    "stm8l15x_iwdg.h": "stm8l15x_iwdg.c",
    "stm8l15x_lcd.h": "stm8l15x_lcd.c",
    "stm8l15x_pwr.h": "stm8l15x_pwr.c",
    "stm8l15x_rst.h": "stm8l15x_rst.c",
    "stm8l15x_rtc.h": "stm8l15x_rtc.c",
    "stm8l15x_spi.h": "stm8l15x_spi.c",
    "stm8l15x_syscfg.h": "stm8l15x_syscfg.c",
    "stm8l15x_tim1.h": "stm8l15x_tim1.c",
    "stm8l15x_tim2.h": "stm8l15x_tim2.c",
    "stm8l15x_tim3.h": "stm8l15x_tim3.c",
    "stm8l15x_tim4.h": "stm8l15x_tim4.c",
    "stm8l15x_tim5.h": "stm8l15x_tim5.c",
    "stm8l15x_usart.h": "stm8l15x_usart.c",
    "stm8l15x_wfe.h": "stm8l15x_wfe.c",
    "stm8l15x_wwdg.h": "stm8l15x_wwdg.c",
}

# --- ST Standard Peripheral Library (patched for SDCC) ---------------------
# When the user's C source #includes any of the SPL_HEADER_MAP keys, we add
# the ST include paths (SPL_INC) and compile the associated .c files
# alongside user.c into one .ihx.
SPL_ROOT = STATIC_DIR / "stm8l-discovery-master" / "discovery"
SPL_INC = [
    SPL_ROOT / "Libraries" / "STM8L15x_StdPeriph_Driver" / "inc",
    SPL_ROOT / "Project"   / "Discover" / "inc",
]
_SPL_SRC = SPL_ROOT / "Libraries" / "STM8L15x_StdPeriph_Driver" / "src"
_DIS_SRC = SPL_ROOT / "Project"   / "Discover" / "src"

# ST header  ->  list of .c files (SPL + Discover) that must be linked in.
SPL_HEADER_MAP = {
    "stm8l_discovery_lcd.h": [
        _DIS_SRC / "stm8l_discovery_lcd.c",
        _SPL_SRC / "stm8l15x_clk.c",
        _SPL_SRC / "stm8l15x_lcd.c",
    ],
    # Add more mappings here if you enable other ST drivers, e.g. adc/gpio.
}

# ---------------------------------------------------------------- SSE broker
_sse_lock = threading.Lock()
_sse_clients = []


def sse_send(kind, msg, **extra):
    payload = {"type": kind, "msg": msg}
    payload.update(extra)
    line = "data: " + json.dumps(payload, ensure_ascii=False) + "\n\n"
    with _sse_lock:
        for q in list(_sse_clients):
            try:
                q.put_nowait(line)
            except queue.Full:
                pass
    print(f"[{kind}] {msg}")


# ---------------------------------------------------------------- ihx -> bin
def ihx_to_bin(ihx_path, base=APP_BASE):
    records = []
    max_addr = 0
    with open(ihx_path, "r") as fh:
        for raw in fh:
            line = raw.strip()
            if not line or not line.startswith(":"):
                continue
            rec_len = int(line[1:3], 16)
            addr = int(line[3:7], 16)
            rec_type = int(line[7:9], 16)
            data = bytes.fromhex(line[9:9 + rec_len * 2])
            if rec_type == 0x00:
                records.append((addr, data))
                if addr + len(data) > max_addr:
                    max_addr = addr + len(data)
            elif rec_type == 0x01:
                break
    if max_addr <= base:
        raise RuntimeError(f"IHX has no data at or above 0x{base:04X}")
    img = bytearray(max_addr - base)
    skipped = 0
    for addr, data in records:
        if addr < base:
            skipped += len(data)
            continue
        off = addr - base
        img[off:off + len(data)] = data
    return bytes(img), skipped


def post_bin_to_esp32(bin_bytes, filename="app.bin"):
    boundary = "----STM8FOTA" + uuid.uuid4().hex
    lines = []
    lines.append(("--" + boundary).encode())
    lines.append(
        f'Content-Disposition: form-data; name="firmware"; filename="{filename}"'
        .encode())
    lines.append(b"Content-Type: application/octet-stream")
    lines.append(b"")
    lines.append(bin_bytes)
    lines.append(("--" + boundary + "--").encode())
    lines.append(b"")
    body = b"\r\n".join(lines)
    req = urequest.Request(
        ESP32_UPLOAD_URL, data=body,
        headers={"Content-Type": "multipart/form-data; boundary=" + boundary,
                 "Content-Length": str(len(body))},
        method="POST")
    with urequest.urlopen(req, timeout=15) as resp:
        return resp.status, resp.read().decode(errors="replace")


# ---------------------------------------------------------------- compile helper
def _copy_libs_into(tmp, src_text):
    """If the user code #includes a bundled header, copy the pair into tmp.
    Recursively handles dependencies (e.g., stm8_lcd.h depends on stm8l15x.h)."""
    extra = []
    inc_flags = []
    copied_headers = set()
    
    def scan_and_copy(text, depth=0):
        """Recursively scan for includes and copy dependencies."""
        if depth > 10:  # Prevent infinite recursion
            return
        low = text.lower()
        for header, csrc in LIBRARIES.items():
            if f'"{header.lower()}"' in low or f'<{header.lower()}>' in low:
                if header in copied_headers:
                    continue  # Already copied
                lh = LIB_DIR / header
                if lh.exists():
                    # Copy header file
                    (tmp / header).write_bytes(lh.read_bytes())
                    copied_headers.add(header)
                    # If there's a corresponding .c file, copy it and add to extra
                    if csrc:
                        lc = LIB_DIR / csrc
                        if lc.exists():
                            (tmp / csrc).write_bytes(lc.read_bytes())
                            extra.append(str(tmp / csrc))
                    # Recursively scan the copied header for more dependencies
                    header_content = lh.read_text(encoding='utf-8', errors='ignore')
                    scan_and_copy(header_content, depth + 1)
    
    # Start with user source
    scan_and_copy(src_text)

    # Always set include flags if we copied any headers
    if copied_headers:
        inc_flags = ["-I", str(tmp)]

    # --- Additionally, check for ST SPL headers -------------------------
    # SPL sources are NOT copied into tmp — they live in webcompile/stm8l-...
    # We just add the include paths and list the .c files directly.
    low = src_text.lower()
    spl_added = False
    for hdr, sources in SPL_HEADER_MAP.items():
        if f'"{hdr.lower()}"' in low or f'<{hdr.lower()}>' in low:
            for src in sources:
                if src.exists():
                    extra.append(str(src))
                    spl_added = True
            if spl_added and not any('-I' in f for f in inc_flags):
                inc_flags = ["-I", str(tmp)]
            for inc in SPL_INC:
                if inc.exists():
                    inc_flags += ["-I", str(inc)]
    return extra, inc_flags


def _run_sdcc(src_path, ihx_path, extra_sources, include_flags):
    """Two-stage build: compile each .c to .rel, then link .rel files to .ihx.
    SDCC cannot accept more than one .c file per invocation."""
    tmp_dir = Path(src_path).parent
    rel_files = []

    all_sources = [str(src_path)] + list(extra_sources)
    # Are we compiling SPL sources? If yes, SPL requires two build defines:
    #   USE_STDPERIPH_DRIVER  (enable prototypes)
    #   STM8L15X_MD           (target: STM8L152C6T6 is medium-density)
    spl_defines = []
    if any("stm8l15x_" in Path(s).name or "stm8l_discovery" in Path(s).name
           for s in extra_sources):
        # -D_SDCC_ forces SPL's stm8l15x.h into the SDCC branch of the
        # patched compiler-detection block.  SDCC does not predefine
        # __SDCC in a way that survives an  #elif defined(...)  chain.
        spl_defines = [
            "-DUSE_STDPERIPH_DRIVER=1",
            "-DSTM8L15X_MD=1",
            "-D_SDCC_=1",
        ]

    for c_file in all_sources:
        c_path = Path(c_file)
        rel_path = tmp_dir / (c_path.stem + ".rel")
        # SPL files trigger SDCC optimizer bugs.  For those we turn OFF as
        # many optimization passes as we can.  User code stays default.
        is_spl_file = ("stm8l15x_" in c_path.name
                       or "stm8l_discovery" in c_path.name)
        extra_opts = []
        if is_spl_file:
            extra_opts = ["--nolospre", "--nostdlibcall",
                          "--max-allocs-per-node", "1"]
        cmd = [
            SDCC, "-mstm8", "-c",
            "--iram-size", "2048",
            "--nogcse", "--noinvariant", "--noinduction",
            *extra_opts,
            *spl_defines,
            *include_flags,
            c_file,
            "-o", str(rel_path),
        ]
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        if r.returncode != 0 or not rel_path.exists():
            # Return the compile failure directly so the caller can show stderr.
            return r
        rel_files.append(str(rel_path))

    # Link stage: SDCC accepts multiple .rel and produces .ihx.
    cmd = [
        SDCC, "-mstm8", "--out-fmt-ihx",
        "--code-loc", f"0x{APP_BASE:04X}",
        "--iram-size", "2048",
        "--stack-loc", "0x07FF",
        *rel_files,
        "-o", str(ihx_path),
    ]
    return subprocess.run(cmd, capture_output=True, text=True, timeout=30)


# ---------------------------------------------------------------- full pipeline (SSE)
def compile_and_flash(src_text, filename_hint="user.c"):
    try:
        sse_send("info", f"Nhan {len(src_text)} byte source", pct=2)
        with tempfile.TemporaryDirectory(prefix="fota_") as tmp:
            tmp = Path(tmp)
            src_path = tmp / "user.c"
            ihx_path = tmp / "user.ihx"
            src_path.write_text(src_text, encoding="utf-8")

            extra_sources, include_flags = _copy_libs_into(tmp, src_text)
            for f in extra_sources:
                sse_send("dim", f"+ thu vien {Path(f).name}")

            sse_send("info",
                     f"Bien dich SDCC ({1+len(extra_sources)} file)...", pct=15)
            try:
                r = _run_sdcc(src_path, ihx_path, extra_sources, include_flags)
            except FileNotFoundError:
                sse_send("err", f"'{SDCC}' khong co tren PATH",
                         done=True, success=False)
                return

            if r.stdout.strip():
                sse_send("dim", r.stdout.strip())
            for ln in r.stderr.splitlines():
                ln = ln.rstrip()
                if not ln:
                    continue
                sse_send("err" if "error" in ln.lower() else "warn", ln)
            if r.returncode != 0 or not ihx_path.exists():
                sse_send("err", f"SDCC that bai (return {r.returncode}).",
                         done=True, success=False)
                return

            sse_send("ok", "Bien dich thanh cong.", pct=45)

            try:
                bin_bytes, skipped = ihx_to_bin(ihx_path, APP_BASE)
            except Exception as e:
                sse_send("err", f"Loi ihx2bin: {e}",
                         done=True, success=False)
                return
            if skipped:
                sse_send("warn", f"Bo qua {skipped} byte duoi 0x{APP_BASE:04X}")
            if not bin_bytes:
                sse_send("err", "Ket qua rong.", done=True, success=False)
                return
            if len(bin_bytes) > APP_MAX:
                sse_send("err",
                         f".bin qua lon: {len(bin_bytes)} > {APP_MAX} byte",
                         done=True, success=False)
                return
            sse_send("ok", f"Da co .bin ({len(bin_bytes)} byte).", pct=65)

            sse_send("info", f"Gui .bin sang ESP32 ({ESP32_UPLOAD_URL})...",
                     pct=75)
            try:
                status, body = post_bin_to_esp32(bin_bytes,
                                                 filename=filename_hint)
            except uerror.URLError as e:
                sse_send("err",
                         f"Khong ket noi duoc ESP32: {e}. "
                         "Kiem tra WiFi 'FOTA-Gateway'.",
                         done=True, success=False)
                return
            except Exception as e:
                sse_send("err", f"Loi mang: {e}",
                         done=True, success=False)
                return
            if status != 200:
                sse_send("err", f"ESP32 tra ve HTTP {status}: {body!r}",
                         done=True, success=False)
                return

            sse_send("ok",
                     "ESP32 nhan file. Dang nap firmware xuong STM8...",
                     pct=88)
            time.sleep(0.5)
            sse_send("ok",
                     "🎉 Hoan tat pipeline. Xem LED tren STM8 de xac nhan.",
                     pct=100, done=True, success=True)
    except Exception as e:
        sse_send("err", f"Loi ngoai y: {e}", done=True, success=False)


# ---------------------------------------------------------------- multipart parser
def parse_multipart(body, boundary):
    boundary_bytes = ("--" + boundary).encode()
    parts = body.split(boundary_bytes)
    out = {}
    for part in parts:
        part = part.strip(b"\r\n-")
        if not part or part == b"--":
            continue
        try:
            head, data = part.split(b"\r\n\r\n", 1)
        except ValueError:
            continue
        data = data.rstrip(b"\r\n")
        head_txt = head.decode("utf-8", errors="replace")
        name = None
        for line in head_txt.split("\r\n"):
            if line.lower().startswith("content-disposition"):
                for tok in line.split(";"):
                    tok = tok.strip()
                    if tok.startswith("name="):
                        name = tok[5:].strip('"')
        if name:
            out[name] = data
    return out


# ---------------------------------------------------------------- HTTP handler
STATIC_MIME = {
    ".html": "text/html; charset=utf-8",
    ".css":  "text/css; charset=utf-8",
    ".js":   "application/javascript; charset=utf-8",
    ".c":    "text/plain; charset=utf-8",
    ".ihx":  "text/plain; charset=utf-8",
    ".bin":  "application/octet-stream",
}


class Handler(BaseHTTPRequestHandler):
    server_version = "STM8FotaGateway/1.0"

    def log_message(self, fmt, *args):
        pass

    def _cors_headers(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods",
                         "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers",
                         "Content-Type, X-Requested-With")
        self.send_header("Access-Control-Max-Age", "3600")

    def do_OPTIONS(self):
        self.send_response(204)
        self._cors_headers()
        self.end_headers()

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path in ("/", "/index.html"):
            return self._serve_static("index.html")
        if path in ("/style.css", "/script.js"):
            return self._serve_static(path.lstrip("/"))
        if path == "/events":
            return self._sse()
        if path == "/status":
            return self._status()
        self.send_error(404, "Not found: " + path)

    def do_POST(self):
        if self.path == "/compile":
            return self._compile()
        if self.path == "/compile-only":
            return self._compile_only()
        self.send_error(404)

    def _serve_static(self, rel):
        p = STATIC_DIR / rel
        if not p.exists() or not p.is_file():
            self.send_error(404, f"{rel} not found")
            return
        data = p.read_bytes()
        mime = STATIC_MIME.get(p.suffix.lower(), "application/octet-stream")
        self.send_response(200)
        self._cors_headers()
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def _status(self):
        info = {
            "sdcc_found": shutil.which(SDCC) is not None,
            "makebin_found": shutil.which(MAKEBIN) is not None,
            "esp32_url": ESP32_UPLOAD_URL,
            "app_base": f"0x{APP_BASE:04X}",
            "app_max":  APP_MAX,
        }
        body = json.dumps(info).encode()
        self.send_response(200)
        self._cors_headers()
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _sse(self):
        self.send_response(200)
        self._cors_headers()
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        self.end_headers()
        q = queue.Queue(maxsize=200)
        with _sse_lock:
            _sse_clients.append(q)
        try:
            self.wfile.write(b'data: {"type":"info","msg":"SSE connected"}\n\n')
            self.wfile.flush()
            while True:
                try:
                    line = q.get(timeout=15)
                    self.wfile.write(line.encode("utf-8"))
                    self.wfile.flush()
                except queue.Empty:
                    try:
                        self.wfile.write(b": ping\n\n")
                        self.wfile.flush()
                    except Exception:
                        break
                except Exception:
                    break
        finally:
            with _sse_lock:
                if q in _sse_clients:
                    _sse_clients.remove(q)

    def _read_source(self):
        length = int(self.headers.get("Content-Length", "0"))
        if length <= 0 or length > 512 * 1024:
            return None
        raw = self.rfile.read(length)
        ctype = self.headers.get("Content-Type", "")
        if "multipart/form-data" in ctype and "boundary=" in ctype:
            boundary = ctype.split("boundary=", 1)[1].strip()
            fields = parse_multipart(raw, boundary)
            return (fields.get("source") or fields.get("file") or b"").decode(
                "utf-8", errors="replace")
        if "application/json" in ctype:
            try:
                return json.loads(raw.decode()).get("source", "")
            except Exception:
                return None
        return raw.decode("utf-8", errors="replace")

    def _compile(self):
        src_text = self._read_source()
        if not src_text or not src_text.strip():
            self.send_error(400, "Empty or invalid source")
            return
        threading.Thread(target=compile_and_flash,
                         args=(src_text, "user.c"), daemon=True).start()
        body = b'{"accepted":true}'
        self.send_response(202)
        self._cors_headers()
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _compile_only(self):
        src_text = self._read_source()
        if not src_text or not src_text.strip():
            return self._json_error(400, "Empty source")
        with tempfile.TemporaryDirectory(prefix="fota_") as tmp:
            tmp = Path(tmp)
            src_path = tmp / "user.c"
            ihx_path = tmp / "user.ihx"
            src_path.write_text(src_text, encoding="utf-8")
            extra_sources, include_flags = _copy_libs_into(tmp, src_text)
            try:
                r = _run_sdcc(src_path, ihx_path, extra_sources, include_flags)
            except FileNotFoundError:
                return self._json_error(500, f"'{SDCC}' not on PATH")
            except subprocess.TimeoutExpired:
                return self._json_error(500, "SDCC timeout")
            if r.returncode != 0 or not ihx_path.exists():
                return self._json_error(400, "SDCC failed",
                                        stdout=r.stdout, stderr=r.stderr,
                                        code=r.returncode)
            try:
                bin_bytes, _ = ihx_to_bin(ihx_path, APP_BASE)
            except Exception as e:
                return self._json_error(500, f"ihx2bin: {e}")
            if not bin_bytes:
                return self._json_error(400, "Empty binary")
            if len(bin_bytes) > APP_MAX:
                return self._json_error(400,
                    f"Too big: {len(bin_bytes)} > {APP_MAX}")
            self.send_response(200)
            self._cors_headers()
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(bin_bytes)))
            self.send_header("X-App-Base", f"0x{APP_BASE:04X}")
            self.send_header("X-App-Size", str(len(bin_bytes)))
            if r.stderr.strip():
                self.send_header("X-Sdcc-Warnings",
                                 r.stderr.replace("\n", " | ")[:250])
            self.end_headers()
            self.wfile.write(bin_bytes)

    def _json_error(self, status, msg, **extra):
        body = json.dumps({"error": msg, **extra},
                          ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self._cors_headers()
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


# ---------------------------------------------------------------- main
def main():
    if not shutil.which(SDCC):
        print(f"WARNING: '{SDCC}' not on PATH. Compile will fail.",
              file=sys.stderr)

    try:
        srv = ThreadingHTTPServer(("0.0.0.0", LISTEN_PORT), Handler)
    except (PermissionError, OSError) as e:
        print(f"ERROR: cannot bind port {LISTEN_PORT}: {e}", file=sys.stderr)
        print(f"       On Windows try another port, e.g.  python server.py 8765",
              file=sys.stderr)
        print(f"       Or check reserved ranges with:",
              file=sys.stderr)
        print(f"         netsh interface ipv4 show excludedportrange protocol=tcp",
              file=sys.stderr)
        return
    print(f"====  STM8 FOTA compile server  ====")
    print(f"  Serving   {STATIC_DIR}")
    print(f"  Open      http://localhost:{LISTEN_PORT}")
    print(f"  Forwards  .bin  ->  {ESP32_UPLOAD_URL}")
    print(f"  PC WiFi must be on  FOTA-Gateway  (pass 12345678)")
    print(f"====================================")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")


if __name__ == "__main__":
    main()
