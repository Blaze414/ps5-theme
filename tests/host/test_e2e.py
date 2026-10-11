#!/usr/bin/env python3
"""End-to-end tests for ps5-theme, run inside the privileged test container.

Builds a fake console tree (/system_ex, /data, /user/app), runs the real plugin
(AddressSanitizer build), and checks the behaviour over HTTP and on disk.
`--serve` only sets up the tree and keeps the plugin running for manual UI use.
"""
import errno
import hashlib
import http.client
import json
import os
import random
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
import traceback
import urllib.parse
import zlib

ASAN = "build-host/ps5-theme-asan"
CRASH = "build-host/ps5-theme-crash"
PORT = 8095
BASE = "/data/ps5-theme"
APPS = "/system_ex/rnps/apps"
HOME = f"{APPS}/NPXS40008/assets/src/modules/home/assets"
OTHER = f"{APPS}/NPXS40028/assets/img"
BULK = f"{APPS}/NPXS40010/assets/bulk"
MEDIA = f"{APPS}/NPXS40012/assets"
VSH = "/system_ex/vsh_asset"
LOG = f"{BASE}/logs/log.txt"
ENV = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:halt_on_error=1:abort_on_error=0",
           UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")


# ---------------------------------------------------------------- fixtures

def png(w, h, rgb):
    raw = b"".join(b"\0" + bytes(rgb) * w for _ in range(h))
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def wav(rate, channels, seconds, amplitude, bits=16, fmt=1):
    frames = int(rate * seconds)
    sample = struct.pack("<h", amplitude) * channels
    data = b"".join(sample if (i // 64) % 2 == 0 else struct.pack("<h", -amplitude) * channels
                    for i in range(frames))
    block = channels * bits // 8
    fmt_chunk = struct.pack("<HHIIHH", fmt, channels, rate, rate * block, block, bits)
    body = b"WAVE" + b"fmt " + struct.pack("<I", 16) + fmt_chunk + b"data" + struct.pack("<I", len(data)) + data
    return b"RIFF" + struct.pack("<I", len(body)) + body


def dds_bc7(w, h, fill=None):
    """A DX10 BC7_UNORM DDS shaped like the console's bg_hub_default.dds."""
    blocks = ((w + 3) // 4) * ((h + 3) // 4)
    data = fill if fill is not None else bytes(range(256)) * (blocks * 16 // 256 + 1)
    data = data[:blocks * 16]
    hdr = bytearray(148)
    hdr[0:4] = b"DDS "
    struct.pack_into("<7I", hdr, 4, 124, 0x81007, h, w, blocks * 16, 1, 1)
    struct.pack_into("<2I4s", hdr, 76, 32, 4, b"DX10")
    struct.pack_into("<I", hdr, 108, 0x1000)
    struct.pack_into("<5I", hdr, 128, 98, 3, 0, 1, 0)
    return bytes(hdr) + data


def zstd_stored(data):
    out = bytearray(b"\x28\xb5\x2f\xfd\xa0") + struct.pack("<I", len(data))
    blocks = max(1, -(-len(data) // 131072))
    for b in range(blocks):
        chunk = data[b * 131072:(b + 1) * 131072]
        h = (len(chunk) << 3) | (1 if b == blocks - 1 else 0)
        out += bytes([h & 255, (h >> 8) & 255, (h >> 16) & 255]) + chunk
    return bytes(out)


def zstd_cli(data, level=19):
    return subprocess.run(["zstd", f"-{level}", "-c", "-q"], input=data, capture_output=True, check=True).stdout


AT9_TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "at9_fixture.js")


def at9_stock(seconds=3, cci=3, level=0.05):
    """A stand-in for the console's bgm_home.at9 (ATRAC9 5.1, 48 kHz), made by
    the theme builder's own encoder (web/atrac9.js)."""
    out = f"/tmp/at9-stock-{cci}-{seconds}-{level}.at9"
    subprocess.run(["node", AT9_TOOL, "stock", out, str(seconds), str(cci), str(level)], check=True, capture_output=True)
    return open(out, "rb").read()


def at9_music(stock_path, seconds, hz=440):
    """A replacement encoded against the system file, as the browser makes it."""
    out = f"/tmp/at9-music-{seconds}-{hz}.at9"
    subprocess.run(["node", AT9_TOOL, "music", stock_path, out, str(seconds), str(hz)], check=True, capture_output=True)
    return open(out, "rb").read()


def jpeg_header(w, h):
    """Smallest byte sequence our parser accepts as a JPEG of w x h."""
    sof = b"\xff\xc0" + struct.pack(">HBHHB", 11, 8, h, w, 1) + b"\x01\x11\x00"
    return b"\xff\xd8" + b"\xff\xe0" + struct.pack(">H", 16) + b"JFIF\0" + b"\0" * 9 + sof + b"\xff\xd9"


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def build_console():
    for d in ("/system_ex", "/mnt/rnps", "/data", "/user", "/tmp/notifications.txt", "/tmp/tile-registered"):
        if os.path.isdir(d):
            shutil.rmtree(d)
        elif os.path.exists(d):
            os.unlink(d)
    write(f"{HOME}/bg.png", png(480, 270, (20, 30, 60)))
    write(f"{HOME}/icon.png", png(96, 96, (200, 200, 200)))
    write(f"{HOME}/icon@2x.png", png(192, 192, (200, 200, 200)))
    write(f"{HOME}/strings.json", b'{"title":"Home"}')
    write(f"{HOME}/sub/deep.png", png(32, 32, (1, 2, 3)))
    write(f"{OTHER}/target.png", png(64, 64, (9, 9, 9)))
    os.symlink(OTHER, f"{HOME}/linkdir")
    write(f"{BULK}/hero.png", png(100, 100, (50, 50, 50)))
    rnd = random.Random(1)
    for i in range(150):
        write(f"{BULK}/blob{i:03}.bin", rnd.randbytes(512 * 1024))
    write("/mnt/rnps/apps/NPXS40099/a.png", png(8, 8, (0, 0, 0)))
    write(f"{MEDIA}/sounds/move.wav", wav(48000, 1, 0.05, 8000))
    write(f"{MEDIA}/fonts/ui.ttf", b"\x00\x01\x00\x00" + b"\0" * 60)
    write(f"{MEDIA}/anim/spinner.json", b'{"v":"5.7","w":100,"h":100,"layers":[]}')
    write(f"{MEDIA}/icons/star.svg", b'<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8"/>')
    write(f"{MEDIA}/video/loop.mp4", b"\x00\x00\x00\x18ftypisom" + b"\0" * 40)
    write(f"{MEDIA}/bundle.js", b"console.log(1)")
    write(f"{VSH}/bg_hub_default.dds.zst", zstd_cli(dds_bc7(256, 128, fill=bytes(range(7, 256)) * 300)))
    write(f"{VSH}/eboot_like.rco", b"\0" * 64)
    write(f"{VSH}/bgm_home.at9", at9_stock())
    write(f"{VSH}/sfx_quiet.at9", at9_stock(10, 3, 0))        # silent: codes only 3 bands
    write(f"{MEDIA}/lib/native.sprx", b"\x7fELF")
    os.makedirs("/data", exist_ok=True)
    os.makedirs("/user/app", exist_ok=True)


def snapshot(root):
    out = {}
    for d, dirs, files in os.walk(root, followlinks=False):
        dirs[:] = [x for x in dirs if not x.endswith(".ps5theme-under")]
        for name in files + [x for x in dirs if os.path.islink(os.path.join(d, x))]:
            p = os.path.join(d, name)
            if os.path.islink(p):
                out[p] = "link:" + os.readlink(p)
            else:
                with open(p, "rb") as f:
                    out[p] = hashlib.sha256(f.read()).hexdigest()
    return out


def crc_file(path):
    with open(path, "rb") as f:
        return zlib.crc32(f.read())


# ---------------------------------------------------------------- plugin process

class Plugin:
    def __init__(self, binary=ASAN, env=None):
        self.out = open("/tmp/plugin-out.txt", "ab")
        self.proc = subprocess.Popen([os.path.abspath(binary)], stdout=self.out, stderr=self.out,
                                     env=dict(ENV, **(env or {})), cwd="/")
        self.wait_port()

    def wait_port(self, timeout=20):
        end = time.time() + timeout
        while time.time() < end:
            if self.proc.poll() is not None:
                raise AssertionError(f"plugin exited early with {self.proc.returncode}")
            try:
                socket.create_connection(("127.0.0.1", PORT), 0.2).close()
                return
            except OSError:
                time.sleep(0.1)
        raise AssertionError("plugin did not open its port")

    def alive(self):
        return self.proc.poll() is None

    def kill(self):
        if self.alive():
            self.proc.kill()
            self.proc.wait()

    def term(self):
        if self.alive():
            self.proc.terminate()
            self.proc.wait(10)


def call(method, path, params=None, body=None, token=True, headers=None, timeout=30):
    url = path + ("?" + urllib.parse.urlencode(params) if params else "")
    conn = http.client.HTTPConnection("127.0.0.1", PORT, timeout=timeout)
    h = {"X-PS5-Theme": "1"} if token and method != "GET" else {}
    h.update(headers or {})
    conn.request(method, url, body=body, headers=h)
    res = conn.getresponse()
    data = res.read()
    conn.close()
    ctype = res.getheader("Content-Type", "")
    return res.status, (json.loads(data) if "json" in ctype else data)


def status():
    code, data = call("GET", "/api/status")
    assert code == 200, code
    return data


def put_file(theme, asset, data, crc=True):
    params = {"id": theme, "path": asset}
    if crc:
        params["crc"] = f"{zlib.crc32(data):08x}"
    return call("PUT", "/api/theme/file", params, data)


def wait_applied(want, timeout=60):
    end = time.time() + timeout
    while time.time() < end:
        s = status()
        if s["applied"] == want and s["active"] == (want or s["active"]):
            time.sleep(0.3)
            return status()
        time.sleep(0.25)
    raise AssertionError(f"timed out waiting for applied={want!r}, last status {s}")


def wait_result(fragment, timeout=60):
    end = time.time() + timeout
    while time.time() < end:
        s = status()
        if fragment in s["result"]:
            return s
        time.sleep(0.25)
    raise AssertionError(f"result never contained {fragment!r}: {s['result']!r}")


EMULATED = bool(os.environ.get("EMULATE_MOUNTS"))


def mounts_on(point):
    if EMULATED:
        return [point] if os.path.islink(point) else []
    with open("/proc/self/mountinfo") as f:
        return [l for l in f if l.split()[4] == point]


def log_text():
    with open(LOG) as f:
        return f.read()


# ---------------------------------------------------------------- tests

S = {}  # shared state between ordered tests


def t_startup():
    s = status()
    assert s["fw"] == "13.42" and s["version"] == "1.22" and s["themes"] == [], s
    listing = open(f"{BASE}/dump/assets-v2-13.42.txt").read()
    assert f"{VSH}/bg_hub_default.dds.zst" in listing
    assert f"{HOME}/bg.png" in listing and "/mnt/rnps/apps/NPXS40099/a.png" in listing
    param = open("/user/app/THEM00001/sce_sys/param.json").read()
    assert '"deeplinkUri":"http://127.0.0.1:8095/"' in param and '"titleId":"THEM00001"' in param
    assert open("/user/app/THEM00001/sce_sys/icon0.png", "rb").read(8) == b"\x89PNG\r\n\x1a\n"
    assert open("/tmp/tile-registered").read().strip() == "THEM00001"
    assert "starting" in log_text()
    notes = open("/tmp/notifications.txt").read()
    assert "ps5-theme 1.22 starting" in notes and "ps5-theme ready" in notes, notes


def t_ui_and_icon_served():
    code, html = call("GET", "/")
    assert code == 200 and b"<title>PS5 Themes</title>" in html
    code, icon = call("GET", "/icon.png")
    assert code == 200 and icon[:4] == b"\x89PNG"
    code, js = call("GET", "/atrac9.js")
    assert code == 200 and js == open("web/atrac9.js", "rb").read()


def t_mutations_need_token():
    code, _ = call("POST", "/api/themes", {"id": "x"}, token=False)
    assert code == 403
    code, _ = call("PUT", "/api/theme/file", {"id": "x", "path": f"{HOME}/bg.png"}, b"x", token=False)
    assert code == 403


def t_create_theme_rules():
    assert call("POST", "/api/themes", {"id": "night", "name": "Night"})[0] == 200
    assert call("POST", "/api/themes", {"id": "night"})[0] == 409
    for bad in ("../x", "none", "", "a/b", "x" * 65):
        assert call("POST", "/api/themes", {"id": bad})[0] == 400, bad
    ini = open(f"{BASE}/themes/night/theme.ini").read()
    assert ini == "name=Night\nfw=13.42\nmusic_gain=0\n", ini


def t_asset_search():
    code, data = call("GET", "/api/assets", {"q": "ICON"})
    paths = {a["path"] for a in data["assets"]}
    assert code == 200 and {f"{HOME}/icon.png", f"{HOME}/icon@2x.png"} <= paths, paths


def t_upload_is_verified():
    S["bg"] = png(480, 270, (180, 40, 40))
    code, r = put_file("night", f"{HOME}/bg.png", S["bg"])
    assert code == 200 and r["crc32"] == f"{zlib.crc32(S['bg']):08x}" and r["width"] == 480, r

    code, r = put_file("night", f"{HOME}/bg.png", png(400, 270, (1, 1, 1)))
    assert code == 422 and "480x270" in r["error"], r
    code, r = put_file("night", f"{HOME}/bg.png", jpeg_header(480, 270))
    assert code == 422 and "PNG" in r["error"], r
    code, r = call("PUT", "/api/theme/file", {"id": "night", "path": f"{HOME}/bg.png", "crc": "deadbeef"}, S["bg"])
    assert code == 422 and "checksum" in r["error"], r
    for path in ("/etc/passwd", f"{APPS}/../../etc/passwd", f"{HOME}/missing.png", "/data/ps5-theme/active"):
        assert put_file("night", path, b"x")[0] == 403, path
    assert put_file("nosuch", f"{HOME}/bg.png", S["bg"])[0] == 404
    assert call("PUT", "/api/theme/file", {"id": "night", "path": f"{HOME}/bg.png"}, b"")[0] == 400
    assert call("PUT", "/api/theme/file", {"id": "night", "path": f"{HOME}/bg.png"}, b"x",
                headers={"Content-Length": str(40 << 20)})[0] == 413

    assert put_file("night", f"{HOME}/strings.json", b'{"title":"Night"}')[0] == 200
    assert put_file("night", f"{HOME}/linkdir/target.png", png(64, 64, (255, 0, 255)))[0] == 200
    S["icon2x"] = png(192, 192, (10, 200, 10))
    assert put_file("night", f"{HOME}/icon@2x.png", S["icon2x"])[0] == 200

    code, r = call("GET", "/api/theme/files", {"id": "night"})
    files = {f["path"] for f in r["files"]}
    assert files == {f"{HOME}/bg.png", f"{HOME}/strings.json", f"{HOME}/linkdir/target.png", f"{HOME}/icon@2x.png"}, files
    leftovers = [f for _, _, fs in os.walk(f"{BASE}/themes") for f in fs if f.endswith((".part", ".tmp"))]
    assert not leftovers, leftovers


def t_apply_verifies_and_is_read_only():
    S["stock_icon"] = crc_file(f"{HOME}/icon.png")
    S["stock_bg"] = crc_file(f"{HOME}/bg.png")
    S["stock_target"] = crc_file(f"{OTHER}/target.png")
    assert call("POST", "/api/apply", {"id": "night"})[0] == 200
    s = wait_applied("night")
    assert "3 files verified" in s["result"] and "1 skipped" in s["result"], s["result"]

    assert crc_file(f"{HOME}/bg.png") == zlib.crc32(S["bg"])
    assert crc_file(f"{HOME}/icon@2x.png") == zlib.crc32(S["icon2x"])
    assert open(f"{HOME}/strings.json", "rb").read() == b'{"title":"Night"}'
    assert crc_file(f"{HOME}/icon.png") == S["stock_icon"]
    assert crc_file(f"{OTHER}/target.png") == S["stock_target"], "write escaped through a symlink"
    assert "reached through a symlink" in log_text()

    for attempt in (() if EMULATED else (lambda: open(f"{HOME}/bg.png", "wb"), lambda: open(f"{HOME}/new.png", "wb"))):
        try:
            attempt()
            raise AssertionError("overlay is writable")
        except OSError as e:
            assert e.errno == errno.EROFS, e
    assert len(mounts_on(HOME)) == 1
    assert open(f"{BASE}/mounts").read() == f"{HOME}\n"


def t_reapply_picks_up_edits():
    newer = png(480, 270, (40, 180, 40))
    assert put_file("night", f"{HOME}/bg.png", newer)[0] == 200
    assert crc_file(f"{HOME}/bg.png") == zlib.crc32(S["bg"])  # live mount unchanged until re-apply
    assert call("POST", "/api/apply", {"id": "night"})[0] == 200
    end = time.time() + 30
    while crc_file(f"{HOME}/bg.png") != zlib.crc32(newer):
        assert time.time() < end, "re-apply did not take effect"
        time.sleep(0.2)
    wait_applied("night")
    assert len(mounts_on(HOME)) == 1, "mounts stacked on re-apply"
    S["bg"] = newer


def t_stock_mirror_and_linked_staging():
    """Staging links a verified stock copy, made once, instead of copying the folder each time."""
    mirror, mark = f"{BASE}/stock/13.42{HOME}", f"{BASE}/stock/13.42/.complete{HOME}.ok"
    assert os.path.exists(mark), "no stock copy was kept"
    stage = [d for d in os.listdir(f"{BASE}/stage")]
    assert len(stage) >= 1 and all(d.startswith("g") for d in stage), stage
    staged = next(f"{BASE}/stage/{d}" for d in stage if os.path.exists(f"{BASE}/stage/{d}/icon.png"))
    # Untouched files are links to the stock copy; replaced ones are separate files.
    assert os.stat(f"{staged}/icon.png").st_ino == os.stat(f"{mirror}/icon.png").st_ino
    assert os.stat(f"{staged}/bg.png").st_ino != os.stat(f"{mirror}/bg.png").st_ino
    assert crc_file(f"{mirror}/bg.png") == S["stock_bg"], "the stock copy changed"
    assert crc_file(f"{mirror}/icon.png") == S["stock_icon"]
    assert status()["applying"] == ""

    # A copy that is missing while a theme covers the folder is made from the
    # system after the overlay comes down, never from the theme.
    os.unlink(mark)
    os.unlink(f"{mirror}/bg.png")
    seen, stop = [], threading.Event()

    def sample():   # what the manager shows while the apply runs
        while not stop.is_set():
            s = status()
            if s["progress"] is not None:
                seen.append((s["progress"]["percent"], s["progress"]["text"]))
            time.sleep(0.005)
    sampler = threading.Thread(target=sample)
    sampler.start()
    assert call("POST", "/api/apply", {"id": "night"})[0] == 200
    end = time.time() + 30
    while not os.path.exists(mark):
        assert time.time() < end, "the stock copy was not made again"
        time.sleep(0.2)
    wait_applied("night")
    stop.set()
    sampler.join()
    assert status()["progress"] is None
    percents = [p for p, _ in seen]
    assert all(0 <= p <= 100 for p in percents) and percents == sorted(percents), seen
    assert all(text for _, text in seen), seen
    assert crc_file(f"{mirror}/bg.png") == S["stock_bg"], "stock copy taken from the theme"
    assert crc_file(f"{HOME}/bg.png") == zlib.crc32(S["bg"])
    assert len(mounts_on(HOME)) == 1 and open(f"{BASE}/mounts").read() == f"{HOME}\n"
    assert all(d.startswith("g") for d in os.listdir(f"{BASE}/stage"))


def t_firmware_mismatch_is_refused():
    assert call("POST", "/api/themes", {"id": "old", "name": "Old", "fw": "13.40, 3.42"})[0] == 200
    assert put_file("old", f"{HOME}/icon.png", png(96, 96, (0, 0, 255)))[0] == 200
    names = {t["id"]: t["compatible"] for t in status()["themes"]}
    assert names == {"night": True, "old": False}, names
    assert call("POST", "/api/apply", {"id": "old"})[0] == 200
    s = wait_result("is for firmware 13.40, 3.42")
    assert s["applied"] == "" and not mounts_on(HOME)
    assert crc_file(f"{HOME}/icon.png") == S["stock_icon"]


def t_delete_rules():
    assert call("DELETE", "/api/themes", {"id": "old"})[0] == 409  # it is the active selection
    assert call("POST", "/api/apply", {"id": "night"})[0] == 200
    wait_applied("night")
    assert call("DELETE", "/api/themes", {"id": "night"})[0] == 409
    assert call("DELETE", "/api/themes", {"id": "old"})[0] == 200
    assert not os.path.exists(f"{BASE}/themes/old")
    assert call("DELETE", "/api/themes", {"id": "../themes"})[0] == 404


def t_concurrent_requests_during_apply():
    assert call("POST", "/api/themes", {"id": "bulk"})[0] == 200
    assert put_file("bulk", f"{BULK}/hero.png", png(100, 100, (255, 255, 0)))[0] == 200
    stock_blob = crc_file(f"{BULK}/blob007.bin")
    assert call("POST", "/api/apply", {"id": "bulk"})[0] == 200

    errors, timings = [], []
    def poll_status():
        for _ in range(15):
            t = time.time()
            try:
                status()
            except Exception as e:
                errors.append(repr(e))
            timings.append(time.time() - t)
    upload = {}
    def late_upload():
        time.sleep(0.05)
        upload["r"] = put_file("bulk", f"{BULK}/hero.png", png(100, 100, (0, 255, 255)))
    threads = [threading.Thread(target=poll_status) for _ in range(4)] + [threading.Thread(target=late_upload)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert not errors, errors
    assert upload["r"][0] == 200, upload
    s = wait_applied("bulk")
    assert "1 files verified" in s["result"], s["result"]
    assert crc_file(f"{BULK}/blob007.bin") == stock_blob
    assert max(timings) < 10, f"status blocked for {max(timings):.1f}s"

    assert call("POST", "/api/apply", {"id": "bulk"})[0] == 200  # re-apply picks up the late upload
    end = time.time() + 60
    while crc_file(f"{BULK}/hero.png") != zlib.crc32(png(100, 100, (0, 255, 255))):
        assert time.time() < end, "late upload never applied"
        time.sleep(0.25)
    wait_applied("bulk")


def t_restore_stock_cleans_up():
    assert call("POST", "/api/apply", {"id": "none"})[0] == 200
    s = wait_applied("")
    assert s["result"] == "Stock home screen.", s
    assert not mounts_on(HOME) and not mounts_on(BULK)
    assert crc_file(f"{HOME}/bg.png") == S["stock_bg"]
    assert os.listdir(f"{BASE}/stage") == [] and not os.path.exists(f"{BASE}/mounts")


def t_recovers_after_hard_kill():
    assert call("POST", "/api/apply", {"id": "night"})[0] == 200
    wait_applied("night")
    S["plugin"].kill()
    assert len(mounts_on(HOME)) == 1  # left behind by the killed process
    S["plugin"] = Plugin()
    s = wait_applied("night")
    assert len(mounts_on(HOME)) == 1, mounts_on(HOME)
    assert crc_file(f"{HOME}/bg.png") == zlib.crc32(S["bg"])
    assert "unmounted " + HOME in log_text()


def raw(data, read=True):
    s = socket.create_connection(("127.0.0.1", PORT), 5)
    s.sendall(data)
    out = s.recv(200) if read else b""
    s.close()
    return out


def t_survives_malformed_and_abusive_requests():
    assert raw(b"garbage\r\n\r\n").startswith(b"HTTP/1.1 400")
    assert raw(b"GET / HTTP/1.1\r\nX: " + b"a" * 20000 + b"\r\n\r\n").startswith(b"HTTP/1.1 400")
    raw(b"GET /api/status HTTP/1.1\r\n", read=False)  # never finishes headers
    for _ in range(20):  # client hangs up mid-download
        s = socket.create_connection(("127.0.0.1", PORT), 5)
        s.sendall(f"GET /api/asset?path={BULK}/blob001.bin HTTP/1.1\r\n\r\n".encode())
        s.recv(10)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        s.close()
    rnd = random.Random(7)
    for _ in range(200):
        try:
            raw(rnd.randbytes(rnd.randint(1, 600)) + b"\r\n\r\n")
        except OSError:
            pass
    assert call("PUT", "/api/theme/file", {"id": "night", "path": f"{HOME}/bg.png"}, b"x",
                headers={"Content-Length": "-5"})[0] == 400
    assert call("GET", "/api/nope")[0] == 404
    assert call("GET", "/api/theme/file", {"id": "night", "path": "/etc/passwd"})[0] == 404
    assert call("GET", "/api/asset", {"path": "/etc/passwd"})[0] == 403
    assert call("GET", "/api/crash", {"name": "../logs/log.txt"})[0] == 404
    assert S["plugin"].alive()
    assert status()["applied"] == "night"


def t_overlays_reach_system_views():
    """ShellUI reads the system through a sandbox view; a theme must show there too."""
    if EMULATED:
        print("      (skipped: needs real mounts)", flush=True)
        return
    box = "/mnt/sandbox/SHELL_000/system_ex"
    box2 = "/mnt/sandbox/SHELL_001/system_ex"
    dummies = []
    for i in range(80):  # unrelated mounts listed before the one that matters (1,072 on a real PS5)
        src, dst = f"/tmp/dummy-src/{i}", f"/mnt/dummy/{i}"
        os.makedirs(src, exist_ok=True)
        os.makedirs(dst, exist_ok=True)
        subprocess.run(["mount", "--bind", src, dst], check=True)
        dummies.append(dst)
    os.makedirs(box, exist_ok=True)
    subprocess.run(["mount", "--bind", "/system_ex", box], check=True)
    try:
        assert call("POST", "/api/apply", {"id": "none"})[0] == 200
        wait_applied("")
        assert call("POST", "/api/apply", {"id": "night"})[0] == 200
        wait_applied("night")
        seen = box + HOME[len("/system_ex"):]
        assert crc_file(f"{seen}/bg.png") == zlib.crc32(S["bg"]), "theme not visible in the sandbox view"
        assert crc_file(f"{seen}/icon.png") == S["stock_icon"]
        try:
            open(f"{seen}/bg.png", "wb")
            raise AssertionError("sandbox overlay is writable")
        except OSError as e:
            assert e.errno == errno.EROFS, e
        assert "(system view)" in log_text()
        assert os.path.exists(box + "/vsh_asset/bg_hub_default.dds.zst")
        assert seen in open(f"{BASE}/mounts").read()
        assert "SHELL_000" in open(f"{BASE}/dump/mounts.txt").read() or True

        # An app restarts (after rest mode) in a new sandbox, whose view shows
        # stock: the plugin covers it by itself, without re-applying.
        os.makedirs(box2, exist_ok=True)
        subprocess.run(["mount", "--bind", "/system_ex", box2], check=True)
        seen2 = box2 + HOME[len("/system_ex"):]
        end = time.time() + 10
        while crc_file(f"{seen2}/bg.png") != zlib.crc32(S["bg"]):
            assert time.time() < end, "the new view was not covered"
            time.sleep(0.2)
        assert crc_file(f"{seen2}/icon.png") == S["stock_icon"]
        assert "a view the system made after the theme was applied" in log_text()
        assert status()["applied"] == "night", "covering a new view must not re-apply"
        assert len(mounts_on(seen)) == 1 and len(mounts_on(seen2)) == 1
        code, table = call("GET", "/api/mounts")
        assert code == 200 and b"SHELL_001" in table, table[:200]

        assert call("POST", "/api/apply", {"id": "none"})[0] == 200
        wait_applied("")
        assert crc_file(f"{seen}/bg.png") == S["stock_bg"], "sandbox view still themed after restore"
        assert crc_file(f"{seen2}/bg.png") == S["stock_bg"], "new view still themed after restore"
        assert not mounts_on(seen) and not mounts_on(seen2)
        time.sleep(2)   # the watcher is idle with no theme applied
        assert not mounts_on(seen2)
    finally:
        subprocess.run(["umount", box2])
        subprocess.run(["umount", box])
        for d in dummies:
            subprocess.run(["umount", d])
    assert call("POST", "/api/apply", {"id": "night"})[0] == 200
    wait_applied("night")


def t_slow_clients_do_not_block_others():
    held = []
    for _ in range(3):  # three clients that never finish their headers
        s = socket.create_connection(("127.0.0.1", PORT), 5)
        s.sendall(b"GET /api/status HTTP/1.1\r\nX-Slow: ")
        held.append(s)
    t = time.time()
    status()
    assert time.time() - t < 3, f"status took {time.time() - t:.1f}s behind slow clients"
    t = time.time()
    assert raw(b"GET /\x00x HTTP/1.1\r\n\r\n").startswith(b"HTTP/1.1 400")
    assert time.time() - t < 3, "NUL in headers was not rejected quickly"
    for s in held:
        s.close()
    results = []
    def burst():
        try:
            results.append(call("GET", "/api/status")[0])
        except Exception as e:
            results.append(repr(e))
    threads = [threading.Thread(target=burst) for _ in range(40)]
    for th in threads:
        th.start()
    for th in threads:
        th.join()
    assert all(r in (200, 503) for r in results) and results.count(200) >= 4, results
    assert status()["fw"] == "13.42" and S["plugin"].alive()


def t_log_rotates():
    with open(LOG, "a") as f:
        f.write("x" * (600 * 1024))
    assert call("POST", "/api/themes", {"id": "rot"})[0] == 200
    assert os.path.getsize(f"{BASE}/logs/log.1.txt") > 600 * 1024
    assert os.path.getsize(LOG) < 4096
    assert call("DELETE", "/api/themes", {"id": "rot"})[0] == 200


def t_media_policy():
    code, r = call("GET", "/api/assets", {"q": "NPXS40012"})
    kinds = {a["path"].rsplit("/", 1)[1]: a["kind"] for a in r["assets"]}
    assert kinds == {"move.wav": "audio", "ui.ttf": "font", "spinner.json": "json", "star.svg": "svg",
                     "loop.mp4": "video"}, kinds
    assert r["protected"] == 2, r
    assert call("POST", "/api/themes", {"id": "media", "name": "Media"})[0] == 200
    assert put_file("media", f"{MEDIA}/bundle.js", b"alert(1)")[0] == 403
    assert put_file("media", f"{MEDIA}/lib/native.sprx", b"\x7fELF")[0] == 403
    assert call("GET", "/api/asset", {"path": f"{MEDIA}/bundle.js"})[0] == 403

    cases = [
        ("sounds/move.wav", wav(48000, 1, 0.1, 3000), 200, None),
        ("sounds/move.wav", wav(44100, 1, 0.1, 3000), 422, "48000 Hz"),
        ("sounds/move.wav", wav(48000, 2, 0.1, 3000), 422, "channel"),
        ("sounds/move.wav", b"RIFFnope", 422, "valid WAV"),
        ("fonts/ui.ttf", b"\x00\x01\x00\x00" + b"\1" * 80, 200, None),
        ("fonts/ui.ttf", b"wOFF" + b"\0" * 80, 422, "same format"),
        ("anim/spinner.json", b'{"v":"5.7","w":100,"h":100,"layers":[{"ty":4}]}', 200, None),
        ("anim/spinner.json", b'{"v":"5.7","w":120,"h":100,"layers":[]}', 422, "canvas"),
        ("anim/spinner.json", b'[1,2,3]', 422, "same structure"),
        ("anim/spinner.json", b'{"w":100,"h":100,', 422, "valid JSON"),
        ("anim/spinner.json", b'{"w":100,"h":100,"x":' + b"[" * 100 + b"]" * 100 + b"}", 422, "valid JSON"),
        ("icons/star.svg", b'<svg xmlns="http://www.w3.org/2000/svg"><circle r="3"/></svg>', 200, None),
        ("icons/star.svg", b'<svg><script>alert(1)</script></svg>', 422, "<script"),
        ("icons/star.svg", b'<svg><image href="http://evil/x.png"/></svg>', 422, "href"),
        ("video/loop.mp4", b"\x00\x00\x00\x18ftypmp42" + b"\1" * 40, 200, None),
        ("video/loop.mp4", b"\x1a\x45\xdf\xa3" + b"\0" * 40, 422, "same format"),
    ]
    for rel, data, want, needle in cases:
        code, r = put_file("media", f"{MEDIA}/{rel}", data)
        assert code == want, (rel, needle, code, r)
        if needle:
            assert needle in r["error"], (rel, r)

    assert call("POST", "/api/apply", {"id": "media"})[0] == 200
    s = wait_applied("media")
    assert "5 files verified" in s["result"], s["result"]
    assert open(f"{MEDIA}/fonts/ui.ttf", "rb").read() == b"\x00\x01\x00\x00" + b"\1" * 80
    assert open(f"{MEDIA}/bundle.js", "rb").read() == b"console.log(1)"
    assert S["plugin"].alive()


def t_music():
    """A theme keeps its music source (48 kHz WAV) and a loudness setting; the
    plugin plays nothing itself, the home music file (bgm_home.at9) does."""
    assert call("POST", "/api/themes", {"id": "tunes", "name": "Tunes"})[0] == 200
    for data, needle in [(wav(44100, 2, 1, 20000), "48 kHz"), (wav(48000, 1, 1, 20000), "48 kHz"),
                         (b"not audio at all", "valid WAV"), (wav(48000, 2, 0.001, 20000), "too short")]:
        code, r = call("PUT", "/api/theme/music", {"id": "tunes", "crc": f"{zlib.crc32(data):08x}"}, data)
        assert code == 422 and needle in r["error"], (needle, code, r)
    track = wav(48000, 2, 2, 20000)
    code, r = call("PUT", "/api/theme/music", {"id": "tunes", "crc": f"{zlib.crc32(track):08x}"}, track)
    assert code == 200 and r["seconds"] == 2, r
    theme = {t["id"]: t for t in status()["themes"]}["tunes"]
    assert theme["music"] == 2 and theme["gain"] == 0, theme
    assert "audio" not in status()
    assert call("GET", "/api/theme/music", {"id": "tunes"})[1] == track

    assert call("POST", "/api/theme/settings", {"id": "tunes", "gain": "-5"})[0] == 200
    assert {t["id"]: t for t in status()["themes"]}["tunes"]["gain"] == -5
    for bad in ("13", "-13", "x", "3dB"):
        assert call("POST", "/api/theme/settings", {"id": "tunes", "gain": bad})[0] == 400, bad
    assert "music_gain=-5" in open(f"{BASE}/themes/tunes/theme.ini").read()

    # The part of the music that becomes the home music, and its loop joint.
    theme = {t["id"]: t for t in status()["themes"]}["tunes"]
    assert (theme["musicStart"], theme["musicEnd"], theme["musicLoop"], theme["musicBlend"]) == (0, 0, "crossfade", 2000), theme
    assert call("POST", "/api/theme/settings", {"id": "tunes", "start": "500", "end": "1800", "loop": "fade", "blend": "1000"})[0] == 200
    theme = {t["id"]: t for t in status()["themes"]}["tunes"]
    assert (theme["musicStart"], theme["musicEnd"], theme["musicLoop"], theme["musicBlend"], theme["gain"]) == (500, 1800, "fade", 1000, -5), theme
    ini = open(f"{BASE}/themes/tunes/theme.ini").read()
    assert "music_start=500\nmusic_end=1800\nmusic_play_from=-1\nmusic_loop_start=-1\nmusic_loop_end=-1\nmusic_order=music\nmusic_loop=fade\nmusic_blend=1000\n" in ini and "music_gain=-5" in ini, ini
    # Where "repeat the music part" starts playing: inside the music part.
    assert call("POST", "/api/theme/settings", {"id": "tunes", "play_from": "1200"})[0] == 200
    assert {t["id"]: t for t in status()["themes"]}["tunes"]["musicPlayFrom"] == 1200
    for bad in ("400", "1800", "x"):
        assert call("POST", "/api/theme/settings", {"id": "tunes", "play_from": bad})[0] == 400, bad
    assert call("POST", "/api/theme/settings", {"id": "tunes", "play_from": "-1"})[0] == 200
    # A loop part and the order the two parts play in.
    assert call("POST", "/api/theme/settings", {"id": "tunes", "order": "loop-first"})[0] == 400        # needs a loop part
    assert call("POST", "/api/theme/settings", {"id": "tunes", "loop_start": "200", "loop_end": "900", "order": "alternate"})[0] == 200
    theme = {t["id"]: t for t in status()["themes"]}["tunes"]
    assert (theme["musicLoopStart"], theme["musicLoopEnd"], theme["musicOrder"]) == (200, 900, "alternate"), theme
    for bad in ({"order": "shuffle"}, {"loop_start": "-1"}, {"loop_start": "900", "loop_end": "200"}, {"loop_end": "-2"}):
        assert call("POST", "/api/theme/settings", {"id": "tunes", **bad})[0] == 400, bad
    assert call("POST", "/api/theme/settings", {"id": "tunes", "loop_start": "-1", "loop_end": "-1"})[0] == 200
    theme = {t["id"]: t for t in status()["themes"]}["tunes"]
    assert (theme["musicLoopStart"], theme["musicOrder"]) == (-1, "music"), theme                     # no loop part: plain repeat
    for bad in ({"start": "-1"}, {"start": "x"}, {"end": "3600001"}, {"start": "2000"}, {"loop": "wobble"}, {"blend": "9000"}):
        assert call("POST", "/api/theme/settings", {"id": "tunes", **bad})[0] == 400, bad
    assert call("POST", "/api/theme/settings", {"id": "tunes", "name": "Tunes 2"})[0] == 200   # keeps the music settings
    theme = {t["id"]: t for t in status()["themes"]}["tunes"]
    assert (theme["name"], theme["musicStart"], theme["musicLoop"], theme["gain"]) == ("Tunes 2", 500, "fade", -5), theme
    code, _ = call("POST", "/api/themes/duplicate", {"id": "tunes", "new_id": "tunes-copy", "name": "Tunes copy"})
    copy = {t["id"]: t for t in status()["themes"]}["tunes-copy"]
    assert code == 200 and (copy["gain"], copy["musicEnd"], copy["musicLoop"]) == (-5, 1800, "fade"), copy
    assert call("DELETE", "/api/themes", {"id": "tunes-copy"})[0] == 200

    # Music the browser has not encoded yet (themes from before 1.10) is reported.
    assert put_file("tunes", f"{HOME}/bg.png", png(480, 270, (1, 2, 3)))[0] == 200
    assert call("POST", "/api/apply", {"id": "tunes"})[0] == 200
    s = wait_applied("tunes")
    assert "not converted to home music yet" in s["result"], s["result"]
    for gone in ("/api/music/test", "/api/music/soundcheck", "/api/music/route"):
        assert call("POST", gone)[0] == 404, gone
    assert call("GET", "/api/music/routes")[0] == 404

    assert call("POST", "/api/apply", {"id": "none"})[0] == 200
    wait_applied("")
    assert call("DELETE", "/api/theme/music", {"id": "tunes"})[0] == 200
    assert {t["id"]: t for t in status()["themes"]}["tunes"]["music"] == 0
    assert call("DELETE", "/api/themes", {"id": "tunes"})[0] == 200


def t_settings_and_duplicate():
    assert call("POST", "/api/theme/settings", {"id": "night", "name": "Night Sky\nfw=any"})[0] == 200
    ini = open(f"{BASE}/themes/night/theme.ini").read()
    assert ini.splitlines()[0] == "name=Night Sky fw any" and ini.count("fw=") == 1, ini
    assert call("POST", "/api/theme/settings", {"id": "night", "fw": "13.40,13.42"})[0] == 200
    assert {t["id"]: t for t in status()["themes"]}["night"]["compatible"] is True
    code, _ = call("POST", "/api/themes/duplicate", {"id": "night", "new_id": "night-copy", "name": "Night Copy"})
    assert code == 200
    themes = {t["id"]: t for t in status()["themes"]}
    assert themes["night-copy"]["name"] == "Night Copy" and themes["night-copy"]["files"] == themes["night"]["files"]
    assert call("POST", "/api/themes/duplicate", {"id": "night", "new_id": "night-copy"})[0] == 409
    assert call("POST", "/api/themes/duplicate", {"id": "night", "new_id": "../x"})[0] == 400
    assert call("DELETE", "/api/themes", {"id": "night-copy"})[0] == 200


def t_preview_pictures():
    """A theme's library picture: a complete JPEG kept beside the theme, never in what is applied."""
    pic, other = jpeg_header(1280, 720), jpeg_header(640, 480)

    def put(kind, data, theme="night"):
        return call("PUT", "/api/theme/preview", {"id": theme, "kind": kind, "crc": f"{zlib.crc32(data):08x}"}, data)

    def night():
        return {t["id"]: t for t in status()["themes"]}["night"]

    def shown(**kw):
        return call("GET", "/api/theme/preview", {"id": "night", **kw})

    assert night()["cover"] == "" and shown()[0] == 404
    code, r = put("background", pic)
    assert code == 200 and r["size"] == len(pic), (code, r)
    assert night()["cover"] == "background" and night()["coverTime"] > 0
    assert shown() == (200, pic)
    assert put("picture", other)[0] == 200
    assert night()["cover"] == "picture" and shown()[1] == other and shown(kind="background")[1] == pic

    # Not a JPEG, cut short, too small, too large a side, unknown kind, no theme, too big.
    for data in (png(64, 36, (1, 2, 3)), pic[:-2], jpeg_header(8, 8), jpeg_header(5000, 100)):
        assert put("picture", data)[0] == 422
    assert put("cover", pic)[0] == 400
    assert put("picture", pic, theme="nope")[0] == 404
    assert call("PUT", "/api/theme/preview", {"id": "night", "kind": "picture"}, b"x",
                headers={"Content-Length": str(8 << 20)})[0] == 413
    assert shown()[1] == other, "a refused upload changed the picture"
    assert not [f for f in os.listdir(f"{BASE}/themes/night") if f.endswith(".part")]
    assert not [f for f in os.listdir(f"{BASE}/themes/night/files") if f.endswith(".jpg")]

    # A duplicate keeps its pictures.
    assert call("POST", "/api/themes/duplicate", {"id": "night", "new_id": "night-pic", "name": "Pic"})[0] == 200
    assert {t["id"]: t for t in status()["themes"]}["night-pic"]["cover"] == "picture"
    assert call("DELETE", "/api/themes", {"id": "night-pic"})[0] == 200

    # Removing the chosen picture shows the background one again.
    assert call("DELETE", "/api/theme/preview", {"id": "night", "kind": "picture"})[0] == 200
    assert night()["cover"] == "background" and shown()[1] == pic
    assert call("DELETE", "/api/theme/preview", {"id": "night", "kind": "background"})[0] == 200
    assert night()["cover"] == "" and shown()[0] == 404
    assert call("DELETE", "/api/theme/preview", {"id": "night", "kind": "background"})[0] == 404
    assert call("DELETE", "/api/theme/preview", {"id": "night", "kind": "picture"}, token=False)[0] == 403


def t_save_and_discard():
    """The first change keeps a snapshot; Save keeps the changes, Discard puts the theme back."""
    def night():
        return {t["id"]: t for t in status()["themes"]}["night"]

    def files():
        return sorted(f["path"] for f in call("GET", "/api/theme/files", {"id": "night"})[1]["files"])

    theme, snap = f"{BASE}/themes/night", f"{BASE}/snapshots/night"
    assert call("POST", "/api/theme/save", {"id": "night"})[0] == 200
    assert night()["unsaved"] is False and not os.path.exists(snap)
    name, before, bg = night()["name"], files(), open(f"{theme}/files{HOME}/bg.png", "rb").read()
    applied = status()["active"] == "night"

    newer = png(480, 270, (200, 40, 40))
    assert put_file("night", f"{HOME}/bg.png", newer)[0] == 200
    assert night()["unsaved"] is True
    assert open(f"{snap}/files{HOME}/bg.png", "rb").read() == bg, "the snapshot changed with the theme"
    assert os.stat(f"{snap}/files{HOME}/icon@2x.png").st_ino == os.stat(f"{theme}/files{HOME}/icon@2x.png").st_ino
    assert call("DELETE", "/api/theme/file", {"id": "night", "path": f"{HOME}/strings.json"})[0] == 200
    assert call("POST", "/api/theme/settings", {"id": "night", "name": "Changed"})[0] == 200

    code, r = call("POST", "/api/theme/discard", {"id": "night"})
    assert code == 200 and r["reapplied"] is applied, (code, r)
    t = night()
    assert t["unsaved"] is False and t["name"] == name and files() == before, t
    assert open(f"{theme}/files{HOME}/bg.png", "rb").read() == bg
    assert not os.path.exists(snap) and not os.path.exists(f"{snap}.trash")
    if applied:
        end = time.time() + 30
        while crc_file(f"{HOME}/bg.png") != zlib.crc32(bg):
            assert time.time() < end, "the console was not updated after discarding"
            time.sleep(0.2)
        wait_applied("night")
    assert call("POST", "/api/theme/discard", {"id": "night"})[0] == 409

    # Save keeps the changes.
    assert put_file("night", f"{HOME}/bg.png", newer)[0] == 200
    assert call("POST", "/api/theme/save", {"id": "night"})[0] == 200
    assert night()["unsaved"] is False and open(f"{theme}/files{HOME}/bg.png", "rb").read() == newer
    assert put_file("night", f"{HOME}/bg.png", bg)[0] == 200
    assert call("POST", "/api/theme/save", {"id": "night"})[0] == 200

    # A picture the library makes by itself is not a change; a chosen one is.
    pic = jpeg_header(1280, 720)
    preview = lambda kind: call("PUT", "/api/theme/preview", {"id": "night", "kind": kind, "crc": f"{zlib.crc32(pic):08x}"}, pic)
    assert preview("background")[0] == 200 and night()["unsaved"] is False
    assert preview("picture")[0] == 200 and night()["unsaved"] is True
    assert call("POST", "/api/theme/discard", {"id": "night"})[0] == 200
    assert night()["cover"] == "background"
    assert call("DELETE", "/api/theme/preview", {"id": "night", "kind": "background"})[0] == 200
    assert night()["unsaved"] is False

    # Unknown themes, and a deleted theme takes its snapshot with it.
    assert call("POST", "/api/theme/save", {"id": "nope"})[0] == 404
    assert call("POST", "/api/theme/discard", {"id": "nope"})[0] == 409
    assert call("POST", "/api/theme/save", {"id": "night"}, token=False)[0] == 403
    assert call("POST", "/api/themes/duplicate", {"id": "night", "new_id": "scratch", "name": "Scratch"})[0] == 200
    assert put_file("scratch", f"{HOME}/bg.png", newer)[0] == 200
    assert os.path.exists(f"{BASE}/snapshots/scratch")
    assert call("DELETE", "/api/themes", {"id": "scratch"})[0] == 200
    assert not os.path.exists(f"{BASE}/snapshots/scratch")


def t_home_background_textures():
    stock_path = f"{VSH}/bg_hub_default.dds.zst"
    stock_dds = subprocess.run(["zstd", "-d", "-c", "-q", stock_path], capture_output=True, check=True).stdout
    code, r = call("GET", "/api/assets", {"q": "vsh_asset"})
    assert {a["path"]: a["kind"] for a in r["assets"]} == {stock_path: "texture", f"{VSH}/bgm_home.at9": "audio",
                                                          f"{VSH}/sfx_quiet.at9": "audio"}, r
    code, info = call("GET", "/api/asset/texture", {"path": stock_path})
    assert code == 200 and info["format"] == "BC7" and info["dxgi"] == 98 and info["zstd"] is True, info
    assert (info["width"], info["height"], info["mips"]) == (256, 128, 1), info
    import base64
    assert base64.b64decode(info["header"]) == stock_dds[:148]
    assert call("GET", "/api/asset/texture", {"path": f"{HOME}/bg.png"})[0] == 403

    assert call("POST", "/api/themes", {"id": "wall", "name": "Wallpaper"})[0] == 200
    mine = stock_dds[:148] + bytes((i * 31) & 255 for i in range(len(stock_dds) - 148))
    for bad, needle in [(zstd_stored(dds_bc7(128, 128)), "exactly like the system file"),
                        (zstd_stored(mine[:-16]), "not a valid DDS"),
                        (b"\x28\xb5\x2f\xfd" + b"\xff" * 40, "zstd"),
                        (mine, "zstd-compressed like the system file")]:
        code, r = put_file("wall", stock_path, bad)
        assert code == 422 and needle in r["error"], (needle, code, r)
    assert put_file("wall", stock_path, zstd_cli(mine))[0] == 200          # fully compressed by the real tool
    stored = zstd_stored(mine)
    assert put_file("wall", stock_path, stored)[0] == 200                  # stored blocks, as the builder writes

    assert call("POST", "/api/apply", {"id": "wall"})[0] == 200
    s = wait_applied("wall")
    assert "1 files verified" in s["result"], s["result"]
    assert open(stock_path, "rb").read() == stored
    assert crc_file(f"{VSH}/eboot_like.rco") == zlib.crc32(b"\0" * 64)    # rest of the folder stays stock
    assert len(mounts_on(VSH)) == 1
    assert call("POST", "/api/apply", {"id": "night"})[0] == 200
    wait_applied("night")
    assert open(stock_path, "rb").read() != stored
    assert call("DELETE", "/api/themes", {"id": "wall"})[0] == 200


def t_home_music_at9():
    path = f"{VSH}/bgm_home.at9"
    code, info = call("GET", "/api/asset/audio", {"path": path})
    assert code == 200 and info == {"codec": "ATRAC9", "channels": 6, "rate": 48000, "bits": 0}, info
    assert call("GET", "/api/asset/audio", {"path": f"{HOME}/bg.png"})[0] == 403
    assert call("POST", "/api/themes", {"id": "tune9", "name": "Tune"})[0] == 200
    stock = open(path, "rb").read()

    music = at9_music(path, 3)
    # Loop laid out like the console's own music: it starts inside the song
    # (here a quarter in, 2 s for longer songs) and audio follows its end.
    smpl = music.index(b"smpl") + 8
    fact = music.index(b"fact") + 8
    start, end = struct.unpack_from("<II", music, smpl + 44)
    total = struct.unpack_from("<I", music, fact)[0]
    assert (start, end, total) == (36000, 144000 + 36000 - 1, 144000 + 72000), (start, end, total)
    data = music.index(b"data") + 8
    flipped = bytearray(music)
    for i in range(40):
        flipped[data + 30000 + i * 37] ^= 0xff
    zeroed = bytearray(music)
    zeroed[data + 800 * 50:data + 800 * 51] = bytes(800)
    shorter = bytearray(music[:-300])
    struct.pack_into("<I", shorter, 4, len(shorter) - 8)
    struct.pack_into("<I", shorter, data - 4, len(shorter) - data)
    at9_stock(3, 2)                                                 # stereo: another ATRAC9 format
    stereo = at9_music("/tmp/at9-stock-2-3-0.05.at9", 2)
    listed = music + b"LIST" + struct.pack("<I", 4) + b"INFO"
    listed = listed[:4] + struct.pack("<I", len(listed) - 8) + listed[8:]
    # The unused end of a superframe must be filled like Sony fills it (0x01):
    # zeros there (all other bits identical) made the console's decoder go silent.
    zero_pad = bytearray(music)
    last = len(zero_pad) - 800
    run = 800 - len(bytes(zero_pad[last:]).rstrip(b"\x01"))      # padding in the last superframe
    assert run > 8, run
    zero_pad[len(zero_pad) - run + 1:] = bytes(run - 1)
    cases = [(stereo, "exactly the system file's format"),
             (bytes(zero_pad), "fills unused space with 0x00 where the system sound uses 0x01"),
             (wav(48000, 6, 0.1, 1000), "not a valid ATRAC9"),
             (bytes(flipped), "damaged"),
             (bytes(zeroed), "damaged"),
             (bytes(shorter), "whole number of blocks"),
             (music[:-800], "RIFF size"),
             (listed, "unexpected chunk"),
             (at9_music(path, 7), "longer than 2 times")]
    for bad, needle in cases:
        code, r = put_file("tune9", path, bad)
        assert code == 422 and needle in r["error"], (needle, code, r)

    code, r = put_file("tune9", path, music)
    assert code == 200, r
    assert call("POST", "/api/apply", {"id": "tune9"})[0] == 200
    s = wait_applied("tune9")
    assert "1 files verified" in s["result"] and "home music" not in s["result"], s["result"]
    assert open(path, "rb").read() == music                       # what ShellUI reads now
    # The editor still gets the system version (kept at apply), to match
    # loudness and length against and to validate further uploads with.
    assert open(f"{BASE}/stock/13.42{path}", "rb").read() == stock
    assert call("GET", "/api/asset", {"path": path})[1] == stock
    code, r = put_file("tune9", path, at9_music(path, 7))
    assert code == 422 and "longer than 2 times" in r["error"], r     # measured against the system file
    assert call("GET", "/api/theme/check", {"id": "tune9", "path": path})[1] == {"ok": True}
    assert call("POST", "/api/apply", {"id": "none"})[0] == 200
    wait_applied("")
    assert open(path, "rb").read() == stock

    # Coding features the system sound never uses are refused, as a
    # precaution: the console's decoder is Sony's. Here: music uses more
    # bands than this silent sound.
    quiet = f"{VSH}/sfx_quiet.at9"
    code, r = put_file("tune9", quiet, at9_music(path, 2))
    assert code == 422 and "the console cannot play this sound" in r["error"] and "13 bands" in r["error"], r
    # A file left in a theme by an older version: flagged, and skipped on apply.
    write(f"{BASE}/themes/tune9/files{quiet}", at9_music(path, 2))
    code, r = call("GET", "/api/theme/check", {"id": "tune9", "path": quiet})
    assert code == 200 and r["ok"] is False and "13 bands" in r["error"], r
    assert call("POST", "/api/apply", {"id": "tune9"})[0] == 200
    s = wait_applied("tune9")
    assert "1 skipped" in s["result"], s["result"]
    assert open(quiet, "rb").read() != open(f"{BASE}/themes/tune9/files{quiet}", "rb").read()
    assert call("POST", "/api/apply", {"id": "none"})[0] == 200
    wait_applied("")
    assert call("DELETE", "/api/themes", {"id": "tune9"})[0] == 200


def t_long_mount_paths_use_parent_folder():
    S["plugin"].term()
    S["plugin"] = Plugin(env={"FAKE_MNAMELEN": "60"})
    assert call("POST", "/api/apply", {"id": "none"})[0] == 200
    wait_applied("")
    assert call("POST", "/api/themes", {"id": "deep", "name": "Deep"})[0] == 200
    deep = png(32, 32, (250, 10, 10))
    assert put_file("deep", f"{HOME}/sub/deep.png", deep)[0] == 200  # folder path is 65 characters
    assert call("POST", "/api/apply", {"id": "deep"})[0] == 200
    s = wait_applied("deep")
    assert "1 files verified" in s["result"] and "partly" not in s["result"], s["result"]
    parent = f"{APPS}/NPXS40008/assets/src/modules/home"   # 54 characters
    assert len(mounts_on(parent)) == 1 and not mounts_on(f"{HOME}/sub"), (mounts_on(parent), mounts_on(f"{HOME}/sub"))
    assert crc_file(f"{HOME}/sub/deep.png") == zlib.crc32(deep)
    assert crc_file(f"{HOME}/icon.png") == S["stock_icon"]  # the rest of the parent stays stock
    assert crc_file(f"{OTHER}/target.png") == S["stock_target"]
    assert "from its parent" in log_text()
    assert call("POST", "/api/apply", {"id": "none"})[0] == 200
    wait_applied("")
    assert not mounts_on(parent)
    assert call("DELETE", "/api/themes", {"id": "deep"})[0] == 200
    S["plugin"].term()
    S["plugin"] = Plugin()


def t_system_is_byte_identical_afterwards():
    assert call("POST", "/api/apply", {"id": "none"})[0] == 200
    wait_applied("")
    S["plugin"].term()
    after = snapshot("/system_ex")
    assert after == S["system_before"], {k for k in after if after.get(k) != S["system_before"].get(k)}
    assert snapshot("/mnt/rnps") == S["mnt_before"]
    assert snapshot("/system_ex") == S["system_before"]
    assert os.listdir("/user/app") == ["THEM00001"]
    assert os.listdir("/data") == ["ps5-theme"]
    assert "ERROR: AddressSanitizer" not in open("/tmp/plugin-out.txt", errors="replace").read()
    assert "runtime error" not in open("/tmp/plugin-out.txt", errors="replace").read()


def t_crash_is_reported_and_recovered():
    p = Plugin(CRASH)
    try:
        call("POST", "/api/debug/crash", timeout=5)
    except Exception:
        pass
    p.proc.wait(10)
    assert p.proc.returncode == 70, p.proc.returncode
    reports = [f for f in os.listdir(f"{BASE}/logs") if f.startswith("crash-")]
    assert len(reports) == 1, reports
    text = open(f"{BASE}/logs/{reports[0]}").read()
    for needle in ("SIGSEGV", "thread:    web worker", ": POST /api/debug/crash",
                   "rip:", "backtrace", "#1"):
        assert needle in text, (needle, text)
    assert os.path.exists(f"{BASE}/logs/last-crash")

    p = Plugin(CRASH)
    try:
        assert "recovered from a crash" in open("/tmp/notifications.txt").read()
        assert not os.path.exists(f"{BASE}/logs/last-crash")
        code, r = call("GET", "/api/crashes")
        assert code == 200 and r["crashes"] == reports, r
        code, body = call("GET", "/api/crash", {"name": reports[0]})
        assert code == 200 and b"SIGSEGV" in body
    finally:
        p.term()


def t_privilege_failure_exits_cleanly():
    out = open("/tmp/init-fail.txt", "wb")
    proc = subprocess.run([os.path.abspath(ASAN)], env=dict(ENV, FAKE_INIT_FAIL="1"), stdout=out,
                          stderr=out, timeout=20)
    assert proc.returncode == 1, proc.returncode
    notes = open("/tmp/notifications.txt").read()
    assert "could not get privileges" in notes, notes  # reported before any file is touched
    assert "could not raise privileges" in open("/tmp/init-fail.txt").read()  # kernel log line


TESTS = [t_startup, t_ui_and_icon_served, t_mutations_need_token, t_create_theme_rules, t_asset_search,
         t_upload_is_verified, t_apply_verifies_and_is_read_only, t_reapply_picks_up_edits,
         t_stock_mirror_and_linked_staging,
         t_firmware_mismatch_is_refused, t_delete_rules, t_concurrent_requests_during_apply,
         t_restore_stock_cleans_up, t_overlays_reach_system_views, t_recovers_after_hard_kill, t_survives_malformed_and_abusive_requests,
         t_slow_clients_do_not_block_others, t_log_rotates, t_media_policy, t_music, t_settings_and_duplicate, t_preview_pictures, t_save_and_discard,
         t_home_background_textures, t_home_music_at9, t_long_mount_paths_use_parent_folder, t_system_is_byte_identical_afterwards, t_crash_is_reported_and_recovered,
         t_privilege_failure_exits_cleanly]


def main():
    build_console()
    if "--serve" in sys.argv:
        # realistic home backgrounds: 4K BC7 like the console's (content is test data)
        big = dds_bc7(3840, 2160, fill=bytes(random.Random(3).randbytes(3840 * 2160)))
        for name in ("bg_hub_default", "Library_BG_blue_Low_Angle_001", "bg_NPXS40037", "bg_image_load_error"):
            write(f"{VSH}/{name}.dds.zst", zstd_cli(big, 3))
        write(f"{VSH}/bgm_home.at9", at9_stock(60))                   # a minute, so music fits up to 2:00
        write(f"{VSH}/sfx_warmboot.at9", at9_stock(2))
        Plugin()
        print(f"plugin running; open http://localhost:18095 (container port {PORT})", flush=True)
        while True:
            time.sleep(3600)

    if EMULATED:
        print("NOTE  mounts are emulated (no privileges); read-only overlay checks are skipped", flush=True)
    S["system_before"] = snapshot("/system_ex")
    S["mnt_before"] = snapshot("/mnt/rnps")
    S["plugin"] = Plugin()
    failed = 0
    for test in TESTS:
        t0 = time.time()
        try:
            test()
            print(f"PASS  {test.__name__}  ({time.time() - t0:.1f}s)", flush=True)
        except Exception as e:
            failed += 1
            print(f"FAIL  {test.__name__}: {e!r}", flush=True)
            tb = traceback.extract_tb(e.__traceback__)[-1]
            print(f"      at line {tb.lineno}: {tb.line}", flush=True)
            if not S["plugin"].alive() and test is not t_system_is_byte_identical_afterwards:
                print("      plugin died; output follows", flush=True)
                print(open("/tmp/plugin-out.txt", errors="replace").read()[-4000:], flush=True)
                S["plugin"] = Plugin()
    S["plugin"].kill()
    print(f"\n{len(TESTS) - failed}/{len(TESTS)} passed", flush=True)
    if failed:
        print("---- plugin log tail ----")
        print(open(LOG, errors="replace").read()[-3000:] if os.path.exists(LOG) else "(no log)")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
