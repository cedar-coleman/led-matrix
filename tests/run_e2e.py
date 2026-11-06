"""Browser end-to-end checks: media conversion in real Chrome, and screenshots of every tab.

Needs ffmpeg (fixtures), Google Chrome and Pillow. Run: python3 tests/run_e2e.py
Screenshots land in tests/e2e/screenshots/ for a visual check.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(HERE))
os.environ["MOCK_QUIET"] = "1"
import mock_server  # noqa: E402
from PIL import Image  # noqa: E402
from test_mock_server import FakeInstagram  # noqa: E402

PIN = "246813"  # the pages under tests/e2e/ use the same one

CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
FIXTURES = HERE / "e2e" / "fixtures"
SHOTS = HERE / "e2e" / "screenshots"


def make_fixtures():
    FIXTURES.mkdir(parents=True, exist_ok=True)
    ff = ["ffmpeg", "-y", "-loglevel", "error"]
    jobs = {
        "clip.mp4": ff + ["-f", "lavfi", "-i", "testsrc=size=640x360:rate=30:duration=4", "-pix_fmt", "yuv420p", "-c:v", "libx264"],
        "clip.webm": ff + ["-f", "lavfi", "-i", "testsrc=size=320x240:rate=25:duration=3", "-c:v", "libvpx"],
        "anim.gif": ff + ["-f", "lavfi", "-i", "testsrc=size=120x120:rate=5:duration=1"],
        "still.png": ff + ["-f", "lavfi", "-i", "testsrc=size=800x800:duration=1", "-frames:v", "1"],
    }
    for name, cmd in jobs.items():
        if not (FIXTURES / name).exists():
            subprocess.run(cmd + [str(FIXTURES / name)], check=True)


class TestHandler(mock_server.Handler):
    results = None
    done = None

    def do_GET(self):
        if self.path.startswith("/__test__/"):
            p = (HERE / "e2e" / self.path[len("/__test__/"):].split("?")[0]).resolve()
            if HERE in p.parents and p.is_file():
                data = p.read_bytes()
                self.send_response(200)
                ctype = {".html": "text/html", ".mp4": "video/mp4", ".webm": "video/webm",
                         ".gif": "image/gif", ".png": "image/png"}.get(p.suffix, "application/octet-stream")
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
                return
        super().do_GET()

    def do_POST(self):
        if self.path == "/__test__/result":
            type(self).results = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            self.send_json(200, {"ok": True})
            type(self).done.set()
            return
        super().do_POST()


def put_json(base, path, body, pin=None):
    import urllib.request
    headers = {"Content-Type": "application/json"} | ({"X-Admin-Pin": pin} if pin else {})
    req = urllib.request.Request(base + path, data=json.dumps(body).encode(), method="PUT", headers=headers)
    with urllib.request.urlopen(req) as r:
        return r.status, json.loads(r.read())


def start_server(handler_base=None):
    data = tempfile.mkdtemp(prefix="wall-e2e-data-")
    store = mock_server.Store(Path(data))
    store.http_get = FakeInstagram()  # never the real Instagram
    handler = type("H", (handler_base or TestHandler,), {"store": store, "done": threading.Event()})
    server = mock_server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return data, handler, server, f"http://127.0.0.1:{server.server_address[1]}"


def screenshot(base, name, tab, phone, size="1280,1400"):
    out = SHOTS / f"{name}-{'phone' if phone else 'desktop'}.png"
    url = f"{base}/__test__/phone.html#{tab}" if phone else f"{base}/#{tab}"
    proc, _ = chrome(f"--screenshot={out}", f"--window-size={'600,1700' if phone else size}",
                     "--virtual-time-budget=5000", url)
    proc.wait(60)
    return out


def chrome(*args, timeout=60):
    profile = tempfile.mkdtemp(prefix="chrome-e2e-")
    cmd = [CHROME, "--headless=new", "--disable-gpu", "--no-first-run", "--mute-audio",
           f"--user-data-dir={profile}", *args]
    return subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL), profile


class BrowserTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        make_fixtures()
        cls.data, cls.handler, cls.server, cls.base = start_server()
        status, _ = put_json(cls.base, "/api/settings", {"new_pin": PIN})  # the first-run step
        assert status == 200

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()

    def test_media_conversion_in_chrome(self):
        proc, _ = chrome(f"{self.base}/__test__/convert.html")
        try:
            self.assertTrue(self.handler.done.wait(90), "browser never reported back")
        finally:
            proc.kill()
        by_file = {r["file"]: r for r in self.handler.results}
        for r in self.handler.results:
            self.assertNotIn("error", r, r)
            self.assertEqual(r["status"], 200, r)
        expected_frames = {"clip.mp4": 45, "clip.webm": 10, "anim.gif": 5, "still.png": 1}
        for name, frames in expected_frames.items():
            self.assertEqual(by_file[name]["frames"], frames, name)
            with Image.open(Path(self.data) / "media" / by_file[name]["uploaded"]) as im:
                self.assertEqual(im.size, (256, 96), name)
                self.assertEqual(getattr(im, "n_frames", 1), frames, name)
        self.assertEqual(by_file["clip.mp4"]["kind"], "video")
        self.assertEqual(by_file["anim.gif"]["kind"], "gif")
        index = json.loads((Path(self.data) / "media_index.json").read_text())
        for r in self.handler.results:
            self.assertAlmostEqual(index[r["uploaded"]]["peak_a"], r["peak_a"], places=2)
            self.assertTrue(0 < r["peak_a"] <= 6 * 4.9, r)

    def test_screenshots_of_every_tab(self):
        SHOTS.mkdir(parents=True, exist_ok=True)
        # Chrome won't make a window narrower than about 500 px, so phone width is a
        # 390 px iframe (tests/e2e/phone.html) inside a wider window.
        # A phone entry may name an act from phone.html that sets up a state (unsaved edits, a search).
        # Read-only: no PIN in the page, so nothing here changes the wall (the stale act saves
        # with the PIN and is last).
        for tab, size, phone in [("wall", "1280,1500", False), ("athletes", "1280,2400", False),
                                 ("media", "1280,1100", False), ("rotation", "1280,1000", False),
                                 ("settings", "1280,1600", False), ("wall", "390,1700", True),
                                 ("athletes", "390,1700", True), ("rotation", "390,1700", True),
                                 ("media", "390,1700", True), ("settings", "390,1700", True),
                                 ("athletes&act=unsaved", "390,1700", True), ("athletes&act=find", "390,1700", True),
                                 ("wall&act=diag", "390,1700", True), (f"athletes&act=stale&pin={PIN}", "390,1700", True)]:
            name = tab.split("&pin=")[0].replace("&act=", "-")
            out = screenshot(self.base, name, tab, phone, size)
            self.assertTrue(out.exists() and out.stat().st_size > 10000, f"no screenshot for {tab}")

    def test_ui_behaviour_on_a_phone(self):
        # Runs after the other tests (alphabetical): it ends by locking this computer out.
        self.handler.done.clear()
        self.handler.results = None
        proc, _ = chrome(f"{self.base}/__test__/ui.html", "--window-size=600,1000")
        try:
            self.assertTrue(self.handler.done.wait(90), "browser never reported back")
        finally:
            proc.kill()
        failed = [r for r in self.handler.results if not r["ok"]]
        self.assertGreater(len(self.handler.results), 35)
        self.assertEqual(failed, [], "\n".join(f"{r['name']}: {r['detail']}" for r in failed))


class FirstRunTest(unittest.TestCase):
    """A wall with no PIN yet: the console shows only the "Set an admin PIN" step."""

    @classmethod
    def setUpClass(cls):
        cls.data, cls.handler, cls.server, cls.base = start_server()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()

    def test_first_run_and_lockout_screenshots(self):
        SHOTS.mkdir(parents=True, exist_ok=True)
        for phone in (False, True):
            out = screenshot(self.base, "setup", "athletes", phone, "1280,900")
            self.assertTrue(out.exists() and out.stat().st_size > 10000)
        self.assertEqual(put_json(self.base, "/api/settings", {"new_pin": PIN})[0], 200)
        out = screenshot(self.base, "athletes-locked", f"athletes&act=locked&pin={PIN}", True)
        self.assertTrue(out.exists() and out.stat().st_size > 10000)


if __name__ == "__main__":
    if not shutil.which("ffmpeg") or not Path(CHROME).exists():
        sys.exit("needs ffmpeg and Google Chrome")
    unittest.main(verbosity=2)
