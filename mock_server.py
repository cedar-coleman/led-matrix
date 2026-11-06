"""Local stand-in for the wall's web API, so the console works before the hardware exists.

Serves web/ and implements the same REST API (and the same validation) the ESP32
firmware will. API.md is the written contract; this file and tests/test_mock_server.py
are its executable form. Data lives in mock-data/ next to this file and is seeded from
athletes-from-chalkboard.csv and the logo slide on first run.

Run:  python3 -I mock_server.py [--port 8080] [--data DIR]
Open: http://localhost:8080

Instagram: until an access token is saved, the Instagram slides show sample tiles from
mock-assets/ig/. With a token, "Refresh now" fetches the real stories and posts from the
Graph API on a worker thread (needs Pillow to crop the images, as the board does with
its JPEG decoder).

Differences from the device (see API.md, "Device-only behaviour"): the mock accepts the
first admin PIN over plain http from anywhere; the device accepts it only from its setup
access point. The mock's status diagnostics are placeholders.
"""
import argparse
import collections
import csv
import hashlib
import json
import math
import os
import re
import secrets
import shutil
import threading
import time
import urllib.error
import urllib.request
from datetime import date, datetime, timedelta, timezone
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, unquote, urlparse
from zoneinfo import ZoneInfo

ROOT = Path(__file__).resolve().parent
WEB = ROOT / "web"
STORAGE_BYTES = 23 * 1024 * 1024  # LittleFS budget on the board's 32 MB flash
USABLE_SHARE = 0.9  # keep 10% free: LittleFS slows down and wears unevenly when nearly full
MAX_UPLOAD = 8 * 1024 * 1024
MAX_JSON = 256 * 1024
MAX_PEAK_A = 40.0
LIFTS = ("snatch", "clean_jerk", "total", "back_squat", "front_squat")
MEDIA_NAME = re.compile(r"^[a-z0-9][a-z0-9._-]{0,47}\.gif$")
ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,32}$")
PIN_RE = re.compile(r"^\d{6,12}$")
PIN_ITER = 10000
SORTS = ("board", "total", "name")
TIMEZONES = ("America/Los_Angeles", "America/Denver", "America/Phoenix", "America/Chicago",
             "America/New_York", "America/Anchorage", "Pacific/Honolulu", "UTC")
SLIDE_TYPES = ("whiteboard", "media", "instagram_story", "instagram_posts")
GRAPH = "https://graph.facebook.com/v23.0"
STORY_SIZE = (54, 96)
POST_SIZE = (78, 78)
MAX_STORIES = 5
IG_SAMPLES = ROOT / "mock-assets" / "ig"
GEN_RE = re.compile(r"^\d{1,9}$")
TILE_PATH = re.compile(r"^(\d{1,9})/((?:story|post)-\d{1,2}\.bmp)$")
MIN_SECONDS, MAX_SECONDS = 5, 999
FW_VERSION = "0.0.0-mock"
UI_VERSION = "0.2.0"
STARTED = time.time()

# Lockout (API.md, "Admin PIN"): per client, then for everyone.
CLIENT_FREE_FAILS = 5
CLIENT_FIRST_LOCK_S = 30
CLIENT_MAX_LOCK_S = 15 * 60
GLOBAL_FAILS = 20
GLOBAL_WINDOW_S = 10 * 60
GLOBAL_LOCK_S = 15 * 60
MAX_CLIENTS = 64

DEFAULT_SETTINGS = {
    "title": "THE STRENGTH TANK", "brightness": 50, "sort": "board", "pr_highlight_days": 7,
    "timezone": "America/Los_Angeles", "pin_salt": None, "pin_hash": None, "pin_iter": None,
}
DEFAULT_INSTAGRAM = {
    "enabled": True, "account_id": "", "access_token": "", "username": "thestrengthtank",
    "refresh_minutes": 15, "last_fetch": None, "last_error": None, "sample": True, "stories": [], "posts": [],
}


class ApiError(Exception):
    DEFAULT_CODES = {400: "invalid", 401: "pin_required", 403: "forbidden", 404: "not_found",
                     409: "conflict", 413: "too_large", 429: "locked", 507: "no_space"}

    def __init__(self, status, message, code=None, **extra):
        super().__init__(message)
        self.status = status
        self.code = code or self.DEFAULT_CODES.get(status, "error")
        self.extra = extra

    def body(self):
        return {"error": str(self), "code": self.code, **self.extra}


def is_int(v):
    return isinstance(v, int) and not isinstance(v, bool)


def is_kg(v):
    return v is None or (
        isinstance(v, (int, float)) and not isinstance(v, bool) and 0 < v <= 500 and float(v * 2).is_integer()
    )


def iso_utc(dt):
    return dt.astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def hash_pin(pin, salt_hex, iterations):
    return hashlib.pbkdf2_hmac("sha256", pin.encode(), bytes.fromhex(salt_hex), iterations).hex()


class PinGuard:
    """Counts wrong PINs per client IP and overall, and says how long a client must wait.

    A missing X-Admin-Pin header is not a wrong PIN and is not counted.
    """

    def __init__(self, clock):
        self.clock = clock
        self.clients = collections.OrderedDict()  # ip -> {"fails": n, "until": t}
        self.recent = collections.deque()  # times of wrong PINs from anyone, last 10 min
        self.global_until = 0.0
        self.lock = threading.Lock()

    def wait_s(self, ip):
        """Seconds this client must wait before a write is considered, or 0."""
        now = self.clock()
        with self.lock:
            c = self.clients.get(ip)
            left = max(self.global_until - now, (c["until"] - now) if c else 0)
        return math.ceil(left) if left > 0 else 0

    def fail(self, ip):
        now = self.clock()
        with self.lock:
            c = self.clients.pop(ip, None) or {"fails": 0, "until": 0.0}
            c["fails"] += 1
            if c["fails"] >= CLIENT_FREE_FAILS:
                c["until"] = now + min(CLIENT_FIRST_LOCK_S * 2 ** (c["fails"] - CLIENT_FREE_FAILS), CLIENT_MAX_LOCK_S)
            self.clients[ip] = c
            while len(self.clients) > MAX_CLIENTS:
                self.clients.popitem(last=False)  # the global limit still covers an IP flood
            self.recent.append(now)
            while self.recent and self.recent[0] <= now - GLOBAL_WINDOW_S:
                self.recent.popleft()
            if len(self.recent) >= GLOBAL_FAILS:
                self.global_until = now + GLOBAL_LOCK_S
                self.recent.clear()

    def succeed(self, ip):
        with self.lock:
            self.clients.pop(ip, None)


class Store:
    """JSON files plus a media folder, written atomically. One lock serialises writes."""

    def __init__(self, data_dir: Path):
        self.dir = Path(data_dir)
        self.media = self.dir / "media"
        self.lock = threading.RLock()
        self.control = {"paused": False, "last_action": None}
        # Injectable for tests: monotonic seconds (lockout) and wall-clock UTC (dates).
        self.clock = time.monotonic
        self.utcnow = lambda: datetime.now(timezone.utc)
        self.time_valid = True  # the device sets this once NTP or the RTC gives a plausible time
        self.guard = PinGuard(lambda: self.clock())
        self.config_errors = []
        self.errors = collections.deque(maxlen=20)
        self.job_lock = threading.Lock()
        self.job = None  # None, "refresh" or "accounts"
        self.job_thread = None
        self.ig_accounts = None
        self.ig_accounts_error = None
        self.building = set()  # generations a job is still downloading into
        self.media.mkdir(parents=True, exist_ok=True)
        self.clean_leftovers()
        self.seed()
        for name in ("settings", "athletes", "playlist", "instagram", "media_index"):
            self.read(name)  # finds corrupt files at start, not on first use
        self.prune_generations()

    # --- files --------------------------------------------------------------
    def path(self, name):
        return self.dir / f"{name}.json"

    def note_error(self, message):
        self.errors.append({"time": iso_utc(self.utcnow()), "message": message})

    def defaults(self, name):
        if name == "settings":
            return dict(DEFAULT_SETTINGS)
        if name == "athletes":
            return {"rev": 1, "athletes": []}
        if name == "playlist":
            return {"rev": 1, "slides": []}
        if name == "instagram":
            return json.loads(json.dumps(DEFAULT_INSTAGRAM))
        if name == "media_index":
            return self.rebuilt_media_index()
        raise KeyError(name)

    def rebuilt_media_index(self):
        index = {}
        for p in sorted(self.media.glob("*.gif")):
            if MEDIA_NAME.match(p.name):
                uploaded = datetime.fromtimestamp(p.stat().st_mtime, timezone.utc).date().isoformat()
                index[p.name] = {"frames": None, "peak_a": None, "uploaded": uploaded}
        return index

    def read(self, name):
        """Parsed JSON object. A file that won't parse is renamed to .bad and replaced by defaults."""
        p = self.path(name)
        with self.lock:
            try:
                value = json.loads(p.read_text())
                if not isinstance(value, dict):
                    raise ValueError("not a JSON object")
                return value
            except FileNotFoundError:
                value = self.defaults(name)
                self.write(name, value)
                return value
            except (ValueError, UnicodeDecodeError):  # JSONDecodeError is a ValueError
                bad = p.with_name(p.name + ".bad")
                os.replace(p, bad)
                value = self.defaults(name)
                self.write(name, value)
                when = iso_utc(self.utcnow())
                self.config_errors.append({"file": p.name, "renamed_to": bad.name, "time": when})
                self.note_error(f"{p.name} was damaged; saved as {bad.name} and reset to defaults")
                return value

    def write(self, name, value):
        tmp = self.path(name).with_suffix(".tmp")
        tmp.write_text(json.dumps(value, indent=1))
        os.replace(tmp, self.path(name))

    def clean_leftovers(self):
        """Delete what an interrupted write leaves behind: upload .part files and .tmp JSON."""
        for p in list(self.media.glob("*.part")) + list(self.dir.glob("*.tmp")):
            p.unlink(missing_ok=True)

    def seed(self):
        if not self.path("settings").exists():
            self.write("settings", dict(DEFAULT_SETTINGS))
        if not self.path("athletes").exists():
            athletes = []
            csv_path = ROOT / "athletes-from-chalkboard.csv"
            if csv_path.exists():
                with open(csv_path, newline="") as f:
                    for i, row in enumerate(csv.DictReader(f), 1):
                        a = {"id": f"a{i}", "name": row["name"], "notes": row.get("notes", ""),
                             "visible": True, "pr_dates": {}}
                        for k in LIFTS:
                            v = row.get(f"{k}_kg", "").strip()
                            a[k] = (float(v) if "." in v else int(v)) if v else None
                        athletes.append(a)
            self.write("athletes", {"rev": 1, "athletes": athletes})
        if not self.path("instagram").exists():
            self.write("instagram", self.defaults("instagram"))
            self.load_samples()
        if not self.path("media_index").exists():
            index = {}
            logo = ROOT / "slides" / "logo-wordmark-white.gif"
            if logo.exists():
                shutil.copy(logo, self.media / "strength-tank-logo.gif")
                index["strength-tank-logo.gif"] = {"frames": 1, "peak_a": None, "uploaded": date.today().isoformat()}
            self.write("media_index", index)
            slides = [{"id": "s1", "type": "whiteboard", "duration_s": 8, "enabled": True},
                      {"id": "s2", "type": "instagram_story", "duration_s": 6, "enabled": True},
                      {"id": "s3", "type": "instagram_posts", "duration_s": 10, "enabled": True}]
            if index:
                slides.insert(0, {"id": "s0", "type": "media", "file": "strength-tank-logo.gif",
                                  "duration_s": 10, "enabled": True})
            self.write("playlist", {"rev": 1, "slides": slides})

    def read_revisioned(self, name):
        value = self.read(name)
        if not is_int(value.get("rev")):
            value["rev"] = 1
        return value

    # --- settings and time --------------------------------------------------
    def settings(self):
        return DEFAULT_SETTINGS | self.read("settings")

    def settings_public(self, s=None):
        s = s or self.settings()
        return {k: v for k, v in s.items() if not k.startswith("pin_")} | {"pin_set": self.pin_set(s)}

    def local_date(self):
        """Today in the wall's time zone, or None while the clock isn't trustworthy."""
        if not self.time_valid:
            return None
        tz = self.settings().get("timezone")
        return self.utcnow().astimezone(ZoneInfo(tz if tz in TIMEZONES else TIMEZONES[0])).date().isoformat()

    # --- auth ---------------------------------------------------------------
    @staticmethod
    def pin_set(s):
        return bool(s.get("pin_hash") and s.get("pin_salt") and is_int(s.get("pin_iter")))

    def pin_ok(self, pin):
        s = self.settings()
        if not self.pin_set(s) or not isinstance(pin, str) or len(pin) > 64:
            return False
        return secrets.compare_digest(hash_pin(pin, s["pin_salt"], s["pin_iter"]), s["pin_hash"])

    # --- instagram ----------------------------------------------------------
    @property
    def ig_dir(self):
        d = self.dir / "ig"
        d.mkdir(exist_ok=True)
        return d

    def ig_public(self):
        ig = self.read("instagram")
        out = {k: v for k, v in ig.items() if k != "access_token"}
        out["token_set"] = bool(ig.get("access_token"))
        out["busy"] = self.job
        out["accounts"] = self.ig_accounts
        out["accounts_error"] = self.ig_accounts_error
        return out

    @staticmethod
    def generation_of(ig):
        for item in ig.get("stories", []) + ig.get("posts", []):
            m = TILE_PATH.match(item.get("file") or "")
            if m:
                return m.group(1)
        return None

    def new_generation(self):
        with self.lock:
            nums = [int(p.name) for p in self.ig_dir.iterdir() if p.is_dir() and GEN_RE.match(p.name)]
            gen = str(max(nums, default=0) + 1)
            (self.ig_dir / gen).mkdir()
            self.building.add(gen)
            return gen

    def prune_generations(self, keep=None):
        """Delete every generation directory except the one instagram.json points at."""
        keep = keep or self.generation_of(self.read("instagram"))
        for p in self.ig_dir.iterdir():
            if p.name != keep and p.name not in self.building:
                if p.is_dir():
                    shutil.rmtree(p, ignore_errors=True)
                else:
                    p.unlink(missing_ok=True)  # tiles from before generations existed

    def save_tile(self, image_bytes, size, path):
        """Cover-crop to the tile size, as the board does after decoding the JPEG."""
        from io import BytesIO

        from PIL import Image, ImageOps
        with Image.open(BytesIO(image_bytes)) as im:
            tile = ImageOps.fit(im.convert("RGB"), size, Image.LANCZOS)
            tile.save(path, format="BMP")

    def load_samples(self):
        """Sample tiles in a new generation. Caller holds the lock (or is seeding)."""
        now = self.utcnow()
        iso = lambda d: (now - d).strftime("%Y-%m-%dT%H:%M:%S+0000")
        stories, posts = [], []
        gen = self.new_generation()
        try:
            for i, (f, age) in enumerate([("sample-story-1.jpg", timedelta(hours=2)), ("sample-story-2.jpg", timedelta(minutes=35))]):
                self.save_tile((IG_SAMPLES / f).read_bytes(), STORY_SIZE, self.ig_dir / gen / f"story-{i}.bmp")
                stories.append({"id": f"sample-s{i}", "timestamp": iso(age), "file": f"{gen}/story-{i}.bmp", "kind": "image"})
            for i, (f, age, kind, likes) in enumerate([("sample-post-2.jpg", timedelta(days=1), "reel", 214),
                                                       ("sample-post-1.jpg", timedelta(days=4), "image", 87)]):
                self.save_tile((IG_SAMPLES / f).read_bytes(), POST_SIZE, self.ig_dir / gen / f"post-{i}.bmp")
                posts.append({"id": f"sample-p{i}", "timestamp": iso(age), "file": f"{gen}/post-{i}.bmp", "kind": kind,
                              "like_count": likes, "permalink": None})
        except (ImportError, OSError) as e:
            print(f"Instagram samples unavailable ({e}); install Pillow to see them.")
            stories, posts = [], []
        finally:
            self.building.discard(gen)
        ig = self.read("instagram")
        ig.update({"sample": True, "stories": stories, "posts": posts})
        self.write("instagram", ig)
        self.prune_generations(keep=gen)

    def http_get(self, url):
        """The one network call. Tests replace it."""
        with urllib.request.urlopen(url, timeout=20) as r:
            return r.read()

    def graph(self, path, token):
        url = f"{GRAPH}/{path}{'&' if '?' in path else '?'}access_token={urllib.request.quote(token)}"
        try:
            return json.loads(self.http_get(url))
        except urllib.error.HTTPError as e:
            try:
                msg = json.loads(e.read())["error"]["message"]
            except Exception:
                msg = f"HTTP {e.code}"
            raise ApiError(502, f"Instagram said: {msg}")
        except urllib.error.URLError as e:
            raise ApiError(502, f"Couldn't reach Instagram: {e.reason}")

    @staticmethod
    def describe(e):
        """A message for last_error. Never echoes URLs (they carry the token)."""
        if isinstance(e, ApiError):
            return str(e)
        if e.__class__.__name__ in ("UnidentifiedImageError", "DecompressionBombError") or isinstance(e, (SyntaxError, ValueError)):
            return "Couldn't read an image from Instagram"
        if isinstance(e, urllib.error.HTTPError):
            return f"Instagram's image server answered HTTP {e.code}"
        if isinstance(e, urllib.error.URLError):
            return f"Couldn't reach Instagram: {e.reason}"
        if isinstance(e, (TimeoutError, OSError)):
            return "Couldn't download from Instagram (network error)"
        return f"Instagram update failed ({e.__class__.__name__})"

    def fetch_instagram(self):
        """Download stories and posts into a new generation; switch to it only if all of it worked.

        Runs on the worker thread. Never raises: any failure keeps the previous generation
        and is recorded in last_error. last_fetch is set either way.
        """
        ig = self.read("instagram")
        token, account = ig.get("access_token"), ig.get("account_id")
        gen = None
        try:
            gen = self.new_generation()
            gdir = self.ig_dir / gen
            profile = self.graph(f"{account}?fields=username", token)
            fields = "id,media_type,media_product_type,media_url,thumbnail_url,timestamp"
            stories_raw = self.graph(f"{account}/stories?fields={fields}", token).get("data", [])[:MAX_STORIES]
            posts_raw = self.graph(f"{account}/media?fields={fields},like_count,permalink&limit=2", token).get("data", [])[:2]
            stories, posts = [], []
            for m in sorted(stories_raw, key=lambda m: m.get("timestamp", "")):
                url = m.get("thumbnail_url") if m.get("media_type") == "VIDEO" else m.get("media_url")
                if not url:
                    continue
                name = f"story-{len(stories)}.bmp"
                self.save_tile(self.http_get(url), STORY_SIZE, gdir / name)
                stories.append({"id": m["id"], "timestamp": m.get("timestamp"), "file": f"{gen}/{name}",
                                "kind": "video" if m.get("media_type") == "VIDEO" else "image"})
            for m in posts_raw:
                url = (m.get("thumbnail_url") or m.get("media_url")) if m.get("media_type") == "VIDEO" else m.get("media_url")
                if not url:
                    continue
                name = f"post-{len(posts)}.bmp"
                self.save_tile(self.http_get(url), POST_SIZE, gdir / name)
                kind = {"VIDEO": "reel" if m.get("media_product_type") == "REELS" else "video",
                        "CAROUSEL_ALBUM": "carousel"}.get(m.get("media_type"), "image")
                posts.append({"id": m["id"], "timestamp": m.get("timestamp"), "file": f"{gen}/{name}", "kind": kind,
                              "like_count": m.get("like_count"), "permalink": m.get("permalink")})
            with self.lock:
                ig = self.read("instagram")
                if ig.get("access_token") != token or ig.get("account_id") != account:
                    raise ApiError(409, "The Instagram settings changed during the update; refresh again")
                ig.update({"username": profile.get("username", ig["username"]), "stories": stories, "posts": posts,
                           "sample": False, "last_error": None, "last_fetch": iso_utc(self.utcnow())})
                self.write("instagram", ig)  # the switch: from here on the wall uses the new generation
                self.building.discard(gen)
                self.prune_generations(keep=gen)
        except Exception as e:  # noqa: BLE001: any failure must keep the old tiles
            message = self.describe(e)
            with self.lock:
                if gen:
                    self.building.discard(gen)
                    shutil.rmtree(self.ig_dir / gen, ignore_errors=True)
                ig = self.read("instagram")
                ig.update({"last_error": message, "last_fetch": iso_utc(self.utcnow())})
                self.write("instagram", ig)
            self.note_error(f"Instagram: {message}")

    def fetch_accounts(self):
        token = self.read("instagram").get("access_token")
        try:
            try:
                pages = self.graph("me/accounts?fields=name,instagram_business_account{id,username}", token).get("data", [])
            except ApiError:
                pages = [self.graph("me?fields=name,instagram_business_account{id,username}", token)]
            self.ig_accounts = [{"page": pg.get("name"), "id": pg["instagram_business_account"]["id"],
                                 "username": pg["instagram_business_account"].get("username")}
                                for pg in pages if pg.get("instagram_business_account")]
            self.ig_accounts_error = None
        except Exception as e:  # noqa: BLE001
            self.ig_accounts = None
            self.ig_accounts_error = self.describe(e)

    def start_job(self, kind):
        """Run an Instagram job on a worker thread. 409 busy if one is already running."""
        with self.job_lock:
            if self.job:
                raise ApiError(409, "The wall is already talking to Instagram. Try again when it's done.", "busy",
                               busy=self.job)
            self.job = kind
            if kind == "accounts":
                self.ig_accounts, self.ig_accounts_error = None, None
        work = self.fetch_instagram if kind == "refresh" else self.fetch_accounts

        def run():
            try:
                work()
            finally:
                with self.job_lock:
                    self.job = None

        self.job_thread = threading.Thread(target=run, daemon=True, name=f"ig-{kind}")
        self.job_thread.start()

    # --- media --------------------------------------------------------------
    def used_bytes(self):
        """Everything stored, as LittleFS would count it: media, tiles and config."""
        return sum(p.stat().st_size for p in self.dir.rglob("*") if p.is_file())

    def storage(self):
        return {"used_bytes": self.used_bytes(), "total_bytes": STORAGE_BYTES,
                "usable_bytes": int(STORAGE_BYTES * USABLE_SHARE)}

    def media_list(self):
        index = self.read("media_index")
        files = []
        for p in sorted(self.media.glob("*.gif")):
            meta = index.get(p.name, {})
            files.append({"name": p.name, "size": p.stat().st_size, "frames": meta.get("frames"),
                          "peak_a": meta.get("peak_a"), "uploaded": meta.get("uploaded")})
        return {"files": files, "storage": self.storage()}

    # --- status -------------------------------------------------------------
    def now_playing(self):
        slides = [s for s in self.read("playlist").get("slides", []) if s.get("enabled")]
        if not slides:
            return None
        s = slides[0]
        return {"slide_id": s["id"], "type": s["type"], "file": s.get("file")}

    def status(self):
        return {
            "mock": True,
            "fw_version": FW_VERSION,
            "ui_version": UI_VERSION,
            "uptime_s": int(time.time() - STARTED),
            "reset_reason": "poweron",
            "paused": self.control["paused"],
            "last_action": self.control["last_action"],
            "now_playing": self.now_playing(),
            "storage": self.storage(),
            "wifi": {"ssid": "(mock)", "rssi": -58},
            "time_valid": self.time_valid,
            "local_date": self.local_date(),
            "timezone": self.settings()["timezone"],
            "min_free_heap": 61440,
            "refresh_hz": 96,
            "limiter_scale": 1.0,
            "config_errors": list(self.config_errors),
            "recent_errors": list(self.errors),
        }


def validate_athletes(body):
    """Shape and values. pr_dates from the client are ignored: the server stamps them."""
    items = body["athletes"]
    if len(items) > 500:
        raise ApiError(400, "At most 500 athletes")
    seen, out = set(), []
    for i, a in enumerate(items, 1):
        if not isinstance(a, dict):
            raise ApiError(400, f"Athlete {i} is not an object")
        aid, name = a.get("id"), a.get("name")
        if not isinstance(aid, str) or not ID_RE.match(aid) or aid in seen:
            raise ApiError(400, f"Athlete {i}: missing or duplicate id")
        seen.add(aid)
        if not isinstance(name, str) or not name.strip() or len(name.strip()) > 40:
            raise ApiError(400, f"Athlete {i}: name must be 1 to 40 characters")
        clean = {"id": aid, "name": name.strip(), "visible": a.get("visible", True) is not False}
        for k in LIFTS:
            v = a.get(k)
            if not is_kg(v):
                raise ApiError(400, f"{name}: {k} must be blank or 0.5 to 500 kg in 0.5 kg steps")
            clean[k] = v
        notes = a.get("notes", "")
        if not isinstance(notes, str) or len(notes) > 200:
            raise ApiError(400, f"{name}: notes must be text up to 200 characters")
        clean["notes"] = notes
        out.append(clean)
    return out


def stamp_prs(athletes, before, today):
    """Server-side PR dates. Existing athlete: a lift that went up, or was filled in for the
    first time, gets today's date (in the wall's zone); other stored dates are kept. New
    athletes start with none. today None (clock not set) stamps nothing."""
    old = {a["id"]: a for a in before}
    for a in athletes:
        o = old.get(a["id"])
        if o is None:
            a["pr_dates"] = {}
            continue
        dates = {k: v for k, v in (o.get("pr_dates") or {}).items() if k in LIFTS}
        if today:
            for lift in LIFTS:
                v, ov = a[lift], o.get(lift)
                if v is not None and (ov is None or v > ov):
                    dates[lift] = today
        a["pr_dates"] = dates
    return athletes


def validate_playlist(body, media_names):
    if len(body["slides"]) > 100:
        raise ApiError(400, "At most 100 slides")
    seen, out = set(), []
    for i, s in enumerate(body["slides"], 1):
        if not isinstance(s, dict) or not isinstance(s.get("id"), str) or not ID_RE.match(s["id"]) or s["id"] in seen:
            raise ApiError(400, f"Slide {i}: missing or duplicate id")
        seen.add(s["id"])
        d = s.get("duration_s")
        if not is_int(d) or not MIN_SECONDS <= d <= MAX_SECONDS:
            raise ApiError(400, f"Slide {i}: duration must be a whole number of seconds from {MIN_SECONDS} to {MAX_SECONDS}")
        clean = {"id": s["id"], "type": s.get("type"), "duration_s": d, "enabled": s.get("enabled", True) is not False}
        if clean["type"] == "media":
            if s.get("file") not in media_names:
                raise ApiError(400, f"Slide {i}: media file {s.get('file')!r} is not on the board")
            clean["file"] = s["file"]
        elif clean["type"] not in SLIDE_TYPES:
            raise ApiError(400, f"Slide {i}: type must be one of {', '.join(SLIDE_TYPES)}")
        out.append(clean)
    return out


def check_rev(body, current):
    if not is_int(body.get("rev")) or body["rev"] != current:
        raise ApiError(409, "Someone else saved changes. Reload to see them.", "stale", rev=current)


def gif_size(data: bytes):
    if len(data) < 13 or data[:6] not in (b"GIF87a", b"GIF89a"):
        return None
    return int.from_bytes(data[6:8], "little"), int.from_bytes(data[8:10], "little")


class Handler(SimpleHTTPRequestHandler):
    store: Store = None  # set by make_server

    def __init__(self, *args, **kwargs):
        self._body = None
        super().__init__(*args, directory=str(WEB), **kwargs)

    def end_headers(self):
        # No Access-Control-* headers, ever: other origins must not be able to call the API.
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, fmt, *args):
        if os.environ.get("MOCK_QUIET") != "1":
            super().log_message(fmt, *args)

    # --- plumbing -----------------------------------------------------------
    def send_json(self, status, value, headers=None):
        body = json.dumps(value).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def body_bytes(self, limit):
        if self._body is None:
            try:
                n = int(self.headers.get("Content-Length") or 0)
            except ValueError:
                raise ApiError(400, "Bad Content-Length")
            if n > limit:
                raise ApiError(413, f"Too large (limit {limit // 1024} KB)")
            self._body = self.rfile.read(n) if n > 0 else b""
        return self._body

    def json_body(self):
        try:
            return json.loads(self.body_bytes(MAX_JSON) or b"null")
        except (json.JSONDecodeError, UnicodeDecodeError):
            raise ApiError(400, "Body is not valid JSON")

    def object_body(self, key=None):
        body = self.json_body()
        if not isinstance(body, dict) or (key and not isinstance(body.get(key), list)):
            raise ApiError(400, f'Expected {{"rev": n, "{key}": [...]}}' if key else "Expected an object")
        return body

    def authorize(self, method, path):
        """Every write: lockout first, then 'no PIN yet', then the PIN itself."""
        st = self.store
        ip = self.client_address[0]
        wait = st.guard.wait_s(ip)
        if wait:
            raise ApiError(429, f"Too many wrong PINs. Try again in {wait} s.", "locked", retry_after_s=wait)
        if not st.pin_set(st.settings()):
            if method == "PUT" and path == "/api/settings":
                body = self.json_body()
                if isinstance(body, dict) and set(body) == {"new_pin"}:
                    return
            raise ApiError(403, "Set an admin PIN first", "pin_not_set")
        pin = self.headers.get("X-Admin-Pin")
        if not pin:
            raise ApiError(401, "Admin PIN required", "pin_required")
        if not st.pin_ok(pin):
            st.guard.fail(ip)
            raise ApiError(401, "That PIN is not right", "pin_wrong")
        st.guard.succeed(ip)

    def dispatch(self, method):
        url = urlparse(self.path)
        try:
            if method != "GET":
                self.authorize(method, url.path)
            result = self.route(method, url.path, parse_qs(url.query, keep_blank_values=True))
            status, value = result if isinstance(result, tuple) else (200, result)
            self.send_json(status, value)
        except ApiError as e:
            headers = {"Retry-After": str(e.extra["retry_after_s"])} if e.status == 429 else None
            self.send_json(e.status, e.body(), headers)

    def do_GET(self):
        path = urlparse(self.path).path
        if path.startswith("/api/"):
            return self.dispatch("GET")
        if path.startswith("/media/"):
            return self.serve_media(unquote(path[len("/media/"):]))
        if path.startswith("/ig/"):
            return self.serve_tile(unquote(path[len("/ig/"):]))
        return super().do_GET()

    def do_POST(self):
        self.dispatch("POST")

    def do_PUT(self):
        self.dispatch("PUT")

    def do_DELETE(self):
        self.dispatch("DELETE")

    def do_OPTIONS(self):
        # A CORS preflight gets a plain refusal, so browsers block cross-origin writes.
        self.send_json(405, {"error": "Method not allowed", "code": "method_not_allowed"})

    def serve_file(self, p, ctype):
        data = p.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def serve_media(self, name):
        p = self.store.media / name
        if not MEDIA_NAME.match(name) or not p.is_file():
            return self.send_json(404, {"error": "No such file", "code": "not_found"})
        self.serve_file(p, "image/gif")

    def serve_tile(self, rel):
        m = TILE_PATH.match(rel)
        if not m:
            return self.send_json(404, {"error": "No such tile", "code": "not_found"})
        base = self.store.ig_dir.resolve()
        p = (base / m.group(1) / m.group(2)).resolve()
        if p.parent.parent != base or not p.is_file():
            return self.send_json(404, {"error": "No such tile", "code": "not_found"})
        self.serve_file(p, "image/bmp")

    # --- API ----------------------------------------------------------------
    def route(self, method, path, query):
        st = self.store
        if path == "/api/status" and method == "GET":
            return st.status()
        if path == "/api/athletes":
            if method == "GET":
                return st.read_revisioned("athletes")
            if method == "PUT":
                return self.put_athletes(self.object_body("athletes"))
        if path == "/api/playlist":
            if method == "GET":
                return st.read_revisioned("playlist")
            if method == "PUT":
                return self.put_playlist(self.object_body("slides"))
        if path == "/api/settings":
            if method == "GET":
                return st.settings_public()
            if method == "PUT":
                return self.put_settings(self.object_body())
        if path == "/api/instagram":
            if method == "GET":
                return st.ig_public()
            if method == "PUT":
                return self.put_instagram(self.object_body())
        if path == "/api/instagram/refresh" and method == "POST":
            ig = st.read("instagram")
            if not ig.get("access_token") or not ig.get("account_id"):
                raise ApiError(400, "Add an access token and an Instagram account ID first", "not_configured")
            st.start_job("refresh")
            return 202, {"queued": True}
        if path == "/api/instagram/accounts" and method == "POST":
            if not st.read("instagram").get("access_token"):
                raise ApiError(400, "Save an access token first", "not_configured")
            st.start_job("accounts")
            return 202, {"queued": True}
        if path == "/api/control" and method == "POST":
            body = self.json_body()
            action = body.get("action") if isinstance(body, dict) else None
            if action not in ("next", "prev", "pause", "resume"):
                raise ApiError(400, "action must be next, prev, pause or resume")
            st.control["last_action"] = action
            if action in ("pause", "resume"):
                st.control["paused"] = action == "pause"
            return {"ok": True}
        if path == "/api/media":
            if method == "GET":
                return st.media_list()
            if method == "POST":
                return self.upload(query)
        if path.startswith("/api/media/") and method == "DELETE":
            return self.delete_media(unquote(path[len("/api/media/"):]))
        raise ApiError(404, f"No route for {method} {path}")

    def put_athletes(self, body):
        st = self.store
        with st.lock:
            cur = st.read_revisioned("athletes")
            check_rev(body, cur["rev"])
            athletes = stamp_prs(validate_athletes(body), cur["athletes"], st.local_date())
            out = {"rev": cur["rev"] + 1, "athletes": athletes}
            st.write("athletes", out)
        return out

    def put_playlist(self, body):
        st = self.store
        with st.lock:
            cur = st.read_revisioned("playlist")
            check_rev(body, cur["rev"])
            names = {p.name for p in st.media.glob("*.gif")}
            out = {"rev": cur["rev"] + 1, "slides": validate_playlist(body, names)}
            st.write("playlist", out)
        return out

    def put_instagram(self, body):
        st = self.store
        with st.lock:
            ig = st.read("instagram")
            if "enabled" in body:
                ig["enabled"] = body["enabled"] is True
            if "account_id" in body:
                if not isinstance(body["account_id"], str) or not re.fullmatch(r"\d{0,32}", body["account_id"]):
                    raise ApiError(400, "The Instagram account ID is a number, up to 32 digits")
                ig["account_id"] = body["account_id"]
            if "refresh_minutes" in body:
                m = body["refresh_minutes"]
                if not is_int(m) or not 5 <= m <= 1440:
                    raise ApiError(400, "Refresh every 5 to 1440 minutes")
                ig["refresh_minutes"] = m
            if "access_token" in body:
                tok = body["access_token"]
                if not isinstance(tok, str) or len(tok) > 1024 or not re.fullmatch(r"[A-Za-z0-9_\-|.]*", tok):
                    raise ApiError(400, "That doesn't look like an access token")
                ig["access_token"] = tok
                st.ig_accounts, st.ig_accounts_error = None, None
            st.write("instagram", ig)
            if "access_token" in body and not body["access_token"]:
                ig.update({"last_error": None, "last_fetch": None})
                st.write("instagram", ig)
                st.load_samples()
        return st.ig_public()

    def put_settings(self, body):
        st = self.store
        with st.lock:
            s = st.settings()
            if "title" in body:
                if not isinstance(body["title"], str) or len(body["title"]) > 32:
                    raise ApiError(400, "Title must be text up to 32 characters")
                s["title"] = body["title"]
            if "brightness" in body:
                if not is_int(body["brightness"]) or not 5 <= body["brightness"] <= 100:
                    raise ApiError(400, "Brightness must be 5 to 100")
                s["brightness"] = body["brightness"]
            if "sort" in body:
                if body["sort"] not in SORTS:
                    raise ApiError(400, f"Sort must be one of {', '.join(SORTS)}")
                s["sort"] = body["sort"]
            if "pr_highlight_days" in body:
                if not is_int(body["pr_highlight_days"]) or not 0 <= body["pr_highlight_days"] <= 60:
                    raise ApiError(400, "PR highlight must be 0 to 60 days")
                s["pr_highlight_days"] = body["pr_highlight_days"]
            if "timezone" in body:
                if body["timezone"] not in TIMEZONES:
                    raise ApiError(400, f"Time zone must be one of {', '.join(TIMEZONES)}")
                s["timezone"] = body["timezone"]
            if "new_pin" in body:
                pin = body["new_pin"]
                if not isinstance(pin, str) or not PIN_RE.match(pin):
                    raise ApiError(400, "The PIN must be 6 to 12 digits")
                s["pin_salt"] = secrets.token_bytes(16).hex()
                s["pin_iter"] = PIN_ITER
                s["pin_hash"] = hash_pin(pin, s["pin_salt"], PIN_ITER)
            st.write("settings", s)
        return st.settings_public(s)

    def upload(self, query):
        st = self.store
        name = (query.get("name") or [""])[0]
        if not MEDIA_NAME.match(name):
            raise ApiError(400, "File name must be lowercase letters, digits, . _ - and end in .gif")
        peak = (query.get("peak_a") or [None])[0]
        if peak is not None:
            try:
                peak = float(peak)
            except ValueError:
                peak = math.nan
            if not math.isfinite(peak) or not 0 <= peak <= MAX_PEAK_A:
                raise ApiError(400, f"peak_a must be a number from 0 to {MAX_PEAK_A:g}")
            peak = round(peak, 2)
        frames = (query.get("frames") or [""])[0]
        data = self.body_bytes(MAX_UPLOAD)
        size = gif_size(data)
        if size is None:
            raise ApiError(400, "Not a GIF file")
        if size != (256, 96):
            raise ApiError(400, f"GIF must be 256x96, got {size[0]}x{size[1]}")
        with st.lock:
            target = st.media / name
            if target.exists():
                raise ApiError(409, f"{name} already exists", "exists")
            storage = st.storage()
            if storage["used_bytes"] + len(data) > storage["usable_bytes"]:
                raise ApiError(507, "Not enough space on the board. Delete something first.", "no_space")
            tmp = target.with_suffix(".part")
            tmp.write_bytes(data)
            os.replace(tmp, target)
            index = st.read("media_index")
            index[name] = {"frames": int(frames) if frames.isdigit() and len(frames) < 7 else None,
                           "peak_a": peak, "uploaded": st.local_date()}
            st.write("media_index", index)
        return {"name": name, "size": len(data), "peak_a": peak}

    def delete_media(self, name):
        st = self.store
        if not MEDIA_NAME.match(name) or not (st.media / name).exists():
            raise ApiError(404, "No such file")
        with st.lock:
            used = [s for s in st.read("playlist")["slides"] if s.get("file") == name]
            if used:
                raise ApiError(409, f"{name} is in the rotation. Remove it there first.", "in_use")
            (st.media / name).unlink()
            index = st.read("media_index")
            index.pop(name, None)
            st.write("media_index", index)
        return {"ok": True}


def make_server(port, data_dir):
    store = Store(Path(data_dir))
    handler = type("BoundHandler", (Handler,), {"store": store})
    server = ThreadingHTTPServer(("127.0.0.1", port), handler)
    server.store = store
    return server


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--data", default=str(ROOT / "mock-data"))
    args = ap.parse_args()
    server = make_server(args.port, args.data)
    print(f"Wall console: http://localhost:{args.port}  (data in {args.data})")
    server.serve_forever()


if __name__ == "__main__":
    main()
