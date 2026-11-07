"""API contract tests for mock_server.py. The firmware must pass the same cases (see API.md).

Run: python3 -I -m unittest discover -s tests
"""
import hashlib
import json
import os
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request
from datetime import datetime, timedelta, timezone
from io import BytesIO
from pathlib import Path
from zoneinfo import ZoneInfo

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
os.environ["MOCK_QUIET"] = "1"
import mock_server  # noqa: E402

PIN = "135790"


def gif(w=256, h=96, pad=0):
    # Header and logical screen descriptor are all the server checks.
    return b"GIF89a" + w.to_bytes(2, "little") + h.to_bytes(2, "little") + b"\x00\x00\x00" + b"\x00" * pad + b"\x3b"


def jpeg(color=(200, 30, 30), size=(120, 160)):
    from PIL import Image
    buf = BytesIO()
    Image.new("RGB", size, color).save(buf, "JPEG")
    return buf.getvalue()


class FakeInstagram:
    """Stands in for Store.http_get: Graph API JSON for graph URLs, JPEG bytes for the rest.

    fail: a substring of an image URL and the exception to raise for it, or (substring, "garbage")
    to return bytes that aren't an image. gate: an Event every call waits on, to hold a job open.
    """

    def __init__(self, fail=None, gate=None, stories=2):
        self.fail, self.gate, self.n_stories = fail, gate, stories
        self.urls = []

    def __call__(self, url):
        self.urls.append(url)
        if self.gate is not None:
            assert self.gate.wait(10), "test never released the gate"
        if url.startswith(mock_server.GRAPH):
            if "/stories?" in url:
                data = [{"id": f"st{i}", "media_type": "IMAGE", "media_url": f"https://cdn.example/story{i}.jpg",
                         "timestamp": f"2026-11-06T1{i}:00:00+0000"} for i in range(self.n_stories)]
            elif "/media?" in url:
                data = [{"id": "p0", "media_type": "VIDEO", "media_product_type": "REELS",
                         "thumbnail_url": "https://cdn.example/post0.jpg", "timestamp": "2026-11-05T10:00:00+0000",
                         "like_count": 5, "permalink": "https://instagram.com/p/x"},
                        {"id": "p1", "media_type": "IMAGE", "media_url": "https://cdn.example/post1.jpg",
                         "timestamp": "2026-11-04T10:00:00+0000", "like_count": 3, "permalink": None}]
            elif "me/accounts" in url:
                data = [{"name": "The Strength Tank", "instagram_business_account": {"id": "17841400000000001", "username": "thestrengthtank"}},
                        {"name": "Unlinked page"}]
            else:
                return json.dumps({"username": "thestrengthtank", "id": "17841400000000001"}).encode()
            return json.dumps({"data": data}).encode()
        if self.fail and self.fail[0] in url:
            if self.fail[1] == "garbage":
                return b"<html>not an image</html>"
            raise self.fail[1]
        return jpeg()


class ApiBase(unittest.TestCase):
    SET_PIN = True

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.data = Path(self.tmp.name)
        self.pin = None
        self.start()
        if self.SET_PIN:
            status, body = self.call("PUT", "/api/settings", {"new_pin": PIN})
            self.assertEqual(status, 200, body)
            self.pin = PIN

    def start(self):
        self.server = mock_server.make_server(0, self.data)
        self.store = self.server.store
        self.base = f"http://127.0.0.1:{self.server.server_address[1]}"
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def stop(self):
        t = self.store.job_thread
        if t:
            t.join(10)
        self.server.shutdown()
        self.server.server_close()

    def restart(self):
        self.stop()
        self.start()

    def tearDown(self):
        self.stop()
        self.tmp.cleanup()

    def request(self, method, path, body=None, headers=None, raw=None, pin=True):
        data = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
        h = dict(headers or {})
        if pin is True and self.pin:
            h.setdefault("X-Admin-Pin", self.pin)
        elif isinstance(pin, str):
            h["X-Admin-Pin"] = pin
        req = urllib.request.Request(self.base + path, data=data, method=method, headers=h)
        try:
            with urllib.request.urlopen(req) as r:
                return r.status, json.loads(r.read() or b"null"), r.headers
        except urllib.error.HTTPError as e:
            with e:
                return e.code, json.loads(e.read() or b"null"), e.headers

    def call(self, method, path, body=None, headers=None, raw=None, pin=True):
        status, body, _ = self.request(method, path, body, headers, raw, pin)
        return status, body

    def rev(self, what):
        return self.call("GET", f"/api/{what}")[1]["rev"]

    def put_athletes(self, athletes, rev=None):
        return self.call("PUT", "/api/athletes", {"rev": self.rev("athletes") if rev is None else rev, "athletes": athletes})

    def put_slides(self, slides, rev=None):
        return self.call("PUT", "/api/playlist", {"rev": self.rev("playlist") if rev is None else rev, "slides": slides})

    def upload(self, name, data=None, query=""):
        return self.call("POST", f"/api/media?name={name}{query}", raw=gif() if data is None else data,
                         headers={"Content-Type": "image/gif"})

    def wait_idle(self, timeout=10):
        end = time.time() + timeout
        while time.time() < end:
            ig = self.call("GET", "/api/instagram")[1]
            if ig["busy"] is None:
                return ig
            time.sleep(0.02)
        self.fail("Instagram job never finished")


class ApiTest(ApiBase):
    def test_seeded_from_chalkboard(self):
        status, body = self.call("GET", "/api/athletes")
        self.assertEqual(status, 200)
        self.assertIsInstance(body["rev"], int)
        self.assertEqual(len(body["athletes"]), 31)
        amousalli = next(a for a in body["athletes"] if a["name"] == "amousalli")
        self.assertEqual(amousalli["back_squat"], 136.5)
        alli = next(a for a in body["athletes"] if a["name"] == "alli_garver")
        self.assertIsNone(alli["back_squat"])

    def test_seeded_rotation_has_new_prs_after_the_whiteboard(self):
        types = [x["type"] for x in self.call("GET", "/api/playlist")[1]["slides"]]
        self.assertEqual(types[types.index("whiteboard") + 1], "new_prs")
        athletes = self.call("GET", "/api/athletes")[1]["athletes"]
        self.assertTrue(all(a["pr_prev"] == {} for a in athletes))

    def test_new_prs_slide_type(self):
        for d, ok in [(5, True), (999, True), (8, True), (4, False), (1000, False), (8.5, False), (True, False)]:
            status, body = self.put_slides([{"id": "p1", "type": "new_prs", "duration_s": d, "enabled": True}])
            self.assertEqual(status == 200, ok, f"duration {d!r}: {status} {body}")
        status, body = self.put_slides([{"id": "p1", "type": "new_prs", "duration_s": 8, "file": "x.gif", "enabled": False}])
        self.assertEqual(status, 200, body)
        self.assertEqual(body["slides"], [{"id": "p1", "type": "new_prs", "duration_s": 8, "enabled": False}])
        status, body = self.put_slides([{"id": "p1", "type": "new_pr", "duration_s": 8}])
        self.assertEqual((status, body["error"]), (400, "Slide 1: type must be one of whiteboard, media, "
                                                         "instagram_story, instagram_posts, new_prs, announcements"))

    def test_screen_time_limits(self):
        for d, ok in [(5, True), (999, True), (4, False), (1000, False), (7.5, False), ("10", False), (True, False)]:
            status, body = self.put_slides([{"id": "s1", "type": "whiteboard", "duration_s": d, "enabled": True}])
            self.assertEqual(status == 200, ok, f"duration {d!r}: {status} {body}")

    def test_weights_half_kilo(self):
        base = {"id": "a1", "name": "x", "notes": "", "visible": True}
        for v, ok in [(136.5, True), (120, True), (None, True), (136.3, False), (0, False), (501, False), ("90", False)]:
            status, _ = self.put_athletes([base | {"snatch": v}])
            self.assertEqual(status == 200, ok, f"snatch {v!r}")

    def test_duplicate_ids_rejected(self):
        a = {"id": "a1", "name": "x"}
        status, body = self.put_athletes([a, a | {"name": "y"}])
        self.assertEqual((status, body["code"]), (400, "invalid"))

    def test_pin_guards_writes_not_reads(self):
        self.assertEqual(self.call("GET", "/api/athletes", pin=False)[0], 200)
        status, body = self.call("PUT", "/api/settings", {"brightness": 40}, pin=False)
        self.assertEqual((status, body["code"]), (401, "pin_required"))
        status, body = self.call("PUT", "/api/settings", {"brightness": 40}, pin="000000")
        self.assertEqual((status, body["code"]), (401, "pin_wrong"))
        status, body = self.call("PUT", "/api/settings", {"brightness": 40})
        self.assertEqual(status, 200)
        self.assertTrue(body["pin_set"])
        for path in ("/api/settings", "/api/status", "/api/instagram"):
            text = json.dumps(self.call("GET", path)[1])
            self.assertNotIn("pin_hash", text, path)
            self.assertNotIn("pin_salt", text, path)
            self.assertNotIn("pin_iter", text, path)

    def test_pin_stored_as_pbkdf2(self):
        s = json.loads((self.data / "settings.json").read_text())
        self.assertEqual(s["pin_iter"], 10000)
        self.assertEqual(len(bytes.fromhex(s["pin_salt"])), 16)
        self.assertEqual(s["pin_hash"], hashlib.pbkdf2_hmac("sha256", PIN.encode(), bytes.fromhex(s["pin_salt"]), 10000).hex())

    def test_changing_the_pin_needs_the_current_one(self):
        self.assertEqual(self.call("PUT", "/api/settings", {"new_pin": "24681357"}, pin=False)[0], 401)
        self.assertEqual(self.call("PUT", "/api/settings", {"new_pin": "24681357"}, pin="999999")[0], 401)
        for bad in ("12345", "1234567890123", "12a456", 123456, ""):
            status, body = self.call("PUT", "/api/settings", {"new_pin": bad})
            self.assertEqual((status, body["code"]), (400, "invalid"), repr(bad))
        self.assertEqual(self.call("PUT", "/api/settings", {"new_pin": "24681357"})[0], 200)
        self.assertEqual(self.call("PUT", "/api/settings", {"brightness": 30}, pin=PIN)[0], 401)
        self.assertEqual(self.call("PUT", "/api/settings", {"brightness": 30}, pin="24681357")[0], 200)

    def test_no_cors_headers(self):
        for method, path, body in [("GET", "/api/status", None), ("PUT", "/api/settings", {"brightness": 40}),
                                   ("PUT", "/api/settings", {"brightness": 4})]:
            _, _, headers = self.request(method, path, body, headers={"Origin": "http://evil.example"})
            self.assertFalse([k for k in headers if k.lower().startswith("access-control-")], (method, path))
        status, _, headers = self.request("OPTIONS", "/api/settings", headers={
            "Origin": "http://evil.example", "Access-Control-Request-Method": "PUT"})
        self.assertEqual(status, 405)
        self.assertFalse([k for k in headers if k.lower().startswith("access-control-")])

    def test_media_upload_checks_and_in_use_delete(self):
        self.assertEqual(self.upload("a.gif", gif(64, 32))[0], 400)
        self.assertEqual(self.upload("a.gif", b"not a gif")[0], 400)
        self.assertEqual(self.upload("../x.gif")[0], 400)
        self.assertEqual(self.upload("a.gif", query="&frames=3")[0], 200)
        self.assertEqual(self.upload("a.gif"), (409, {"error": "a.gif already exists", "code": "exists"}))
        _, media = self.call("GET", "/api/media")
        self.assertIn("a.gif", [f["name"] for f in media["files"]])
        slides = [{"id": "s1", "type": "media", "file": "a.gif", "duration_s": 10, "enabled": True}]
        self.assertEqual(self.put_slides(slides)[0], 200)
        status, body = self.call("DELETE", "/api/media/a.gif")
        self.assertEqual((status, body["code"]), (409, "in_use"))
        self.assertEqual(self.put_slides([])[0], 200)
        self.assertEqual(self.call("DELETE", "/api/media/a.gif")[0], 200)

    def test_media_peak_current(self):
        self.assertEqual(self.upload("a.gif", query="&frames=2&peak_a=12.345")[1]["peak_a"], 12.35)
        self.assertEqual(self.upload("b.gif")[0], 200)
        self.assertEqual(self.upload("c.gif", query="&peak_a=40")[0], 200)
        self.assertEqual(self.upload("d.gif", query="&peak_a=0")[0], 200)
        for bad in ("40.01", "-1", "nan", "inf", "abc", ""):
            status, body = self.upload("e.gif", query=f"&peak_a={bad}")
            self.assertEqual((status, body["code"]), (400, "invalid"), bad)
        files = {f["name"]: f for f in self.call("GET", "/api/media")[1]["files"]}
        self.assertEqual(files["a.gif"]["peak_a"], 12.35)
        self.assertEqual(files["a.gif"]["frames"], 2)
        self.assertIsNone(files["b.gif"]["peak_a"])
        self.assertEqual(files["c.gif"]["peak_a"], 40)
        self.assertNotIn("e.gif", files)

    def test_playlist_rejects_unknown_media(self):
        slides = [{"id": "s1", "type": "media", "file": "nope.gif", "duration_s": 10, "enabled": True}]
        self.assertEqual(self.put_slides(slides)[0], 400)

    def test_control(self):
        self.assertEqual(self.call("POST", "/api/control", {"action": "pause"})[0], 200)
        self.assertTrue(self.call("GET", "/api/status")[1]["paused"])
        status, body = self.call("POST", "/api/control", {"action": "explode"})
        self.assertEqual((status, body), (400, {"error": "action must be next, prev, pause, resume, wake or sleep",
                                                "code": "invalid"}))

    def test_instagram_slide_types_and_settings(self):
        for t in ("instagram_story", "instagram_posts"):
            status, _ = self.put_slides([{"id": "s1", "type": t, "duration_s": 6, "enabled": True}])
            self.assertEqual(status, 200, t)
        self.assertEqual(self.put_slides([{"id": "s1", "type": "tiktok", "duration_s": 6}])[0], 400)
        status, ig = self.call("GET", "/api/instagram")
        self.assertEqual(status, 200)
        self.assertFalse(ig["token_set"])
        self.assertNotIn("access_token", ig)
        self.assertTrue(ig["sample"])
        self.assertEqual((ig["busy"], ig["accounts"], ig["accounts_error"]), (None, None, None))
        self.assertEqual(len(ig["stories"]), 2)
        self.assertEqual(len(ig["posts"]), 2)
        status, ig = self.call("PUT", "/api/instagram", {"access_token": "EAAB-test_token.1", "account_id": "17841400000000000", "refresh_minutes": 30})
        self.assertEqual(status, 200)
        self.assertTrue(ig["token_set"])
        self.assertNotIn("access_token", ig)
        self.assertEqual(self.call("PUT", "/api/instagram", {"refresh_minutes": 4})[0], 400)
        self.assertEqual(self.call("PUT", "/api/instagram", {"account_id": "abc"})[0], 400)
        self.assertEqual(self.call("PUT", "/api/instagram", {"access_token": "has spaces"})[0], 400)

    def test_instagram_tiles_served_as_bmp(self):
        _, ig = self.call("GET", "/api/instagram")
        name = ig["stories"][0]["file"]
        self.assertRegex(name, r"^\d+/story-0\.bmp$")
        with urllib.request.urlopen(f"{self.base}/ig/{name}") as r:
            self.assertEqual(r.headers["Content-Type"], "image/bmp")
            data = r.read()
        self.assertEqual(data[:2], b"BM")
        self.assertEqual(int.from_bytes(data[18:22], "little", signed=True), 54)
        self.assertEqual(abs(int.from_bytes(data[22:26], "little", signed=True)), 96)
        gen = name.split("/")[0]
        for bad in ("..%2Fsettings.json", f"{gen}/..%2F..%2Fsettings.json", f"{gen}%2F..%2F..%2Fsettings.json",
                    "story-0.bmp", f"{gen}/story-0.gif", f"{gen}/x.bmp", f"..%2F{gen}/story-0.bmp",
                    f"{gen}/story-0.bmp%00", "%2E%2E/settings.json", "999/story-0.bmp"):
            with self.assertRaises(urllib.error.HTTPError, msg=bad) as cm:
                urllib.request.urlopen(f"{self.base}/ig/{bad}")
            cm.exception.close()
            self.assertEqual(cm.exception.code, 404, bad)

    def test_instagram_refresh_needs_a_token(self):
        status, body = self.call("POST", "/api/instagram/refresh")
        self.assertEqual((status, body["code"]), (400, "not_configured"))
        status, body = self.call("POST", "/api/instagram/accounts")
        self.assertEqual((status, body["code"]), (400, "not_configured"))
        self.assertEqual(self.call("GET", "/api/instagram/accounts")[0], 404)  # it's a POST now


class FirstRunTest(ApiBase):
    SET_PIN = False

    def test_every_write_needs_a_pin_first(self):
        self.assertFalse(self.call("GET", "/api/settings")[1]["pin_set"])
        writes = [("PUT", "/api/settings", {"brightness": 40}),
                  ("PUT", "/api/settings", {"new_pin": PIN, "brightness": 40}),
                  ("PUT", "/api/athletes", {"rev": 1, "athletes": []}),
                  ("PUT", "/api/playlist", {"rev": 1, "slides": []}),
                  ("PUT", "/api/instagram", {"enabled": False}),
                  ("POST", "/api/instagram/refresh", None), ("POST", "/api/instagram/accounts", None),
                  ("POST", "/api/control", {"action": "pause"}), ("DELETE", "/api/media/strength-tank-logo.gif", None)]
        for method, path, body in writes:
            status, out = self.call(method, path, body, pin="123456")
            self.assertEqual(status, 403, (method, path))
            self.assertEqual(out, {"error": "Set an admin PIN first", "code": "pin_not_set"})
        status, out = self.upload("a.gif")
        self.assertEqual((status, out["code"]), (403, "pin_not_set"))
        self.assertEqual(self.call("PUT", "/api/settings", {"new_pin": "12345"})[0], 400)
        status, out = self.call("PUT", "/api/settings", {"new_pin": PIN})
        self.assertEqual(status, 200)
        self.assertTrue(out["pin_set"])
        self.assertEqual(self.call("PUT", "/api/settings", {"brightness": 40}, pin=False)[0], 401)
        self.assertEqual(self.call("PUT", "/api/settings", {"brightness": 40}, pin=PIN)[0], 200)


class LockoutTest(ApiBase):
    def setUp(self):
        super().setUp()
        self.t = [1000.0]
        self.store.clock = lambda: self.t[0]

    def wrong(self):
        return self.request("PUT", "/api/settings", {"brightness": 40}, pin="000000")

    def right(self):
        return self.request("PUT", "/api/settings", {"brightness": 40})

    def assertLocked(self, result, seconds):
        status, body, headers = result
        self.assertEqual(status, 429, body)
        self.assertEqual(body["code"], "locked")
        self.assertEqual(body["retry_after_s"], seconds)
        self.assertEqual(headers["Retry-After"], str(seconds))

    def test_client_lock_doubles_and_resets(self):
        for _ in range(4):
            self.assertEqual(self.wrong()[0], 401)
        self.assertEqual(self.right()[0], 200)  # a correct PIN resets the count
        for _ in range(4):
            self.assertEqual(self.wrong()[0], 401)
        self.assertEqual(self.wrong()[1]["code"], "pin_wrong")  # 5th wrong: answered, then locked
        self.assertLocked(self.right(), 30)  # locked: even the right PIN isn't checked
        self.t[0] += 29.5
        self.assertLocked(self.right(), 1)
        self.t[0] += 0.5
        self.assertEqual(self.wrong()[0], 401)  # 6th wrong: 60 s
        self.assertLocked(self.wrong(), 60)
        self.t[0] += 60
        self.assertEqual(self.wrong()[0], 401)  # 7th: 120 s
        self.assertLocked(self.right(), 120)
        for _ in range(6):  # 240, 480, 900 (cap), 900 ...
            self.t[0] += 1000
            self.wrong()
        self.assertLocked(self.right(), 900)
        self.t[0] += 900
        self.assertEqual(self.right()[0], 200)
        for _ in range(4):
            self.assertEqual(self.wrong()[0], 401)
        self.assertEqual(self.right()[0], 200)

    def test_missing_pin_is_not_a_wrong_pin(self):
        for _ in range(12):
            status, body, _ = self.request("PUT", "/api/settings", {"brightness": 40}, pin=False)
            self.assertEqual(body["code"], "pin_required")

    def test_global_lock(self):
        g = self.store.guard
        for i in range(19):
            g.fail(f"10.0.0.{i}")
        self.assertEqual(self.right()[0], 200)
        self.t[0] += 601  # the first 19 have aged out of the 10 minute window
        for i in range(19):
            g.fail(f"10.0.1.{i}")
        self.assertEqual(self.right()[0], 200)
        g.fail("10.0.2.1")  # 20 within 10 minutes
        self.assertLocked(self.right(), 900)
        self.assertLocked(self.request("POST", "/api/control", {"action": "next"}), 900)
        self.assertEqual(self.call("GET", "/api/status")[0], 200)  # reads stay open
        self.t[0] += 900
        self.assertEqual(self.right()[0], 200)

    def test_client_table_is_bounded(self):
        g = self.store.guard
        for i in range(500):
            g.fail(f"10.1.{i // 250}.{i % 250}")
            self.t[0] += 31  # stay under the global limit
        self.assertLessEqual(len(g.clients), mock_server.MAX_CLIENTS)


class RevisionTest(ApiBase):
    def test_athletes_rev(self):
        rev = self.rev("athletes")
        status, body = self.put_athletes([{"id": "a1", "name": "x"}], rev=rev)
        self.assertEqual((status, body["rev"]), (200, rev + 1))
        self.assertEqual(self.rev("athletes"), rev + 1)
        for stale in (rev, rev + 2, None, "2"):
            status, body = self.call("PUT", "/api/athletes", {"rev": stale, "athletes": [{"id": "a2", "name": "y"}]}
                                     if stale is not None else {"athletes": []})
            self.assertEqual(status, 409, stale)
            self.assertEqual(body["code"], "stale")
            self.assertEqual(body["error"], "Someone else saved changes. Reload to see them.")
            self.assertEqual(body["rev"], rev + 1)
        self.assertEqual([a["id"] for a in self.call("GET", "/api/athletes")[1]["athletes"]], ["a1"])

    def test_playlist_rev(self):
        rev = self.rev("playlist")
        status, body = self.put_slides([], rev=rev)
        self.assertEqual((status, body["rev"]), (200, rev + 1))
        status, body = self.put_slides([{"id": "s9", "type": "whiteboard", "duration_s": 9}], rev=rev)
        self.assertEqual((status, body["code"]), (409, "stale"))
        self.assertEqual(self.call("GET", "/api/playlist")[1], {"rev": rev + 1, "slides": []})


class PrDateTest(ApiBase):
    # 03:30 UTC on 7 Nov 2026 is still 6 Nov in California (PST, UTC-8).
    NOW = datetime(2026, 11, 7, 3, 30, tzinfo=timezone.utc)

    def setUp(self):
        super().setUp()
        self.store.utcnow = lambda: self.NOW
        self.base_athletes = [{"id": "a1", "name": "Ann", "snatch": 80, "clean_jerk": None, "total": None,
                               "back_squat": 150, "front_squat": None, "notes": "", "visible": True}]
        status, body = self.put_athletes(self.base_athletes)
        self.assertEqual(status, 200)
        self.assertEqual(body["athletes"][0]["pr_dates"], {})  # new athletes start with none

    def ann(self, **lifts):
        return [self.base_athletes[0] | lifts]

    def test_stamped_in_the_wall_zone(self):
        self.assertEqual(self.call("GET", "/api/settings")[1]["timezone"], "America/Los_Angeles")
        self.assertEqual(self.call("GET", "/api/status")[1]["local_date"], "2026-11-06")
        _, body = self.put_athletes(self.ann(snatch=82.5, clean_jerk=100))  # up, and filled in
        self.assertEqual(body["athletes"][0]["pr_dates"], {"snatch": "2026-11-06", "clean_jerk": "2026-11-06"})
        self.assertEqual(self.call("PUT", "/api/settings", {"timezone": "UTC"})[0], 200)
        self.assertEqual(self.call("GET", "/api/status")[1]["local_date"], "2026-11-07")
        _, body = self.put_athletes(self.ann(snatch=83, clean_jerk=100))
        self.assertEqual(body["athletes"][0]["pr_dates"], {"snatch": "2026-11-07", "clean_jerk": "2026-11-06"})
        self.assertEqual(self.call("PUT", "/api/settings", {"timezone": "Pacific/Honolulu"})[0], 200)
        self.assertEqual(self.call("GET", "/api/status")[1]["local_date"], "2026-11-06")

    def test_lower_or_same_keeps_dates_and_client_dates_are_ignored(self):
        self.put_athletes(self.ann(snatch=85))
        forged = {"snatch": "2020-01-01", "back_squat": "2026-11-06"}
        _, body = self.put_athletes([self.ann(snatch=70, back_squat=150)[0] | {"pr_dates": forged}])
        self.assertEqual(body["athletes"][0]["pr_dates"], {"snatch": "2026-11-06"})
        _, body = self.put_athletes([self.ann(snatch=None)[0]])
        self.assertEqual(body["athletes"][0]["pr_dates"], {"snatch": "2026-11-06"})
        new = {"id": "a2", "name": "Bo", "snatch": 60, "pr_dates": {"snatch": "2026-11-06"}}
        _, body = self.put_athletes(self.ann(snatch=None) + [new])
        self.assertEqual(body["athletes"][1]["pr_dates"], {})

    def test_previous_value_recorded_with_the_date(self):
        self.assertEqual(self.call("GET", "/api/athletes")[1]["athletes"][0]["pr_prev"], {})
        _, body = self.put_athletes(self.ann(snatch=82.5, clean_jerk=100))  # up from 80, and filled in
        self.assertEqual(body["athletes"][0]["pr_prev"], {"snatch": 80, "clean_jerk": None})
        stored = json.loads((self.data / "athletes.json").read_text())["athletes"][0]
        self.assertEqual((stored["pr_dates"], stored["pr_prev"]),
                         ({"snatch": "2026-11-06", "clean_jerk": "2026-11-06"}, {"snatch": 80, "clean_jerk": None}))
        self.assertEqual(self.call("GET", "/api/athletes")[1]["athletes"][0]["pr_prev"], body["athletes"][0]["pr_prev"])

        # Down, same, cleared: nothing stamped, the previous values are kept.
        for change in ({"snatch": 70}, {"snatch": 70, "back_squat": 150}, {"snatch": None}):
            _, body = self.put_athletes(self.ann(clean_jerk=100, **change))
            self.assertEqual(body["athletes"][0]["pr_prev"], {"snatch": 80, "clean_jerk": None}, change)
        # Filled in again after being cleared: there was nothing stored to replace.
        _, body = self.put_athletes(self.ann(snatch=75, clean_jerk=100))
        self.assertEqual(body["athletes"][0]["pr_prev"], {"snatch": None, "clean_jerk": None})
        # A second PR the same day records what that one replaced.
        _, body = self.put_athletes(self.ann(snatch=77.5, clean_jerk=100.5))
        self.assertEqual(body["athletes"][0]["pr_prev"], {"snatch": 75, "clean_jerk": 100})

    def test_client_previous_values_are_ignored(self):
        forged = {"snatch": 1, "back_squat": 2, "bench": 3}
        _, body = self.put_athletes([self.ann(snatch=90)[0] | {"pr_prev": forged}])
        self.assertEqual(body["athletes"][0]["pr_prev"], {"snatch": 80})
        _, body = self.put_athletes([self.ann(snatch=90)[0] | {"pr_prev": {}}])
        self.assertEqual(body["athletes"][0]["pr_prev"], {"snatch": 80})
        new = {"id": "a2", "name": "Bo", "snatch": 60, "pr_prev": {"snatch": 50}}
        _, body = self.put_athletes(self.ann(snatch=90) + [new])
        self.assertEqual(body["athletes"][1]["pr_prev"], {})

    def test_bad_timezone_rejected(self):
        for tz in ("Europe/London", "PST", "", None):
            self.assertEqual(self.call("PUT", "/api/settings", {"timezone": tz})[0], 400, tz)

    def test_no_stamp_without_valid_time(self):
        self.store.time_valid = False
        status = self.call("GET", "/api/status")[1]
        self.assertEqual((status["time_valid"], status["local_date"]), (False, None))
        _, body = self.put_athletes(self.ann(snatch=90))
        self.assertEqual(body["athletes"][0]["snatch"], 90)
        self.assertEqual(body["athletes"][0]["pr_dates"], {})
        self.assertEqual(body["athletes"][0]["pr_prev"], {})
        self.store.time_valid = True
        self.put_athletes(self.ann(snatch=95))
        self.store.time_valid = False
        _, body = self.put_athletes(self.ann(snatch=100))  # kept as they were, not restamped
        self.assertEqual((body["athletes"][0]["pr_dates"], body["athletes"][0]["pr_prev"]),
                         ({"snatch": "2026-11-06"}, {"snatch": 90}))


class InstagramJobTest(ApiBase):
    def setUp(self):
        super().setUp()
        status, _ = self.call("PUT", "/api/instagram", {"access_token": "EAAB-test", "account_id": "17841400000000001"})
        self.assertEqual(status, 200)

    def gens(self):
        return sorted(p.name for p in (self.data / "ig").iterdir())

    def tile(self, file):
        with urllib.request.urlopen(f"{self.base}/ig/{file}") as r:
            return r.read()

    def test_refresh_runs_in_the_background(self):
        gate = threading.Event()
        self.store.http_get = FakeInstagram(gate=gate)
        before = self.call("GET", "/api/instagram")[1]
        old_gen = before["stories"][0]["file"].split("/")[0]
        t0 = time.time()
        self.assertEqual(self.call("POST", "/api/instagram/refresh"), (202, {"queued": True}))
        self.assertLess(time.time() - t0, 2)
        ig = self.call("GET", "/api/instagram")[1]
        self.assertEqual(ig["busy"], "refresh")
        self.assertEqual(ig["stories"], before["stories"])  # old tiles until the new ones are complete
        for path in ("/api/instagram/refresh", "/api/instagram/accounts"):
            status, body = self.call("POST", path)
            self.assertEqual((status, body["code"]), (409, "busy"), path)
        self.assertEqual(self.call("PUT", "/api/settings", {"brightness": 45})[0], 200)  # other writes aren't blocked
        gate.set()
        ig = self.wait_idle()
        self.assertIsNone(ig["last_error"])
        self.assertFalse(ig["sample"])
        self.assertIsNotNone(ig["last_fetch"])
        self.assertEqual(len(ig["stories"]), 2)
        self.assertEqual([p["kind"] for p in ig["posts"]], ["reel", "image"])
        new_gen = ig["stories"][0]["file"].split("/")[0]
        self.assertNotEqual(new_gen, old_gen)
        for item in ig["stories"] + ig["posts"]:
            self.assertRegex(item["file"], rf"^{new_gen}/(story|post)-\d\.bmp$")
            self.assertEqual(self.tile(item["file"])[:2], b"BM")
        self.assertEqual(self.gens(), [new_gen])  # the old generation was deleted

    def test_failed_refresh_keeps_the_previous_generation(self):
        import urllib.error as ue
        self.store.http_get = FakeInstagram()
        self.call("POST", "/api/instagram/refresh")
        good = self.wait_idle()
        self.assertIsNone(good["last_error"])
        good_gen = good["stories"][0]["file"].split("/")[0]
        failures = [("story1", ue.HTTPError("https://cdn.example/story1.jpg", 403, "Forbidden", {}, None)),
                    ("post1", ue.URLError("timed out")),
                    ("story0", "garbage"),
                    ("post0", RuntimeError("anything at all")),
                    ("post1", ValueError("bad url"))]
        for i, fail in enumerate(failures):
            self.store.utcnow = lambda i=i: datetime(2026, 11, 6, 12, i, tzinfo=timezone.utc)
            self.store.http_get = FakeInstagram(fail=fail)
            self.assertEqual(self.call("POST", "/api/instagram/refresh")[0], 202)
            ig = self.wait_idle()
            self.assertTrue(ig["last_error"], fail)
            self.assertNotIn("access_token", ig["last_error"])
            self.assertEqual(ig["last_fetch"], f"2026-11-06T12:{i:02d}:00Z")
            self.assertEqual(ig["stories"], good["stories"], fail)
            self.assertEqual(ig["posts"], good["posts"], fail)
            self.assertEqual(self.gens(), [good_gen], fail)
            for item in ig["stories"] + ig["posts"]:
                self.assertEqual(self.tile(item["file"])[:2], b"BM")
        errors = self.call("GET", "/api/status")[1]["recent_errors"]
        self.assertEqual(len(errors), len(failures))
        self.assertTrue(all(set(e) == {"time", "message"} for e in errors))

    def test_failed_graph_call_keeps_the_previous_generation(self):
        def down(url):
            raise urllib.error.URLError("no route to host")
        before = self.call("GET", "/api/instagram")[1]
        self.store.http_get = down
        self.call("POST", "/api/instagram/refresh")
        ig = self.wait_idle()
        self.assertEqual(ig["last_error"], "Couldn't reach Instagram: no route to host")
        self.assertEqual(ig["stories"], before["stories"])
        self.assertTrue(ig["sample"])

    def test_find_accounts_job(self):
        gate = threading.Event()
        self.store.http_get = FakeInstagram(gate=gate)
        self.assertEqual(self.call("POST", "/api/instagram/accounts"), (202, {"queued": True}))
        self.assertEqual(self.call("GET", "/api/instagram")[1]["busy"], "accounts")
        self.assertEqual(self.call("POST", "/api/instagram/refresh")[1]["code"], "busy")
        gate.set()
        ig = self.wait_idle()
        self.assertEqual(ig["accounts"], [{"page": "The Strength Tank", "id": "17841400000000001", "username": "thestrengthtank"}])
        self.assertIsNone(ig["accounts_error"])

        def refused(url):
            raise urllib.error.URLError("refused")
        self.store.http_get = refused
        self.call("POST", "/api/instagram/accounts")
        ig = self.wait_idle()
        self.assertIsNone(ig["accounts"])
        self.assertEqual(ig["accounts_error"], "Couldn't reach Instagram: refused")
        self.call("PUT", "/api/instagram", {"access_token": "EAAB-other"})  # a new token forgets the old answer
        self.assertIsNone(self.call("GET", "/api/instagram")[1]["accounts_error"])


class StorageTest(ApiBase):
    def test_reserve_and_leftovers(self):
        media = self.call("GET", "/api/media")[1]["storage"]
        self.assertEqual(media["total_bytes"], 23 * 1024 * 1024)
        self.assertEqual(media["usable_bytes"], int(23 * 1024 * 1024 * 0.9))
        self.assertEqual(self.call("GET", "/api/status")[1]["storage"]["usable_bytes"], media["usable_bytes"])
        # Shrink the disk so it's nearly at the 90% line, then go past it but stay under 100%.
        used = media["used_bytes"]
        old = mock_server.STORAGE_BYTES
        self.addCleanup(setattr, mock_server, "STORAGE_BYTES", old)
        mock_server.STORAGE_BYTES = int((used + 10_000) / 0.9) + 1
        self.assertEqual(self.upload("small.gif", gif(pad=5_000))[0], 200)
        status, body = self.upload("big.gif", gif(pad=6_000))
        storage = self.call("GET", "/api/media")[1]["storage"]
        self.assertLess(storage["used_bytes"] + 6_000, storage["total_bytes"])  # it would fit on the disk
        self.assertEqual((status, body["code"]), (507, "no_space"))
        self.assertFalse((self.data / "media" / "big.gif").exists())

    def test_part_files_removed_at_start(self):
        (self.data / "media" / "half.part").write_bytes(b"x" * 100)
        (self.data / "athletes.tmp").write_text("{")
        self.restart()
        self.assertFalse((self.data / "media" / "half.part").exists())
        self.assertFalse((self.data / "athletes.tmp").exists())


class CorruptConfigTest(ApiBase):
    def test_corrupt_files_at_start(self):
        self.assertEqual(self.upload("kept.gif", query="&frames=4&peak_a=3")[0], 200)
        (self.data / "athletes.json").write_text('{"rev": 3, "athletes": [')
        (self.data / "playlist.json").write_text('[1, 2]')  # valid JSON, wrong shape
        (self.data / "media_index.json").write_bytes(b"\xff\xfe garbage")
        self.restart()
        status = self.call("GET", "/api/status")[1]
        self.assertEqual(sorted(e["file"] for e in status["config_errors"]),
                         ["athletes.json", "media_index.json", "playlist.json"])
        self.assertTrue(all(e["renamed_to"] == e["file"] + ".bad" for e in status["config_errors"]))
        self.assertEqual((self.data / "athletes.json.bad").read_text(), '{"rev": 3, "athletes": [')
        self.assertEqual(self.call("GET", "/api/athletes")[1], {"rev": 1, "athletes": []})
        self.assertEqual(self.call("GET", "/api/playlist")[1], {"rev": 1, "slides": []})
        files = {f["name"]: f for f in self.call("GET", "/api/media")[1]["files"]}
        self.assertEqual(set(files), {"kept.gif", "strength-tank-logo.gif"})
        self.assertIsNone(files["kept.gif"]["frames"])
        self.assertIsNone(files["kept.gif"]["peak_a"])
        self.assertEqual(json.loads((self.data / "media_index.json").read_text())["kept.gif"]["frames"], None)
        self.assertEqual(self.call("GET", "/api/settings")[1]["pin_set"], True)  # settings.json was fine
        self.assertEqual(self.put_athletes([{"id": "a1", "name": "x"}], rev=1)[0], 200)

    def test_corrupt_settings_while_running_drops_the_pin(self):
        (self.data / "settings.json").write_text("not json")
        settings = self.call("GET", "/api/settings")[1]
        self.assertFalse(settings["pin_set"])
        self.assertEqual(settings["timezone"], "America/Los_Angeles")
        self.assertEqual([e["file"] for e in self.call("GET", "/api/status")[1]["config_errors"]], ["settings.json"])
        self.assertEqual(self.call("PUT", "/api/settings", {"brightness": 40})[1]["code"], "pin_not_set")


class StatusTest(ApiBase):
    def test_diagnostics(self):
        s = self.call("GET", "/api/status")[1]
        self.assertTrue(s["mock"])
        for key, kind in [("reset_reason", str), ("min_free_heap", int), ("refresh_hz", (int, float)),
                          ("limiter_scale", (int, float)), ("recent_errors", list), ("fw_version", str),
                          ("ui_version", str), ("time_valid", bool), ("local_date", str), ("config_errors", list),
                          ("uptime_s", int), ("paused", bool)]:
            self.assertIsInstance(s[key], kind, key)
        self.assertRegex(s["local_date"], r"^\d{4}-\d{2}-\d{2}$")
        self.assertTrue(0 < s["limiter_scale"] <= 1)
        self.assertEqual(s["now_playing"], {"slide_id": "s0", "type": "media", "file": "strength-tank-logo.gif"})
        self.assertEqual(set(s["storage"]), {"used_bytes", "total_bytes", "usable_bytes"})
        self.put_slides([])
        self.assertIsNone(self.call("GET", "/api/status")[1]["now_playing"])

    def test_recent_errors_keep_the_last_20(self):
        for i in range(25):
            self.store.note_error(f"error {i}")
        errors = self.call("GET", "/api/status")[1]["recent_errors"]
        self.assertEqual([e["message"] for e in errors], [f"error {i}" for i in range(5, 25)])


LA = ZoneInfo("America/Los_Angeles")
WEEKDAYS = [{"start": "05:00", "end": "21:00"}]
# Mon to Fri 05:00-21:00, Saturday two ranges, Sunday off.
WEEK = {"enabled": True, "days": {**{d: WEEKDAYS for d in ("mon", "tue", "wed", "thu", "fri")},
                                  "sat": [{"start": "16:00", "end": "18:00"}, {"start": "08:00", "end": "12:00"}],
                                  "sun": []}}


def la(*args):
    return datetime(*args, tzinfo=LA)


class ScheduleTest(ApiBase):
    """settings.schedule, wake/sleep and the status fields (API.md 7.4), on an injected clock."""

    def setUp(self):
        super().setUp()
        self.now = la(2026, 11, 6, 20, 0)  # a Friday evening, PST
        self.store.utcnow = lambda: self.now.astimezone(timezone.utc)

    def state(self):
        s = self.call("GET", "/api/status")[1]
        return s["display_on"], s["schedule_state"], s["next_change"]

    def control(self, action):
        self.assertEqual(self.call("POST", "/api/control", {"action": action}), (200, {"ok": True}))

    def put_schedule(self, schedule):
        return self.call("PUT", "/api/settings", {"schedule": schedule})

    def test_defaults(self):
        s = self.call("GET", "/api/settings")[1]
        self.assertEqual(s["schedule"], {"enabled": False, "days": {d: [{"start": "05:00", "end": "21:00"}] for d in
                                                                     ("mon", "tue", "wed", "thu", "fri", "sat", "sun")}})
        self.assertEqual(s["power"], {"supplies": 2, "rated_a": 18, "budget_pct": 85})
        self.assertEqual(list(s), ["title", "brightness", "sort", "pr_highlight_days", "timezone", "schedule", "power",
                                   "pin_set"])
        self.assertEqual(self.state(), (True, "always_on", None))

    def test_validation(self):
        day = lambda d, windows: {"enabled": True, "days": WEEK["days"] | {d: windows}}  # noqa: E731
        cases = [
            ("x", "Schedule must be an object with enabled and days", None),
            ({"enabled": 1, "days": WEEK["days"]}, "Schedule enabled must be true or false", None),
            ({"enabled": True}, "Schedule days must list mon, tue, wed, thu, fri, sat and sun", None),
            ({"enabled": True, "days": {k: v for k, v in WEEK["days"].items() if k != "sun"}},
             "Schedule days must list mon, tue, wed, thu, fri, sat and sun", None),
            (day("tue", WEEKDAYS * 3), "Tuesday: give a list of 0 to 2 time ranges", "tue"),
            (day("wed", {"start": "05:00", "end": "21:00"}), "Wednesday: give a list of 0 to 2 time ranges", "wed"),
            (day("thu", [{"start": "5:00", "end": "21:00"}]),
             "Thursday: times must be HH:MM, 00:00 to 23:59 (an end can be 24:00)", "thu"),
            (day("thu", [{"start": "24:00", "end": "24:00"}]),
             "Thursday: times must be HH:MM, 00:00 to 23:59 (an end can be 24:00)", "thu"),
            (day("thu", [{"start": "05:00", "end": "24:01"}]),
             "Thursday: times must be HH:MM, 00:00 to 23:59 (an end can be 24:00)", "thu"),
            (day("thu", [{"start": "05:00"}]), "Thursday: times must be HH:MM, 00:00 to 23:59 (an end can be 24:00)", "thu"),
            (day("thu", ["05:00-21:00"]), "Thursday: times must be HH:MM, 00:00 to 23:59 (an end can be 24:00)", "thu"),
            (day("fri", [{"start": "09:00", "end": "09:00"}]), "Friday: a time range must end after it starts", "fri"),
            (day("fri", [{"start": "21:00", "end": "05:00"}]), "Friday: a time range must end after it starts", "fri"),
            (day("sun", [{"start": "05:00", "end": "12:00"}, {"start": "11:59", "end": "13:00"}]),
             "Sunday: the two time ranges overlap", "sun"),
        ]
        before = self.call("GET", "/api/settings")[1]
        for body, message, d in cases:
            status, err = self.put_schedule(body)
            want = {"error": message, "code": "invalid"} | ({"day": d} if d else {})
            self.assertEqual((status, err), (400, want), body)
        self.assertEqual(self.call("GET", "/api/settings")[1], before)
        # Touching ranges are fine, 24:00 ends a day, ranges come back sorted, extra keys go.
        ok = day("sun", [{"start": "12:00", "end": "24:00", "note": "x"}, {"start": "00:00", "end": "12:00"}])
        status, saved = self.put_schedule(ok | {"extra": 1})
        self.assertEqual(status, 200, saved)
        self.assertEqual(saved["schedule"]["days"]["sun"], [{"start": "00:00", "end": "12:00"}, {"start": "12:00", "end": "24:00"}])
        self.assertEqual(saved["schedule"]["days"]["sat"], [{"start": "08:00", "end": "12:00"}, {"start": "16:00", "end": "18:00"}])
        self.assertNotIn("extra", saved["schedule"])

    def test_states_through_a_week(self):
        self.assertEqual(self.put_schedule(WEEK)[0], 200)
        expect = [
            (la(2026, 11, 6, 20, 0), (True, "on", "2026-11-06T21:00:00-08:00")),
            (la(2026, 11, 6, 21, 0), (False, "off", "2026-11-07T08:00:00-08:00")),
            (la(2026, 11, 7, 13, 0), (False, "off", "2026-11-07T16:00:00-08:00")),
            (la(2026, 11, 7, 17, 59, 59), (True, "on", "2026-11-07T18:00:00-08:00")),
            (la(2026, 11, 7, 19, 0), (False, "off", "2026-11-09T05:00:00-08:00")),  # Sunday is off
            (la(2026, 11, 9, 4, 59), (False, "off", "2026-11-09T05:00:00-08:00")),
        ]
        for when, state in expect:
            self.now = when
            self.assertEqual(self.state(), state, when)
        self.assertEqual(self.call("PUT", "/api/settings", {"timezone": "America/New_York"})[0], 200)
        self.assertEqual(self.state(), (True, "on", "2026-11-09T21:00:00-05:00"))  # 07:59 in New York

    def test_no_clock_fails_open(self):
        self.put_schedule(WEEK | {"days": {d: [] for d in WEEK["days"]}})
        self.assertEqual(self.state(), (False, "off", None))  # every day off: no change ahead
        self.store.time_valid = False
        self.assertEqual(self.state(), (True, "no_clock", None))

    def test_wake_lasts_until_the_next_scheduled_change(self):
        self.put_schedule(WEEK)
        self.now = la(2026, 11, 6, 22, 0)
        self.control("wake")
        # On by hand until the schedule turns on at 08:00, then the schedule keeps it on to 12:00.
        self.assertEqual(self.state(), (True, "override_on", "2026-11-07T12:00:00-08:00"))
        self.now = la(2026, 11, 7, 7, 59, 59)
        self.assertEqual(self.state(), (True, "override_on", "2026-11-07T12:00:00-08:00"))
        self.now = la(2026, 11, 7, 8, 0)
        self.assertEqual(self.state(), (True, "on", "2026-11-07T12:00:00-08:00"))
        self.now = la(2026, 11, 7, 12, 0)
        self.assertEqual(self.state(), (False, "off", "2026-11-07T16:00:00-08:00"))
        self.assertEqual(self.call("GET", "/api/status")[1]["last_action"], "wake")

    def test_sleep_lasts_until_the_next_scheduled_change(self):
        self.put_schedule(WEEK)
        self.control("sleep")
        self.assertEqual(self.state(), (False, "override_off", "2026-11-07T08:00:00-08:00"))
        self.now = la(2026, 11, 6, 21, 0)
        self.assertEqual(self.state(), (False, "off", "2026-11-07T08:00:00-08:00"))
        self.now = la(2026, 11, 7, 8, 0)
        self.assertEqual(self.state(), (True, "on", "2026-11-07T12:00:00-08:00"))

    def test_wake_while_on_and_saving_a_schedule_clear_the_override(self):
        self.put_schedule(WEEK)
        self.control("wake")  # already on: nothing to override
        self.assertEqual(self.state(), (True, "on", "2026-11-06T21:00:00-08:00"))
        self.control("sleep")
        self.assertEqual(self.state()[1], "override_off")
        self.control("wake")
        self.assertEqual(self.state()[1], "on")
        self.control("sleep")
        self.assertEqual(self.put_schedule(WEEK)[0], 200)
        self.assertEqual(self.state(), (True, "on", "2026-11-06T21:00:00-08:00"))
        self.control("sleep")
        self.assertEqual(self.call("PUT", "/api/settings", {"title": "OTHER"})[0], 200)  # not a schedule save
        self.assertEqual(self.state()[1], "override_off")

    def test_sleep_without_a_schedule_lasts_until_wake(self):
        self.control("sleep")
        self.assertEqual(self.state(), (False, "override_off", None))
        self.now += timedelta(days=30)
        self.assertEqual(self.state(), (False, "override_off", None))
        self.control("wake")
        self.assertEqual(self.state(), (True, "always_on", None))
        self.store.time_valid = False
        self.put_schedule(WEEK)
        self.control("sleep")
        self.assertEqual(self.state(), (False, "override_off", None))

    def test_daylight_saving_and_midnight(self):
        sunday = lambda windows: WEEK | {"days": WEEK["days"] | {"sun": windows}}  # noqa: E731
        # Spring forward, Sunday 8 March 2026: 02:00 PST jumps to 03:00 PDT, so a range starting
        # at 02:30 begins at the jump.
        self.put_schedule(sunday([{"start": "02:30", "end": "05:00"}]))
        self.now = la(2026, 3, 8, 1, 0)
        self.assertEqual(self.state(), (False, "off", "2026-03-08T03:00:00-07:00"))
        self.now = la(2026, 3, 8, 3, 0)
        self.assertEqual(self.state(), (True, "on", "2026-03-08T05:00:00-07:00"))
        # Fall back, Sunday 1 November 2026: 01:30 to 01:45 happens twice.
        self.put_schedule(sunday([{"start": "01:30", "end": "01:45"}]))
        self.now = la(2026, 11, 1, 0, 0)
        self.assertEqual(self.state(), (False, "off", "2026-11-01T01:30:00-07:00"))
        self.now = datetime(2026, 11, 1, 8, 50, tzinfo=timezone.utc)  # 01:50 PDT
        self.assertEqual(self.state(), (False, "off", "2026-11-01T01:30:00-08:00"))
        # A range to 24:00 and one from 00:00 the next day are one stretch: no change at midnight.
        self.put_schedule(WEEK | {"days": WEEK["days"] | {"sat": [{"start": "20:00", "end": "24:00"}],
                                                           "sun": [{"start": "00:00", "end": "02:00"}]}})
        self.now = la(2026, 11, 7, 23, 0)
        self.assertEqual(self.state(), (True, "on", "2026-11-08T02:00:00-08:00"))

    def test_power_setting(self):
        for body, message in [
            ("x", "Power must be an object with supplies, rated_a and budget_pct"),
            ({"supplies": 3, "rated_a": 18, "budget_pct": 85}, "Power supplies must be 1 or 2"),
            ({"supplies": 1.0, "rated_a": 18, "budget_pct": 85}, "Power supplies must be 1 or 2"),
            ({"supplies": True, "rated_a": 18, "budget_pct": 85}, "Power supplies must be 1 or 2"),
            ({"supplies": 1, "rated_a": 4.9, "budget_pct": 85}, "Supply rating must be 5 to 100 A"),
            ({"supplies": 1, "rated_a": 100.5, "budget_pct": 85}, "Supply rating must be 5 to 100 A"),
            ({"supplies": 1, "rated_a": "80", "budget_pct": 85}, "Supply rating must be 5 to 100 A"),
            ({"supplies": 1, "rated_a": True, "budget_pct": 85}, "Supply rating must be 5 to 100 A"),
            ({"supplies": 1, "budget_pct": 85}, "Supply rating must be 5 to 100 A"),
            ({"supplies": 1, "rated_a": 80, "budget_pct": 96}, "Power budget must be 50 to 95 percent"),
            ({"supplies": 1, "rated_a": 80, "budget_pct": 49}, "Power budget must be 50 to 95 percent"),
            ({"supplies": 1, "rated_a": 80, "budget_pct": 85.0}, "Power budget must be 50 to 95 percent"),
        ]:
            self.assertEqual(self.call("PUT", "/api/settings", {"power": body}),
                             (400, {"error": message, "code": "invalid"}), body)
        status, s = self.call("PUT", "/api/settings", {"power": {"supplies": 1, "rated_a": 80, "budget_pct": 85, "x": 1}})
        self.assertEqual((status, s["power"]), (200, {"supplies": 1, "rated_a": 80, "budget_pct": 85}))
        status, s = self.call("PUT", "/api/settings", {"power": {"supplies": 2, "rated_a": 30.5, "budget_pct": 50}})
        self.assertEqual((status, s["power"]), (200, {"supplies": 2, "rated_a": 30.5, "budget_pct": 50}))
        self.restart()
        self.assertEqual(self.call("GET", "/api/settings")[1]["power"], {"supplies": 2, "rated_a": 30.5, "budget_pct": 50})


ANNOUNCEMENT_CASES = json.loads((Path(__file__).resolve().parent / "announcement_cases.json").read_text())["cases"]


class AnnouncementTest(ApiBase):
    def put(self, announcements, rev=None, pin=True):
        rev = self.rev("announcements") if rev is None else rev
        return self.call("PUT", "/api/announcements", {"rev": rev, "announcements": announcements}, pin=pin)

    def test_seeded_with_a_welcome_and_in_the_rotation(self):
        self.assertEqual(self.call("GET", "/api/announcements", pin=False), (200, {"rev": 1, "announcements": [
            {"id": "n1", "text": "Welcome to The Strength Tank", "enabled": True, "start_date": None, "end_date": None}]}))
        slides = self.call("GET", "/api/playlist")[1]["slides"]
        types = [x["type"] for x in slides]
        self.assertEqual(types[types.index("new_prs") + 1], "announcements")
        self.assertEqual(slides[types.index("announcements")]["duration_s"], 8)

    def test_validation_cases_shared_with_the_firmware(self):
        for case in ANNOUNCEMENT_CASES:
            with self.subTest(case["name"]):
                rev = self.rev("announcements")
                status, body = self.put(case["announcements"], rev)
                if "error" in case:
                    self.assertEqual((status, body), (400, {"error": case["error"], "code": "invalid"}))
                    self.assertEqual(self.rev("announcements"), rev, "nothing written")
                else:
                    self.assertEqual((status, body), (200, {"rev": rev + 1, "announcements": case["saved"]}))

    def test_shape_rev_and_pin(self):
        rev = self.rev("announcements")
        for bad in ({"rev": rev}, {"rev": rev, "announcements": {}}, [], None):
            self.assertEqual(self.call("PUT", "/api/announcements", bad),
                             (400, {"error": 'Expected {"rev": n, "announcements": [...]}', "code": "invalid"}), bad)
        # Shape, then rev, then contents: a stale save with bad contents is stale.
        for stale in (rev - 1, rev + 1, "1", None, True):
            status, body = self.call("PUT", "/api/announcements", {"rev": stale, "announcements": [{"id": "x"}]})
            self.assertEqual((status, body), (409, {"error": "Someone else saved changes. Reload to see them.",
                                                    "code": "stale", "rev": rev}), stale)
        self.assertEqual(self.put([], pin=False)[1]["code"], "pin_required")
        self.assertEqual(self.put([], pin="000000")[1]["code"], "pin_wrong")
        self.assertEqual(self.rev("announcements"), rev)
        status, body = self.put([{"id": "a", "text": "One"}], rev)
        self.assertEqual((status, body["rev"]), (200, rev + 1))
        self.assertEqual(self.put([{"id": "b", "text": "Two"}], rev)[0], 409)  # the same rev can't save twice
        self.restart()
        self.assertEqual(self.call("GET", "/api/announcements")[1]["announcements"][0]["text"], "One")

    def test_slide_type(self):
        for d, ok in [(5, True), (999, True), (8, True), (4, False), (1000, False), (8.5, False), (True, False)]:
            status, body = self.put_slides([{"id": "n", "type": "announcements", "duration_s": d}])
            self.assertEqual(status == 200, ok, f"duration {d!r}: {status} {body}")
        status, body = self.put_slides([{"id": "n", "type": "announcements", "duration_s": 12, "file": "x.gif", "enabled": False}])
        self.assertEqual((status, body["slides"]), (200, [{"id": "n", "type": "announcements", "duration_s": 12, "enabled": False}]))
        self.assertEqual(self.put_slides([{"id": "n", "type": "announcement", "duration_s": 8}])[1]["error"],
                         "Slide 1: type must be one of whiteboard, media, instagram_story, instagram_posts, new_prs, announcements")
        self.call("POST", "/api/control", {"action": "next"})
        self.put_slides([{"id": "n", "type": "announcements", "duration_s": 8}])
        self.assertEqual(self.call("GET", "/api/status")[1]["now_playing"],
                         {"slide_id": "n", "type": "announcements", "file": None})

    def test_corrupt_file_at_start(self):
        (self.data / "announcements.json").write_text('{"rev": 4, "announcements": [')
        self.restart()
        self.assertEqual(self.call("GET", "/api/announcements")[1], {"rev": 1, "announcements": []})
        self.assertEqual([e["file"] for e in self.call("GET", "/api/status")[1]["config_errors"]], ["announcements.json"])


class PinGuardTest(unittest.TestCase):
    def test_schedule(self):
        t = [0.0]
        g = mock_server.PinGuard(lambda: t[0])
        waits = []
        for _ in range(12):
            g.fail("a")
            waits.append(g.wait_s("a"))
            t[0] += 1000  # let each lock run out, staying under the global limit
        self.assertEqual(waits, [0, 0, 0, 0, 30, 60, 120, 240, 480, 900, 900, 900])
        self.assertEqual(g.wait_s("b"), 0)
        g.succeed("a")
        g.fail("a")
        self.assertEqual(g.wait_s("a"), 0)


if __name__ == "__main__":
    unittest.main()
