"""Black-box contract smoke test for a running wall (or mock_server.py), over HTTP only.

    WALL_URL=http://gymboard.local WALL_PIN=123456 python3 tests/device_contract.py --yes

It checks API.md from the outside: status fields, no CORS, the PIN rules, revisions,
validation messages, media upload/delete, path traversal, and the device-only firmware
route. It WRITES to the wall: it saves the athlete list and the rotation (restoring what
was there), uploads and deletes a small test GIF, and pauses then resumes the rotation.
It never touches the Instagram token, Wi-Fi or the PIN of a wall that already has one.

  --yes        required: confirms you accept the writes above
  --lockout    also test the lockout (locks writes from this computer for 30 s)
  --instagram  also start an Instagram refresh/lookup if a token is configured

Against the mock (proves the test itself):
  python3 -I mock_server.py --port 8095 --data "$(mktemp -d)" &
  WALL_URL=http://127.0.0.1:8095 WALL_PIN=135790 python3 tests/device_contract.py --yes
A fresh mock has no PIN; this test sets WALL_PIN as the first PIN there (the mock allows
that from anywhere; a real wall only from its setup Wi-Fi).
"""
import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.request

URL = os.environ.get("WALL_URL", "http://gymboard.local").rstrip("/")
PIN = os.environ.get("WALL_PIN", "")
TEST_GIF = "contract-test.gif"
TEST_ATHLETE = "contract-test"
LIFTS = ("snatch", "clean_jerk", "total", "back_squat", "front_squat")


def gif(w=256, h=96, pad=0):
    return b"GIF89a" + w.to_bytes(2, "little") + h.to_bytes(2, "little") + b"\x00\x00\x00" + b"\x00" * pad + b"\x3b"


def request(method, path, body=None, raw=None, pin=True, headers=None, timeout=30):
    """(status, parsed JSON or None, headers, raw bytes). pin: True = WALL_PIN, str = that PIN, False = none."""
    h = dict(headers or {})
    data = raw
    if body is not None:
        data = json.dumps(body).encode()
        h.setdefault("Content-Type", "application/json")
    if pin is True and PIN:
        h["X-Admin-Pin"] = PIN
    elif isinstance(pin, str):
        h["X-Admin-Pin"] = pin
    req = urllib.request.Request(URL + path, data=data, method=method, headers=h)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            payload = r.read()
            status, hdrs = r.status, r.headers
    except urllib.error.HTTPError as e:
        with e:
            payload = e.read()
            status, hdrs = e.code, e.headers
    try:
        parsed = json.loads(payload) if payload else None
    except ValueError:
        parsed = None
    return status, parsed, hdrs, payload


class Failed(Exception):
    pass


def check(cond, what):
    if not cond:
        raise Failed(what)


def expect(result, status, code=None, what=""):
    s, body, _, _ = result
    got = body.get("code") if isinstance(body, dict) else None
    check(s == status and (code is None or got == code), f"{what}: expected {status} {code or ''}, got {s} {str(body)[:200]}")
    return body


class Contract:
    def __init__(self, args):
        self.args = args
        self.status = None
        self.mock = False

    # ---- checks ---------------------------------------------------------------------------
    def t_status_fields(self):
        s, st, h, _ = request("GET", "/api/status", pin=False)
        check(s == 200, f"status {s}")
        self.status, self.mock = st, st.get("mock") is True
        kinds = {"mock": bool, "fw_version": str, "ui_version": str, "uptime_s": int, "reset_reason": str,
                 "paused": bool, "storage": dict, "wifi": dict, "time_valid": bool, "timezone": str,
                 "min_free_heap": int, "refresh_hz": (int, float), "limiter_scale": (int, float),
                 "config_errors": list, "recent_errors": list}
        for k, kind in kinds.items():
            check(isinstance(st.get(k), kind) and not (kind is int and isinstance(st.get(k), bool)), f"status.{k} = {st.get(k)!r}")
        check(st["reset_reason"] in ("poweron", "external", "software", "panic", "int_wdt", "task_wdt", "wdt",
                                     "deepsleep", "brownout", "sdio", "unknown"), f"reset_reason {st['reset_reason']}")
        check(0 <= st["limiter_scale"] <= 1, "limiter_scale in 0..1")
        check(set(st["storage"]) == {"used_bytes", "total_bytes", "usable_bytes"}, "storage keys")
        check(st["storage"]["usable_bytes"] == st["storage"]["total_bytes"] * 9 // 10, "usable = floor(90%)")
        check(st["local_date"] is None if not st["time_valid"] else isinstance(st["local_date"], str), "local_date vs time_valid")
        check(st["now_playing"] is None or set(st["now_playing"]) == {"slide_id", "type", "file"}, "now_playing shape")
        check(len(st["recent_errors"]) <= 20, "recent_errors <= 20")
        check(all(set(e) == {"time", "message"} for e in st["recent_errors"]), "recent_errors entries")
        check(h.get("Cache-Control") == "no-store", "Cache-Control: no-store")
        return f"{'mock' if self.mock else 'device'} fw {st['fw_version']}, ui {st['ui_version']}"

    def t_no_cors(self):
        for method, path, body in [("GET", "/api/status", None), ("GET", "/", None), ("PUT", "/api/settings", {"brightness": 4})]:
            _, _, h, _ = request(method, path, body, headers={"Origin": "http://evil.example"})
            check(not [k for k in h if k.lower().startswith("access-control-")], f"CORS header on {method} {path}")
        s, body, h, _ = request("OPTIONS", "/api/settings", pin=False,
                                headers={"Origin": "http://evil.example", "Access-Control-Request-Method": "PUT"})
        check(s == 405 and body == {"error": "Method not allowed", "code": "method_not_allowed"}, f"OPTIONS: {s} {body}")
        check(not [k for k in h if k.lower().startswith("access-control-")], "CORS header on OPTIONS")

    def t_console_served(self):
        s, _, h, data = request("GET", "/", pin=False)
        check(s == 200 and b"<html" in data.lower()[:2000] or b"<!doctype" in data.lower()[:100], f"console: {s}")
        s, _, _, _ = request("GET", "/app.js", pin=False)
        check(s == 200, f"/app.js {s}")

    def t_pin(self):
        s, settings, _, _ = request("GET", "/api/settings", pin=False)
        check(s == 200, "GET settings")
        check(not {"pin_hash", "pin_salt", "pin_iter"} & set(settings), "PIN fields exposed")
        if not settings["pin_set"]:
            body = expect(request("PUT", "/api/settings", {"brightness": 40}, pin=PIN), 403, "pin_not_set", "write before a PIN")
            check(body == {"error": "Set an admin PIN first", "code": "pin_not_set"}, f"pin_not_set body {body}")
            s, body, _, _ = request("PUT", "/api/settings", {"new_pin": PIN}, pin=False)
            if s == 403:
                check(body["error"] == "Set the admin PIN from the wall's setup Wi-Fi", body)
                raise Failed("no PIN is set and the wall only accepts the first PIN from its setup Wi-Fi: set it there, then rerun")
            check(s == 200 and body["pin_set"], f"first PIN: {s} {body}")
        expect(request("PUT", "/api/settings", {"brightness": 40}, pin=False), 401, "pin_required", "no PIN header")
        body = expect(request("PUT", "/api/settings", {"brightness": 40}, pin="000000" if PIN != "000000" else "111111"),
                      401, "pin_wrong", "wrong PIN")
        check(body["error"] == "That PIN is not right", body)
        s, body, _, _ = request("PUT", "/api/settings", {})  # the right PIN, no change: resets the count
        check(s == 200, f"right PIN: {s} {body} (is WALL_PIN right?)")
        for path in ("/api/settings", "/api/status", "/api/instagram"):
            text = json.dumps(request("GET", path, pin=False)[1])
            check("pin_hash" not in text and "pin_salt" not in text, f"PIN material in {path}")

    def t_settings(self):
        _, before, _, _ = request("GET", "/api/settings", pin=False)
        for bad, msg in [({"brightness": 4}, "Brightness must be 5 to 100"), ({"brightness": 50.0}, None),
                         ({"sort": "weight"}, "Sort must be one of board, total, name"),
                         ({"pr_highlight_days": 61}, "PR highlight must be 0 to 60 days"),
                         ({"title": "x" * 33}, "Title must be text up to 32 characters"),
                         ({"timezone": "Europe/London"}, None), ({"new_pin": "12345"}, "The PIN must be 6 to 12 digits"),
                         ({"title": "NEW", "brightness": 200}, None)]:
            body = expect(request("PUT", "/api/settings", bad), 400, "invalid", f"settings {bad}")
            check(msg is None or body["error"] == msg, f"message for {bad}: {body['error']}")
        _, after, _, _ = request("GET", "/api/settings", pin=False)
        check(after == before, "a rejected settings save changed something")
        new_bright = 45 if before["brightness"] != 45 else 46
        s, body, _, _ = request("PUT", "/api/settings", {"brightness": new_bright, "unknown": 1})
        check(s == 200 and body["brightness"] == new_bright and "unknown" not in body, f"settings save {s} {body}")
        s, body, _, _ = request("PUT", "/api/settings", {"brightness": before["brightness"]})
        check(s == 200 and body == before, "settings restore")

    def t_bodies(self):
        expect(request("PUT", "/api/settings", raw=b"{not json", headers={"Content-Type": "application/json"}), 400, "invalid", "bad JSON")
        expect(request("PUT", "/api/athletes", raw=b"[1, 2]", headers={"Content-Type": "application/json"}), 400, "invalid", "array body")
        big = json.dumps({"rev": 1, "athletes": [], "pad": "x" * (300 * 1024)}).encode()
        expect(request("PUT", "/api/athletes", raw=big, headers={"Content-Type": "application/json"}), 413, "too_large", "300 KB body")

    def t_athletes(self):
        _, cur, _, _ = request("GET", "/api/athletes", pin=False)
        rev, original = cur["rev"], cur["athletes"]
        check(isinstance(rev, int), "athletes rev is an int")
        for stale in (rev - 1, rev + 1, None, str(rev)):
            body = {"athletes": original} if stale is None else {"rev": stale, "athletes": original}
            out = expect(request("PUT", "/api/athletes", body), 409, "stale", f"stale rev {stale!r}")
            check(out == {"error": "Someone else saved changes. Reload to see them.", "code": "stale", "rev": rev}, out)
        expect(request("PUT", "/api/athletes", {"rev": rev, "athletes": {}}), 400, "invalid", "athletes not a list")
        base = {"id": TEST_ATHLETE, "name": "Contract Test", "notes": "", "visible": True}
        for v, ok in [(136.3, False), (0, False), (501, False), ("90", False), (True, False)]:
            expect(request("PUT", "/api/athletes", {"rev": rev, "athletes": original + [base | {"snatch": v}]}), 400, "invalid", f"snatch {v!r}")
        dup = [base, base | {"name": "y"}]
        out = expect(request("PUT", "/api/athletes", {"rev": rev, "athletes": dup}), 400, "invalid", "duplicate ids")
        check(out["error"] == "Athlete 2: missing or duplicate id", out)
        check(request("GET", "/api/athletes", pin=False)[1]["rev"] == rev, "a rejected save moved rev")

        try:
            s, out, _, _ = request("PUT", "/api/athletes", {"rev": rev, "athletes": original + [base | {"snatch": 80, "pr_dates": {"snatch": "2020-01-01"}}]})
            check(s == 200 and out["rev"] == rev + 1, f"save {s} {str(out)[:200]}")
            mine = out["athletes"][-1]
            check(mine["pr_dates"] == {} and mine["snatch"] == 80, f"new athlete {mine}")
            check(set(mine) == {"id", "name", "visible", "notes", "pr_dates", *LIFTS}, f"athlete keys {sorted(mine)}")
            s, out, _, _ = request("PUT", "/api/athletes", {"rev": rev + 1, "athletes": original + [base | {"snatch": 82.5}]})
            check(s == 200, f"second save {s}")
            st = request("GET", "/api/status", pin=False)[1]
            want = {"snatch": st["local_date"]} if st["time_valid"] else {}
            check(out["athletes"][-1]["pr_dates"] == want, f"PR stamp {out['athletes'][-1]['pr_dates']} vs {want}")
        finally:
            cur = request("GET", "/api/athletes", pin=False)[1]
            s, out, _, _ = request("PUT", "/api/athletes", {"rev": cur["rev"], "athletes": original})
            check(s == 200 and [a["id"] for a in out["athletes"]] == [a["id"] for a in original], "athletes restore")

    def t_playlist(self):
        _, cur, _, _ = request("GET", "/api/playlist", pin=False)
        rev, slides = cur["rev"], cur["slides"]
        for d in (4, 1000, 7.5, "10", True):
            expect(request("PUT", "/api/playlist", {"rev": rev, "slides": [{"id": "x", "type": "whiteboard", "duration_s": d}]}),
                   400, "invalid", f"duration {d!r}")
        out = expect(request("PUT", "/api/playlist", {"rev": rev, "slides": [{"id": "x", "type": "media", "file": "nope.gif", "duration_s": 10}]}),
                     400, "invalid", "unknown media")
        check(out["error"] == "Slide 1: media file 'nope.gif' is not on the board", out)
        expect(request("PUT", "/api/playlist", {"rev": rev, "slides": [{"id": "x", "type": "tiktok", "duration_s": 6}]}), 400, "invalid", "tiktok")
        expect(request("PUT", "/api/playlist", {"rev": rev + 5, "slides": slides}), 409, "stale", "stale playlist")
        s, out, _, _ = request("PUT", "/api/playlist", {"rev": rev, "slides": slides})
        check(s == 200 and out["rev"] == rev + 1 and out["slides"] == slides, f"playlist resave {s} {out}")

    def t_media(self):
        q = lambda name, extra="": f"/api/media?name={name}{extra}"  # noqa: E731
        g = {"Content-Type": "image/gif"}
        expect(request("POST", q("A.gif"), raw=gif(), headers=g), 400, "invalid", "bad name")
        out = expect(request("POST", q(TEST_GIF), raw=gif(64, 32), headers=g), 400, "invalid", "64x32")
        check(out["error"] == "GIF must be 256x96, got 64x32", out)
        out = expect(request("POST", q(TEST_GIF), raw=b"not a gif", headers=g), 400, "invalid", "not a GIF")
        check(out["error"] == "Not a GIF file", out)
        for bad in ("40.01", "-1", "nan", "abc", ""):
            expect(request("POST", q(TEST_GIF, f"&peak_a={bad}"), raw=gif(), headers=g), 400, "invalid", f"peak_a={bad}")
        expect(request("POST", q(TEST_GIF), raw=gif(), headers=g, pin=False), 401, "pin_required", "upload without PIN")
        files = {f["name"] for f in request("GET", "/api/media", pin=False)[1]["files"]}
        if TEST_GIF in files:  # left over from an interrupted run
            request("DELETE", f"/api/media/{TEST_GIF}")
        s, out, _, _ = request("POST", q(TEST_GIF, "&frames=2&peak_a=12.345"), raw=gif(pad=100), headers=g)
        check(s == 200 and out == {"name": TEST_GIF, "size": len(gif(pad=100)), "peak_a": 12.35}, f"upload {s} {out}")
        try:
            out = expect(request("POST", q(TEST_GIF), raw=gif(), headers=g), 409, "exists", "duplicate upload")
            check(out == {"error": f"{TEST_GIF} already exists", "code": "exists"}, out)
            listing = {f["name"]: f for f in request("GET", "/api/media", pin=False)[1]["files"]}
            check(listing[TEST_GIF]["frames"] == 2 and listing[TEST_GIF]["peak_a"] == 12.35, f"listing {listing[TEST_GIF]}")
            names = list(listing)
            check(names == sorted(names), "files sorted by name")
            s, _, h, data = request("GET", f"/media/{TEST_GIF}", pin=False)
            check(s == 200 and h.get("Content-Type", "").startswith("image/gif") and data == gif(pad=100), "GET /media/")
            # in_use: a disabled slide, so the wall never tries to play the header-only GIF
            pl = request("GET", "/api/playlist", pin=False)[1]
            slide = {"id": "contract-test", "type": "media", "file": TEST_GIF, "duration_s": 10, "enabled": False}
            s, out, _, _ = request("PUT", "/api/playlist", {"rev": pl["rev"], "slides": pl["slides"] + [slide]})
            check(s == 200, f"slide with the test GIF {s} {out}")
            try:
                expect(request("DELETE", f"/api/media/{TEST_GIF}"), 409, "in_use", "delete while in the rotation")
            finally:
                cur = request("GET", "/api/playlist", pin=False)[1]
                request("PUT", "/api/playlist", {"rev": cur["rev"], "slides": pl["slides"]})
        finally:
            s, out, _, _ = request("DELETE", f"/api/media/{TEST_GIF}")
            check(s == 200 and out == {"ok": True}, f"delete {s} {out}")
        expect(request("DELETE", f"/api/media/{TEST_GIF}"), 404, "not_found", "delete twice")
        expect(request("GET", f"/media/{TEST_GIF}", pin=False), 404, "not_found", "deleted GIF")

    def t_paths(self):
        for bad in ("/ig/..%2Fsettings.json", "/ig/1/..%2F..%2Fsettings.json", "/ig/1%2F..%2F..%2Fsettings.json",
                    "/ig/story-0.bmp", "/ig/1/story-0.gif", "/ig/1/x.bmp", "/ig/1/story-0.bmp%00", "/ig/%2E%2E/settings.json",
                    "/ig/999999999/story-0.bmp", "/media/..%2Fsettings.json", "/media/settings.json"):
            s, body, _, _ = request("GET", bad, pin=False)
            check(s == 404 and isinstance(body, dict) and body.get("code") == "not_found", f"{bad}: {s}")
        ig = request("GET", "/api/instagram", pin=False)[1]
        for item in ig["stories"][:1] + ig["posts"][:1]:
            s, _, h, data = request("GET", f"/ig/{item['file']}", pin=False)
            check(s == 200 and h.get("Content-Type") == "image/bmp" and data[:2] == b"BM", f"tile {item['file']}")
        out = expect(request("GET", "/api/nope", pin=False), 404, "not_found", "unknown GET")
        check(out["error"] == "No route for GET /api/nope", out)
        expect(request("POST", "/api/nope", {}), 404, "not_found", "unknown POST with PIN")
        expect(request("POST", "/api/nope", {}, pin=False), 401, "pin_required", "unknown POST without PIN")

    def t_instagram(self):
        s, ig, _, _ = request("GET", "/api/instagram", pin=False)
        check(s == 200 and "access_token" not in ig, "GET instagram")
        for k in ("enabled", "account_id", "username", "refresh_minutes", "last_fetch", "last_error", "sample", "token_set",
                  "stories", "posts", "busy", "accounts", "accounts_error"):
            check(k in ig, f"instagram.{k} missing")
        check(len(ig["stories"]) <= 5 and len(ig["posts"]) <= 2, "story/post counts")
        if not self.mock:
            check(ig["sample"] is False, "the device has no samples")
        for bad in ({"refresh_minutes": 4}, {"account_id": "abc"}, {"access_token": "has spaces"}):
            expect(request("PUT", "/api/instagram", bad), 400, "invalid", f"instagram {bad}")
        s, after, _, _ = request("PUT", "/api/instagram", {"refresh_minutes": ig["refresh_minutes"]})
        check(s == 200 and after["token_set"] == ig["token_set"], "instagram no-op save")
        if not ig["token_set"]:
            out = expect(request("POST", "/api/instagram/refresh"), 400, "not_configured", "refresh without a token")
            check(out["error"] == "Add an access token and an Instagram account ID first", out)
            expect(request("POST", "/api/instagram/accounts"), 400, "not_configured", "accounts without a token")
        elif self.args.instagram:
            check(request("POST", "/api/instagram/accounts")[:2] == (202, {"queued": True}), "accounts 202")
            expect(request("POST", "/api/instagram/refresh"), 409, "busy", "second job while one runs")
            end = time.time() + 90
            while time.time() < end and request("GET", "/api/instagram", pin=False)[1]["busy"]:
                time.sleep(1.5)
            final = request("GET", "/api/instagram", pin=False)[1]
            check(final["busy"] is None and (final["accounts"] is not None or final["accounts_error"]), "accounts finished")
        expect(request("GET", "/api/instagram/accounts", pin=False), 404, "not_found", "GET accounts")

    def t_control(self):
        expect(request("POST", "/api/control", {"action": "explode"}), 400, "invalid", "bad action")
        was = request("GET", "/api/status", pin=False)[1]["paused"]
        check(request("POST", "/api/control", {"action": "pause"})[:2] == (200, {"ok": True}), "pause")
        st = request("GET", "/api/status", pin=False)[1]
        check(st["paused"] is True and st["last_action"] == "pause", "paused in status")
        request("POST", "/api/control", {"action": "pause" if was else "resume"})

    def t_firmware_route(self):
        s, body, _, _ = request("POST", "/api/firmware", raw=b"\x00" * 512, headers={"Content-Type": "application/octet-stream"})
        if self.mock:
            check(s == 404, f"mock has no firmware route: {s}")
            return "mock: 404 as expected"
        check(s == 400 and body["code"] == "invalid", f"garbage firmware: {s} {body}")
        s, body, _, _ = request("POST", "/api/firmware", raw=b"\x00" * 512, pin=False,
                                headers={"Content-Type": "application/octet-stream"})
        check(s == 401, f"firmware without PIN: {s}")

    def t_lockout(self):
        if not self.args.lockout:
            return "skipped (--lockout)"
        wrong = "000000" if PIN != "000000" else "111111"
        for i in range(4):
            expect(request("PUT", "/api/settings", {}, pin=wrong), 401, "pin_wrong", f"wrong {i + 1}")
        expect(request("PUT", "/api/settings", {}, pin=wrong), 401, "pin_wrong", "wrong 5 (answered, then locked)")
        s, body, h, _ = request("PUT", "/api/settings", {})
        check(s == 429 and body["code"] == "locked" and body["retry_after_s"] in (29, 30) and h.get("Retry-After") == str(body["retry_after_s"]),
              f"locked: {s} {body} {h.get('Retry-After')}")
        check(request("GET", "/api/status", pin=False)[0] == 200, "reads stay open")
        time.sleep(body["retry_after_s"] + 1)
        check(request("PUT", "/api/settings", {})[0] == 200, "right PIN after the lock")

    # ---- runner -------------------------------------------------------------------------------
    def run(self):
        tests = [m for m in dir(self) if m.startswith("t_")]
        order = ["t_status_fields", "t_no_cors", "t_console_served", "t_pin", "t_settings", "t_bodies", "t_athletes",
                 "t_playlist", "t_media", "t_paths", "t_instagram", "t_control", "t_firmware_route", "t_lockout"]
        assert sorted(order) == sorted(tests)
        failed = passed = 0
        for name in order:
            try:
                note = getattr(self, name)()
                passed += 1
                print(f"PASS {name[2:]}" + (f"  ({note})" if note else ""))
            except Failed as e:
                failed += 1
                print(f"FAIL {name[2:]}: {e}")
                if name == "t_pin":
                    print("Stopping: the remaining checks need a working PIN.")
                    break
            except (urllib.error.URLError, OSError) as e:
                failed += 1
                print(f"FAIL {name[2:]}: can't reach {URL}: {e}")
                break
        print(f"\n{passed} passed, {failed} failed, {len(order) - passed - failed} not run")
        return 1 if failed else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--yes", action="store_true", help="confirm that this test may write to the wall")
    ap.add_argument("--lockout", action="store_true", help="also test the PIN lockout (locks writes for 30 s)")
    ap.add_argument("--instagram", action="store_true", help="also run an Instagram account lookup if a token is saved")
    args = ap.parse_args()
    print(f"Wall: {URL}")
    print("WARNING: this test writes to the wall. It re-saves the athletes and the rotation (restoring\n"
          "them afterwards), uploads and deletes a test GIF, and pauses/resumes the rotation.")
    if not args.yes:
        print("Not run. Add --yes to accept the writes.")
        return 2
    if not PIN:
        print("Set WALL_PIN to the wall's admin PIN.")
        return 2
    return Contract(args).run()


if __name__ == "__main__":
    sys.exit(main())
