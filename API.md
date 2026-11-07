# Gym LED Wall: HTTP API contract

This is the contract between the web console (`web/`) and the wall. The ESP32-S3 firmware
implements it; `mock_server.py` is the reference implementation, and
`tests/test_mock_server.py` is the test suite the firmware must also pass. Where this file
and the mock disagree, that is a bug: report it rather than picking one.

Section 12 lists the places where the device behaves differently from the mock on purpose.

---

## 1. Transport

- Plain HTTP on port 80, at `http://gymboard.local` (mDNS) or the board's IP address. The
  mock listens on `http://localhost:8080`.
- Every API body is JSON (UTF-8), except media uploads (raw GIF bytes) and the files served
  under `/media/` and `/ig/`.
- Every response carries `Cache-Control: no-store`.
- **No CORS headers, ever.** No `Access-Control-*` header is sent on any response, and
  `OPTIONS` on any path answers `405` (see section 3). Browsers therefore refuse to let a page
  from another origin send the `X-Admin-Pin` header or a `PUT`/`DELETE`.
- The console and the API ship in the same app image (the console is embedded, gzipped), so a
  console always talks to the API version it was built with.

## 2. Errors

Every error is a JSON object:

```json
{"error": "Text written for the coach to read", "code": "machine_readable_code"}
```

Some codes add fields, listed below. The console shows `error` as is, so keep it plain and
short, and never put a URL, token or PIN in it.

| Status | `code` | When | Extra fields |
| --- | --- | --- | --- |
| 400 | `invalid` | Body not JSON, wrong shape, or a value out of range | `day`: for a schedule error about one day, that day's key (`"mon"` to `"sun"`, section 7.4) |
| 400 | `not_configured` | Instagram job asked for before a token (and, for refresh, an account ID) is saved | |
| 401 | `pin_required` | A write without an `X-Admin-Pin` header | |
| 401 | `pin_wrong` | A write with a wrong PIN | |
| 403 | `pin_not_set` | A write while no PIN is set (section 4.2) | |
| 404 | `not_found` | Unknown `/api/` route, media file, or tile | |
| 405 | `method_not_allowed` | `OPTIONS` on any path | |
| 409 | `stale` | A whole-list save sent an old `rev` (section 5) | `rev`: the current revision |
| 409 | `busy` | An Instagram job is already running | `busy`: `"refresh"` or `"accounts"` |
| 409 | `exists` | Upload of a media name that is already on the board | |
| 409 | `in_use` | Delete of a media file the playlist uses | |
| 413 | `too_large` | Body over the limit for that route (section 11) | |
| 429 | `locked` | Writes are locked after wrong PINs (section 4.4) | `retry_after_s`: whole seconds, also sent as the `Retry-After` header |
| 507 | `no_space` | Upload would push storage past the usable space | |

Success codes: `200` for everything except the two Instagram jobs, which answer `202`.

## 3. Routes at a glance

| Method | Path | Auth | Success |
| --- | --- | --- | --- |
| GET | `/api/status` | open | 200 status object (section 10) |
| GET | `/api/athletes` | open | 200 `{"rev", "athletes"}` |
| PUT | `/api/athletes` | PIN | 200 `{"rev", "athletes"}` |
| GET | `/api/playlist` | open | 200 `{"rev", "slides"}` |
| PUT | `/api/playlist` | PIN | 200 `{"rev", "slides"}` |
| GET | `/api/announcements` | open | 200 `{"rev", "announcements"}` |
| PUT | `/api/announcements` | PIN | 200 `{"rev", "announcements"}` |
| GET | `/api/settings` | open | 200 public settings |
| PUT | `/api/settings` | PIN (first PIN: see 4.2) | 200 public settings |
| GET | `/api/instagram` | open | 200 public Instagram state |
| PUT | `/api/instagram` | PIN | 200 public Instagram state |
| POST | `/api/instagram/refresh` | PIN | 202 `{"queued": true}` |
| POST | `/api/instagram/accounts` | PIN | 202 `{"queued": true}` |
| POST | `/api/control` | PIN | 200 `{"ok": true}` |
| GET | `/api/media` | open | 200 `{"files", "storage"}` |
| POST | `/api/media?name=&frames=&peak_a=` | PIN | 200 `{"name", "size", "peak_a"}` |
| DELETE | `/api/media/{name}` | PIN | 200 `{"ok": true}` |
| GET | `/media/{name}` | open | 200 `image/gif` |
| GET | `/ig/{gen}/{tile}.bmp` | open | 200 `image/bmp` |
| GET | `/`, `/app.js`, ... | open | The console's static files |
| OPTIONS | any | | 405 `method_not_allowed` |
| POST | `/api/firmware` | PIN | 200 `{"ok", "version", "rebooting"}`. **Device only** (section 15) |
| PUT | `/api/wifi` | PIN, setup access point only | 200 `{"ok", "restarting"}`. **Device only** (section 12) |
| GET | `/setup` | open | The setup page. **Device only** (section 12) |

"Auth: PIN" means every `POST`, `PUT` and `DELETE`. All `GET`s are open. An unknown
`/api/` path or method answers `404 not_found`, but only after the write checks in section 4
(so a write to an unknown path with a wrong PIN still counts as a wrong PIN).

Paths are matched after percent-decoding. Query strings are parsed with blank values kept
(`peak_a=` is a present, invalid value, not an absent one).

## 4. Admin PIN and lockout

### 4.1 Format and storage

- A PIN is a string of **6 to 12 ASCII digits** (`^[0-9]{6,12}$`).
- Stored in `settings.json` as:
  - `pin_salt`: 16 random bytes, lowercase hex (32 characters), new on every PIN change
  - `pin_iter`: `10000`
  - `pin_hash`: PBKDF2-HMAC-SHA256(password = PIN as ASCII, salt = the 16 salt bytes,
    iterations = `pin_iter`, length = 32 bytes), lowercase hex (64 characters)
- A PIN is "set" when all three are present (`pin_iter` an integer). Otherwise no PIN is set.
- Compare hashes in constant time. A header value longer than 64 bytes is a wrong PIN
  without hashing.
- No `GET` ever returns `pin_salt`, `pin_hash` or `pin_iter`. Settings expose only
  `pin_set: true|false`.

### 4.2 Order of checks for every write

For every `POST`, `PUT` and `DELETE`, before the route or body is looked at:

1. **Lockout.** If this client, or everyone, is locked (4.4): `429 locked`. The PIN is not
   checked and the attempt is not counted.
2. **No PIN set.** If no PIN is set: `403 pin_not_set`
   (`{"error": "Set an admin PIN first", "code": "pin_not_set"}`), with one exception:
   `PUT /api/settings` whose body is a JSON object with **exactly one key, `new_pin`**. That
   request goes through to section 7.3 (where the PIN format is checked). Any other body,
   including `new_pin` plus other keys, gets the 403. On the device the exception applies only
   to requests from the setup access point (section 12).
3. **Header missing.** No `X-Admin-Pin` header (or an empty one): `401 pin_required`. Not
   counted as a wrong PIN: it carries no guess.
4. **Wrong PIN:** `401 pin_wrong`, and the failure is counted (4.4).
5. **Right PIN:** this client's failure count is reset, and the request proceeds.

Changing the PIN is an ordinary write: `PUT /api/settings {"new_pin": "..."}` with the
current PIN in `X-Admin-Pin`.

### 4.3 Responses

```json
{"error": "Admin PIN required", "code": "pin_required"}
{"error": "That PIN is not right", "code": "pin_wrong"}
{"error": "Too many wrong PINs. Try again in 30 s.", "code": "locked", "retry_after_s": 30}
```

The 429 also carries `Retry-After: 30` (the same whole number of seconds).

### 4.4 Lockout rules

Clients are told apart by IP address.

Per client:

- Each wrong PIN adds 1 to the client's count `n`.
- When `n >= 5`, the client is locked until `now + min(30 * 2^(n - 5), 900)` seconds. So the
  5th wrong PIN locks for 30 s, the 6th for 60 s, then 120, 240, 480, and 900 s (15 min) from
  the 10th on.
- The wrong PIN that sets the lock is itself answered `401 pin_wrong`; requests during the lock
  get `429`.
- After a lock runs out, the count is kept, so the next wrong PIN locks again straight away
  for the next, longer time.
- A right PIN resets the client (count 0, no lock).
- The table of clients is bounded: when full, the client with the oldest entry is dropped
  (the mock keeps 64; the device may keep fewer, but at least 16). The global rule below
  still covers an attacker who cycles addresses.

For everyone:

- Every wrong PIN, from any client, is recorded with its time.
- When 20 wrong PINs have been recorded within the last 10 minutes (600 s), **all writes from
  every client** are locked for 15 minutes (900 s), and the record is cleared.

`retry_after_s` is the larger of the client's and the global remaining time, rounded up to a
whole second. Reads are never locked. Lock state lives in RAM: a restart clears it.

Time for the lockout comes from a monotonic clock, never the wall clock. (The mock's
`Store.clock` is injectable for tests.)

## 5. Revisions on whole-list saves

`athletes.json`, `playlist.json` and `announcements.json` each carry an integer `rev`.

- `GET` returns the current `rev` with the list.
- `PUT` must send that `rev` back. If `rev` is missing, not an integer, or not equal to the
  current one: `409 stale`, nothing is written:

  ```json
  {"error": "Someone else saved changes. Reload to see them.", "code": "stale", "rev": 7}
  ```

- On success the server writes `rev + 1` and returns it with the saved list.
- New files start at `rev: 1`. A file without a valid `rev` is read as `rev: 1`.
- Order of checks: shape (`400 invalid`), then `rev` (`409 stale`), then the contents
  (`400 invalid`). The `rev` check and the write happen under one lock, so two saves with the
  same `rev` can't both succeed.

## 6. Athletes

### `GET /api/athletes`

```json
{"rev": 4, "athletes": [
  {"id": "a1", "name": "rytownusa", "visible": true,
   "snatch": 90, "clean_jerk": 112, "total": 200, "back_squat": 172, "front_squat": 137.5,
   "notes": "", "pr_dates": {"snatch": "2026-11-06"}, "pr_prev": {"snatch": 87.5}}
]}
```

### `PUT /api/athletes`

Request: `{"rev": <int>, "athletes": [<athlete>, ...]}`, at most 500 athletes, body at most
256 KB. Per athlete:

| Field | Rule |
| --- | --- |
| `id` | String matching `^[A-Za-z0-9_-]{1,32}$`, unique in the list |
| `name` | String; after trimming spaces, 1 to 40 characters. Stored trimmed |
| `snatch`, `clean_jerk`, `total`, `back_squat`, `front_squat` | `null`/absent, or a number (not a boolean) with `0 < v <= 500` and `v * 2` a whole number (0.5 kg steps) |
| `notes` | String up to 200 characters; absent means `""` |
| `visible` | Anything other than `false` is stored as `true` |
| `pr_dates`, `pr_prev` | **Ignored.** The server sets them (below) |

Unknown fields are dropped. The response is the saved document, in the order sent.

**PR dates are stamped by the server**, in the wall's time zone (section 9), and each stamp
records the value it replaced:

- `pr_dates` maps a lift name to the `YYYY-MM-DD` of its last PR. `pr_prev` maps a lift name
  to the value that PR replaced: a number, or `null` when the lift was blank before. A lift
  missing from `pr_prev` has no recorded previous value (it was stamped before `pr_prev`
  existed, or never). Both maps hold only the five lift names.
- An athlete whose `id` is not in the stored list is new: `pr_dates` and `pr_prev` are `{}`.
- For an athlete whose `id` is already stored, start from the stored `pr_dates` and
  `pr_prev`. For each lift, if the new value is not `null` and either the stored value is
  `null` or the new value is greater (numeric comparison), set that lift's date to today's
  local date and its `pr_prev` to the stored value (`null` if it was blank).
- Every other stored date and previous value is kept as it was, including for a lift that went
  down or was cleared. A second PR on the same day records the value that one replaced.
- If the clock is not valid (`time_valid` false, section 10), nothing is stamped; stored dates
  and previous values are still kept.

## 6a. Announcements

Short messages from the coaches. Each one the wall shows gets a screen of its own (white
text on the brand teal) from the `announcements` slide type (section 7.2).

### `GET /api/announcements`

```json
{"rev": 3, "announcements": [
  {"id": "n1", "text": "Welcome to The Strength Tank", "enabled": true, "start_date": null, "end_date": null},
  {"id": "n2", "text": "Closed Thursday for Thanksgiving\nOpen gym Friday 8 to 12", "enabled": true,
   "start_date": "2026-11-20", "end_date": "2026-11-27"}
]}
```

### `PUT /api/announcements`

Request: `{"rev": <int>, "announcements": [<announcement>, ...]}`, at most 50. The whole list
is replaced, with the same `rev` rule as athletes (section 5) and the same PIN rules
(section 4). Per announcement:

| Field | Rule |
| --- | --- |
| `id` | String matching `^[A-Za-z0-9_-]{1,32}$`, unique in the list |
| `text` | String; after trimming whitespace from both ends (Python's `str.strip()`), 1 to 200 characters (code points). `\n` is a line break on the wall, at most 6 of them. Stored trimmed. Any character is stored as sent, but the wall's font draws only printable ASCII (and the heart, U+2665); everything else shows as `?` |
| `enabled` | Anything other than `false` is stored as `true` |
| `start_date`, `end_date` | `null`, absent or `""` (stored as `null`), or a real calendar date written `YYYY-MM-DD` (no time, no trailing newline). `end_date` is the last day shown and must not be before `start_date` |

Unknown fields are dropped. The response is the saved document, in the order sent, each
announcement with exactly the five fields above.

Errors, all `400 invalid`, in this order (the first one wins). `<n>` is the announcement's
place in the list, from 1:

| `error` | When |
| --- | --- |
| `Expected {"rev": n, "announcements": [...]}` | The body is not an object with an `announcements` list (before the `rev` check) |
| `At most 50 announcements` | More than 50 |
| `Announcement <n> is not an object` | |
| `Announcement <n>: missing or duplicate id` | `id` missing, malformed or used twice |
| `Announcement <n>: text must be 1 to 200 characters` | `text` missing, not a string, blank, or too long after trimming |
| `Announcement <n>: at most 6 line breaks` | More than six `\n` after trimming |
| `Announcement <n>: the start date must be YYYY-MM-DD or blank` | `start_date` set but not a real date in that form |
| `Announcement <n>: the end date must be YYYY-MM-DD or blank` | The same for `end_date` |
| `Announcement <n>: the end date is before the start date` | Both set and `end_date` < `start_date` |

`tests/announcement_cases.json` lists cases and the answer for each; the mock's tests and
the firmware's netcore tests both run them.

**Which announcements show.** An announcement is showing today when it is enabled and,
for each date it has, today (the wall's local date, section 9) is on or after `start_date`
and on or before `end_date`. While the wall's clock is not valid, undated announcements
still show and dated ones don't. `announcementScreens()` in `web/announceslides.js` is the
rule.

## 7. Playlist and settings

### 7.1 `GET /api/playlist`

```json
{"rev": 2, "slides": [
  {"id": "s0", "type": "media", "file": "strength-tank-logo.gif", "duration_s": 10, "enabled": true},
  {"id": "s1", "type": "whiteboard", "duration_s": 8, "enabled": true}
]}
```

### 7.2 `PUT /api/playlist`

Request: `{"rev": <int>, "slides": [...]}`, at most 100 slides.

| Field | Rule |
| --- | --- |
| `id` | `^[A-Za-z0-9_-]{1,32}$`, unique |
| `type` | `whiteboard`, `media`, `instagram_story`, `instagram_posts`, `new_prs` or `announcements` |
| `duration_s` | Integer (not boolean), 5 to 999. Per page for the whiteboard, per story for `instagram_story`, per athlete for `new_prs`, per announcement for `announcements` |
| `enabled` | Anything other than `false` is `true` |
| `file` | `media` only: the name of a GIF on the board (else `400`). Dropped for other types |

An unknown type is `400 invalid` with `Slide <n>: type must be one of whiteboard, media,
instagram_story, instagram_posts, new_prs, announcements`.

**What each type shows.** `whiteboard`: one screen per page. `instagram_story`: one per
current story, `instagram_posts`: one while there are posts (both skipped while Instagram is
off). `media`: the GIF. `new_prs`: one screen per athlete with a PR in the last
`pr_highlight_days` days (section 7.3), as `recentPrs()` in `web/prslides.js` picks them:

- An athlete counts when they are visible, their name isn't blank, and at least one lift has
  a value and a `pr_dates` date `d` with `0 <= today - d < pr_highlight_days` days, where
  `today` is the wall's local date (section 9). The whiteboard's green highlight uses the same
  rule.
- Athletes are ordered by their newest such date, newest first; athletes with the same date
  keep their order in `athletes.json`. Only the first 20 get a screen in a pass.
- Each screen shows up to three of the athlete's recent lifts, newest first, with the
  increase over `pr_prev` when it is known.
- The slide is skipped when no athlete counts, when `pr_highlight_days` is 0, and while the
  wall's clock is not valid. The list is worked out again when the athletes or settings
  change and when the wall's date changes.

`announcements`: one screen per announcement showing today (section 6a), in list order.
Only the first 20 get a screen in a pass. The slide is skipped when none is showing. The
list is worked out again when the announcements change and when the wall's date changes
(an announcement whose `end_date` was yesterday drops off at midnight). Each screen is
`renderAnnouncement()` in `web/announceslides.js`: the text word-wrapped at spaces and at
each `\n` (a word longer than a line is split), centred, at the largest of 3x, 2x or 1x
font size that fits inside a 4 px margin (3x and 2x only without splitting a word), with
a row of dots at the bottom when more than one is showing. Text that doesn't fit even at
1x (nine lines) is cut and ends in `~`.
### 7.3 Settings

`GET /api/settings`:

```json
{"title": "THE STRENGTH TANK", "brightness": 50, "sort": "board", "pr_highlight_days": 7,
 "timezone": "America/Los_Angeles",
 "schedule": {"enabled": false, "days": {"mon": [{"start": "05:00", "end": "21:00"}], "tue": [...], "wed": [...],
              "thu": [...], "fri": [...], "sat": [...], "sun": [...]}},
 "power": {"supplies": 2, "rated_a": 18, "budget_pct": 85},
 "pin_set": true}
```

`PUT /api/settings` takes any subset of these keys; unknown keys are ignored. The response
is the same object as the `GET`, after the change. Checks stop at the first bad value
(nothing is written).

| Key | Rule | Default |
| --- | --- | --- |
| `title` | String, up to 32 characters | `"THE STRENGTH TANK"` |
| `brightness` | Integer 5 to 100 (percent) | `50` |
| `sort` | `board`, `total` or `name` | `"board"` |
| `pr_highlight_days` | Integer 0 to 60 (0 = off) | `7` |
| `timezone` | One of the eight zones in section 9 | `"America/Los_Angeles"` |
| `schedule` | When the display is on, the whole object (section 7.4) | enabled `false`, every day 05:00 to 21:00 |
| `power` | The supplies the power limiter protects, the whole object (section 7.5) | `{"supplies": 2, "rated_a": 18, "budget_pct": 85}` |
| `new_pin` | String of 6 to 12 digits; sets a new salt and hash. Write-only | |

The keys are checked in the order of this table.

### 7.4 Display schedule

The gym sets, for each day of the week, when the wall is lit:

```json
{"enabled": true,
 "days": {"mon": [{"start": "05:00", "end": "21:00"}],
          "sat": [{"start": "08:00", "end": "12:00"}, {"start": "16:00", "end": "18:00"}],
          "sun": [], "tue": [...], "wed": [...], "thu": [...], "fri": [...]}}
```

- `days` has all seven keys, `mon` to `sun`. Each is a list of 0 to 2 ranges; an empty list
  is off all day.
- Times are `HH:MM` (exactly five characters), 00:00 to 23:59, in the wall's time zone
  (`timezone`, section 9). An `end` can also be `"24:00"`, the end of the day. A range
  includes its start and stops at its end.
- `end` must be after `start` on the same day. The two ranges of a day must not overlap;
  they may touch (`12:00` to `14:00` and `14:00` to `16:00`).
- The saved schedule is the one sent with its ranges sorted by `start` and unknown keys
  dropped. A range ending at 24:00 followed by one starting at 00:00 the next day is one
  stretch: the wall doesn't blink at midnight.

Errors, all `400 invalid`, checked in this order (the first one wins). `<Day>` is the full
English name (`Monday`) and the error carries `"day": "mon"`:

| `error` | When |
| --- | --- |
| `Schedule must be an object with enabled and days` | `schedule` is not an object |
| `Schedule enabled must be true or false` | `enabled` missing or not a boolean |
| `Schedule days must list mon, tue, wed, thu, fri, sat and sun` | `days` not an object, or a day key missing |
| `<Day>: give a list of 0 to 2 time ranges` | A day is not a list, or has more than two |
| `<Day>: times must be HH:MM, 00:00 to 23:59 (an end can be 24:00)` | A range is not an object, or a time is missing or malformed |
| `<Day>: a time range must end after it starts` | `end` <= `start` |
| `<Day>: the two time ranges overlap` | Sorted by start, the first ends after the second starts |

**When the display is on.** While `enabled` is true and the wall's clock is valid
(`time_valid`, section 10), the display is on inside a range and off outside every range.
Off means the panels are black at brightness 0 and the render task idles (no GIF decoding,
no frames); the web server, the Instagram fetcher and everything else keep running. When it
comes back on, the rotation carries on from the screen and the time it had left. Daylight
saving follows the zone's rules: a range that starts inside the hour skipped in spring
starts at the jump (02:30 on that Sunday starts at 03:00), and in autumn a range in the
repeated hour happens twice. Setup and error messages on the wall (section 12) show even
while the display is off.

**Fail open.** While the clock is not valid, the display stays on whatever the schedule says.

**By hand.** `POST /api/control` with `"wake"` turns the display on now and `"sleep"` turns it
off now (section 11). The override lasts until the next scheduled change, then the schedule
takes over again: a wake at 22:00 when the wall is off until 05:00 lasts until 05:00, and
the schedule then keeps the wall on. A wake while the schedule already has the wall on (or a
sleep while it has it off) just clears any override. With no schedule in force (disabled, or
no clock) only a sleep is kept, and it lasts until a wake. The same goes for a schedule
that never changes (every day off, or on all day every day). Saving a schedule (any
`PUT /api/settings` with `schedule`) clears the override; saving other settings does not.
Overrides live in RAM: a restart goes back to the schedule.

Status reports it as `display_on`, `schedule_state` and `next_change` (section 10).

### 7.5 Power supplies

`power` tells the power limiter what feeds the panels (`docs/power-wiring-guide.md`):

| Key | Rule |
| --- | --- |
| `supplies` | Integer `1` or `2`. With 2, supply A feeds chain panels 1 to 6 and supply B panels 7 to 12, each against its own budget. With 1, all twelve panels count against one |
| `rated_a` | Number (not boolean) from 5 to 100: each supply's rated current in amps |
| `budget_pct` | Integer 50 to 95: the share of `rated_a` the wall loads a supply to continuously |

A supply's budget is `rated_a * (budget_pct / 100)` amps (15.3 A by default). Unknown keys
are dropped. Errors, all `400 invalid`, in this order: `Power must be an object with
supplies, rated_a and budget_pct`, `Power supplies must be 1 or 2`, `Supply rating must be 5
to 100 A`, `Power budget must be 50 to 95 percent`. For one Mean Well UHP-500-5 on its
aluminium plate: `{"supplies": 1, "rated_a": 80, "budget_pct": 85}`.

## 8. Instagram

### 8.1 `GET /api/instagram`

```json
{"enabled": true, "account_id": "17841400000000001", "username": "thestrengthtank",
 "refresh_minutes": 15, "last_fetch": "2026-11-06T20:15:00Z", "last_error": null,
 "sample": false, "token_set": true,
 "stories": [{"id": "1790...", "timestamp": "2026-11-06T18:02:11+0000", "file": "12/story-0.bmp", "kind": "image"}],
 "posts": [{"id": "1801...", "timestamp": "2026-11-05T16:40:00+0000", "file": "12/post-0.bmp", "kind": "reel",
            "like_count": 214, "permalink": "https://www.instagram.com/reel/..."}],
 "busy": null, "accounts": null, "accounts_error": null}
```

- `access_token` is never returned; `token_set` says whether one is saved.
- `stories`: at most 5, oldest first, `kind` is `image` or `video` (video shows its cover).
- `posts`: at most 2, newest first, `kind` is `image`, `video`, `reel` or `carousel`;
  `like_count` and `permalink` may be `null`.
- `last_fetch`: UTC, `YYYY-MM-DDTHH:MM:SSZ`, set at the end of every refresh, failed or not;
  `null` before the first.
- `last_error`: `null` after a refresh that worked, else a message for the coach.
- `busy`: `null`, `"refresh"` or `"accounts"`: the job running now.
- `accounts`: the result of the last finished accounts job, a list of
  `{"page": string|null, "id": string, "username": string|null}`, or `null` (never run,
  running, failed, or the token changed since).
- `accounts_error`: message from the last accounts job, or `null`.
- `sample`: `true` while the tiles are the mock's samples (mock only, see section 12).

### 8.2 `PUT /api/instagram`

Any subset of:

| Key | Rule |
| --- | --- |
| `enabled` | `true` shows the two Instagram screens; any other value is `false` |
| `account_id` | String of 0 to 32 digits |
| `refresh_minutes` | Integer 5 to 1440 |
| `access_token` | String up to 1024 characters of `A-Z a-z 0-9 _ - | .`. Any change clears `accounts` and `accounts_error`. `""` removes the token, clears `last_fetch` and `last_error`, and drops the tiles (section 12 for what the mock shows instead) |

### 8.3 Background jobs

`POST /api/instagram/refresh` and `POST /api/instagram/accounts` take no body. They answer at
once and the work runs on a worker task:

| Answer | When |
| --- | --- |
| `400 not_configured` | refresh: no token or no account ID; accounts: no token |
| `409 busy` | Either job is already running (one job at a time, of either kind) |
| `202 {"queued": true}` | Started. `busy` is set before the answer is sent |

The console polls `GET /api/instagram` about every 1.5 s until `busy` is `null`, and gives up
after 90 s. Then:

- refresh: success if `last_error` is `null`; the new `stories` and `posts` are in place.
- accounts: `accounts` (a list, possibly empty) or `accounts_error`.

The device also runs a refresh on its own every `refresh_minutes` while `enabled`, a token
and an account ID are set. A scheduled refresh that finds a job running is skipped. Other
writes are never blocked by a running job.

Graph calls (v23.0, `https://graph.facebook.com/v23.0`):

- refresh: `{account}?fields=username`, then
  `{account}/stories?fields=id,media_type,media_product_type,media_url,thumbnail_url,timestamp`,
  then `{account}/media?fields=<same>,like_count,permalink&limit=2`, then one download per
  image (`thumbnail_url` for `VIDEO`, else `media_url`; items with neither are skipped).
- accounts: `me/accounts?fields=name,instagram_business_account{id,username}`; if that fails,
  `me?fields=name,instagram_business_account{id,username}` as a single page. Pages without
  `instagram_business_account` are left out.

### 8.4 Atomic swap (refresh)

Tiles live in **generation directories** `ig/<gen>/`, where `<gen>` is a decimal number
(1 to 9 digits). A refresh:

1. Creates a new generation, one higher than the highest directory under `ig/`.
2. Makes every Graph call and downloads, decodes and crops every image into that directory.
   Nothing the wall is showing changes during this.
3. Only when all of that worked: under the config lock, re-reads `instagram.json`; if the
   token or account ID changed meanwhile, treats it as a failure. Otherwise writes
   `instagram.json` with the new `stories`/`posts` (their `file` values point into the new
   generation), `last_error: null` and `last_fetch`. **This write is the switch.**
4. Then deletes every other generation directory.

Any failure at any step (HTTP error, network error, bad URL, JPEG that won't decode, a full
disk, anything): delete the new generation directory, keep `instagram.json`'s stories, posts
and files as they were, set `last_error` to a message and `last_fetch` to now, and add the
message to `recent_errors`. The error message never contains a URL or the token.

On start, any generation directory that `instagram.json` doesn't point at is deleted (an
interrupted refresh).

## 9. Time zone and dates

`timezone` is one of these eight IANA names. The device uses the POSIX TZ string next to it.

| `timezone` | POSIX TZ |
| --- | --- |
| `America/Los_Angeles` (default) | `PST8PDT,M3.2.0,M11.1.0` |
| `America/Denver` | `MST7MDT,M3.2.0,M11.1.0` |
| `America/Phoenix` | `MST7` |
| `America/Chicago` | `CST6CDT,M3.2.0,M11.1.0` |
| `America/New_York` | `EST5EDT,M3.2.0,M11.1.0` |
| `America/Anchorage` | `AKST9AKDT,M3.2.0,M11.1.0` |
| `Pacific/Honolulu` | `HST10` |
| `UTC` | `UTC0` |

"Today" (`local_date`, and the PR stamp) is the calendar date in that zone, as
`YYYY-MM-DD`. Example: 03:30 UTC on 7 Nov 2026 is still 2026-11-06 in Los Angeles.

The console highlights recent PRs using the wall's `local_date` from `/api/status`, not the
phone's date. It falls back to the phone's date only while the wall's clock is not valid,
except for `new_prs` screens and dated announcements: while `time_valid` is false the preview
skips them, as the wall does.

## 10. Status

### `GET /api/status`

```json
{
 "mock": false,
 "fw_version": "1.0.0",
 "ui_version": "0.2.0",
 "uptime_s": 86400,
 "reset_reason": "poweron",
 "paused": false,
 "last_action": "next",
 "display_on": true,
 "schedule_state": "on",
 "next_change": "2026-11-06T21:00:00-08:00",
 "now_playing": {"slide_id": "s1", "type": "whiteboard", "file": null},
 "storage": {"used_bytes": 1048576, "total_bytes": 24117248, "usable_bytes": 21705523},
 "wifi": {"ssid": "GymWiFi", "rssi": -58},
 "time_valid": true,
 "local_date": "2026-11-06",
 "timezone": "America/Los_Angeles",
 "min_free_heap": 61440,
 "refresh_hz": 96,
 "limiter_scale": 1.0,
 "config_errors": [],
 "recent_errors": [{"time": "2026-11-06T20:15:03Z", "message": "Instagram: Couldn't reach Instagram: timed out"}]
}
```

| Field | Meaning |
| --- | --- |
| `mock` | `true` only from `mock_server.py`, whose diagnostics are placeholders |
| `fw_version` | Firmware version string |
| `ui_version` | Version of the console bundled in the same image |
| `uptime_s` | Whole seconds since boot |
| `reset_reason` | Why the board last started: `poweron`, `external`, `software`, `panic`, `int_wdt`, `task_wdt`, `wdt`, `deepsleep`, `brownout`, `sdio` or `unknown` (ESP-IDF `esp_reset_reason()`) |
| `paused`, `last_action` | From `POST /api/control`; `last_action` is `null` until the first one |
| `display_on` | Whether the panels are lit now (section 7.4) |
| `schedule_state` | `always_on` (schedule disabled), `on` or `off` (by the schedule), `override_on` or `override_off` (a wake or sleep in force), `no_clock` (schedule enabled but the clock isn't valid, so on) |
| `next_change` | When `display_on` next changes, as the wall's local time with its offset (`YYYY-MM-DDTHH:MM:SS±HH:MM`, Python's `isoformat()`), looking up to 8 days ahead; `null` for `always_on`, `no_clock`, an override that lasts until the next wake, or a schedule with no change ahead |
| `now_playing` | The slide on the wall now (also while paused): `{"slide_id", "type", "file"}`, `file` only for `media` (else `null`). `null` when no slide is enabled |
| `storage` | Same object as in `GET /api/media` (section 11) |
| `wifi` | Connected network name and signal in dBm |
| `time_valid` | `true` once NTP or the RTC has given a time after the firmware's build date |
| `local_date` | Today in the wall's zone, `YYYY-MM-DD`; `null` while `time_valid` is false |
| `timezone` | The setting, for display |
| `min_free_heap` | Lowest free internal heap since boot, bytes (`esp_get_minimum_free_heap_size()`) |
| `refresh_hz` | Panel refresh rate the display driver reports |
| `limiter_scale` | Current factor of the per-frame power limiter, 0 to 1 (1 = not dimming) |
| `config_errors` | Files found corrupt since boot (section 13.3): `{"file", "renamed_to", "time"}` |
| `recent_errors` | The last 20 errors, oldest first: `{"time": "YYYY-MM-DDTHH:MM:SSZ", "message"}`. RAM only |

## 11. Media

### `GET /api/media`

```json
{"files": [{"name": "logo.gif", "size": 91234, "frames": 1, "peak_a": 6.42, "uploaded": "2026-11-06"}],
 "storage": {"used_bytes": 1048576, "total_bytes": 24117248, "usable_bytes": 21705523}}
```

- `files` sorted by name. `frames` and `peak_a` may be `null` (unknown).
- `storage.total_bytes`: the LittleFS partition (23 MB, 24,117,248 bytes).
- `storage.usable_bytes`: `floor(total_bytes * 0.9)`. The last 10% is never filled: LittleFS
  slows down and wears unevenly when nearly full, and config writes need room.
- `storage.used_bytes`: everything stored (media, tiles, config), as the filesystem counts it.

### `POST /api/media?name=<name>&frames=<n>&peak_a=<amps>`

Body: the GIF bytes (`Content-Type: image/gif`), with `Content-Length`.

| Parameter | Rule |
| --- | --- |
| `name` | `^[a-z0-9][a-z0-9._-]{0,47}\.gif$` |
| `frames` | Optional, informational only: stored for the console's list. The wall counts frames itself while playing and never trusts this |
| `peak_a` | Optional. A finite number, `0 <= peak_a <= 40`, stored rounded to 2 decimals. Absent means `null` |

Checks, in order (after section 4): `name` (400), `peak_a` (400), body over 8 MB (413), not a
GIF (first 6 bytes `GIF87a`/`GIF89a`, at least 13 bytes) (400), logical screen not 256x96
(400), name already on the board (`409 exists`), `used_bytes + size > usable_bytes`
(`507 no_space`). The file is written as `media/<name>.part` and renamed into place, then the
index is updated.

Response: `{"name": "logo.gif", "size": 91234, "peak_a": 6.42}`.

**`peak_a` and slide brightness.** `peak_a` is the brightest frame's estimated current on the
busier half of the wall (chain panels 1 to 6 or 7 to 12) at 100% brightness, in amps, using
`estimateSupplyAmps(rgba, 100)` from `web/power.js` (the two-supply split, whatever `power`
says) on the frame `convert()` reports as `peakFrame`. It keeps that meaning when the
supplies change, so files uploaded before stay right. For the whole slide the wall uses

```
budget = power.rated_a * (power.budget_pct / 100)          (15.3 A by default)
load   = peak_a            with 2 supplies
         2 * peak_a        with 1 supply (both halves; exact for all-white, cautious otherwise)
slide brightness = min(brightness setting, floor(budget / load * 100))
```

(`slideBrightness(peakA, brightness, power)` in `web/power.js`). With `peak_a` `null` or 0 the
slide plays at the setting. The per-frame limiter (`limitedBrightness`, which measures each
frame against the real supplies) stays on as a backstop for any frame the estimate missed.

### `DELETE /api/media/{name}`

`404 not_found` if the name is invalid or missing, `409 in_use` if a playlist slide uses it,
else deletes the file and its index entry: `200 {"ok": true}`.

### `GET /media/{name}` and `GET /ig/{gen}/{tile}.bmp`

- `/media/{name}`: the GIF, `image/gif`, if `name` matches the media pattern and exists;
  else `404 not_found` JSON.
- `/ig/{gen}/{tile}.bmp`: after percent-decoding, the path must match
  `^(\d{1,9})/((story|post)-\d{1,2}\.bmp)$` exactly, and the resolved file must sit directly
  in `ig/<gen>/`. Anything else (`..`, encoded slashes, other names, a missing generation)
  is `404 not_found`. Served as `image/bmp`.

### `POST /api/control`

Body `{"action": "next" | "prev" | "pause" | "resume" | "wake" | "sleep"}`. Anything else:
`400 invalid` (`action must be next, prev, pause, resume, wake or sleep`). `wake` and `sleep`
turn the display on or off now, until the next scheduled change (section 7.4); they don't
touch `paused`. Every action sets `last_action`.

## 12. Device-only behaviour

These differ from the mock on purpose.

- **First PIN only from the setup access point.** While no PIN is set, the device accepts
  `PUT /api/settings {"new_pin": ...}` only from a client on its own access point. From the
  gym network it answers
  `403 {"error": "Set the admin PIN from the wall's setup Wi-Fi", "code": "pin_not_set"}`. The
  mock accepts the first PIN from anyone, over plain http, for development.
- **Setup access point.** Opens only on first boot (no Wi-Fi saved) or when BOOT is pressed
  within 1.5 s of the wall lighting up and held for 5 s. (BOOT held through power-on puts the
  ESP32 into download mode, so the gesture starts after it boots.) WPA2, with a password shown on the wall; the AP's name and the setup URL
  are shown too. Otherwise the board keeps retrying the saved network indefinitely and never
  falls back to an open AP.
- **BOOT-button recovery.** Pressing BOOT within 1.5 s of the wall lighting up and holding it
  for 10 s clears `pin_salt`, `pin_hash`
  and `pin_iter` (and opens the setup AP), so the next PIN is set from there.
- **Instagram samples.** The mock shows sample tiles while no token is saved (`sample: true`).
  The device has none: with no token, `stories` and `posts` are `[]`, `sample` is `false`, and
  both Instagram screens are skipped.
- **Setup page and Wi-Fi.** `GET /setup` serves a small page for the setup access point: it
  sets the first PIN (`PUT /api/settings {"new_pin"}`) and then the gym's network with
  `PUT /api/wifi {"ssid": "1 to 32 bytes", "password": "" or 8 to 63 characters}`. That route
  follows section 4 like any write (so the PIN comes first), then answers
  `403 {"error": "Change the Wi-Fi from the wall's setup Wi-Fi", "code": "setup_only"}` from any
  other network, `400 invalid` for a bad name or password, and otherwise saves the network in
  NVS, answers `200 {"ok": true, "restarting": true}` and restarts into it. The page talks to
  the same API as the console.
- **Instagram access token in NVS.** The device keeps the token in NVS (namespace `wall`, key
  `ig_token`), not in `instagram.json`, so the file never holds it. A token found in an
  `instagram.json` (written by the mock or older firmware) is moved to NVS at start.
- **Tiles for the renderer.** Next to each `ig/<gen>/<tile>.bmp` the device writes
  `ig/<gen>/<tile>.565`: the same tile as raw RGB565 little-endian, row-major, which is what
  the render task reads. `/ig/` serves only the `.bmp`.
- **First start.** The mock seeds athletes from the chalkboard CSV, one announcement
  ("Welcome to The Strength Tank") and a rotation (logo, whiteboard, New PRs,
  announcements, Instagram story and posts); the device starts from the section 13.3
  defaults (no athletes, no announcements, empty rotation).
- **Request bodies.** A JSON body sent as `application/x-www-form-urlencoded` (what `curl -d`
  and Python's urllib send when no type is given) is parsed as form fields by the web server
  before the API sees it, so the device answers `400 invalid` "Send the body as JSON
  (Content-Type: application/json)". Any other content type, or none, works. The console
  always sends `application/json`.
- **Times while the clock is invalid.** Before NTP or the RTC give a plausible time,
  `recent_errors[].time`, `config_errors[].time` and `last_fetch` are `null` on the device.
- **Storage figures.** `total_bytes` is what LittleFS reports for the `spiffs` partition, so
  on the device it is the partition's real size (0x17E0000 = 25,034,752 bytes with
  `partitions_32MB.csv`), not the mock's fixed 24,117,248. `usable_bytes` is still
  `floor(total_bytes * 0.9)`.
- **Errors only the device gives.** `500 {"code": "storage_error"}` when a flash write fails
  (nothing is changed), and `503 {"code": "busy"}` if the wall's request queue is full.
- **Config files are read at start.** The device keeps the parsed files in RAM and writes
  them through on every save; a file damaged on flash while running is found (and recovered
  as in 13.3) at the next start, not on the next read.
- **Extra status fields.** `/api/status` also carries `free_psram` (bytes),
  `firmware: {"partition", "pending_verify", "max_bytes"}` (section 15), and in `wifi`, `ip`
  and `setup_ap`. `wifi.ssid` and `wifi.rssi` are `null` while not connected.
- **When the schedule is applied.** The device works the schedule out every 15 seconds and
  straight after a schedule save or a wake or sleep, so the display switches within 15 s of
  a scheduled time (and within 15 s of the clock becoming valid). The mock works it out on
  every `GET /api/status`. Both report the same `next_change`.
- **Diagnostics.** The mock's `reset_reason`, `min_free_heap`, `refresh_hz`, `limiter_scale`
  and `wifi` are fixed placeholders, `time_valid` is `true` unless a test sets it, and
  `now_playing` is the first enabled slide.

## 13. Files on disk

All files live on LittleFS (the mock: its `--data` folder). JSON files are written to
`<name>.tmp` and renamed over the old file, so a power cut leaves the old or the new file,
never half of one.

### 13.1 Layout

```
settings.json
athletes.json
playlist.json
announcements.json
instagram.json
media_index.json
media/<name>.gif
ig/<gen>/story-<i>.bmp      i = 0..4, oldest story first
ig/<gen>/post-<i>.bmp       i = 0..1, newest post first
ig/<gen>/<tile>.565         device only: the same tile as raw RGB565 for the renderer (section 12)
```

Tile `file` values in `instagram.json` are `"<gen>/story-0.bmp"` style. Tiles are 24-bit BMP:
stories 54x96, posts 78x78, cover-cropped from the decoded JPEG.

### 13.2 JSON files

`settings.json`

```json
{"title": "THE STRENGTH TANK", "brightness": 50, "sort": "board", "pr_highlight_days": 7,
 "timezone": "America/Los_Angeles",
 "schedule": {"enabled": false, "days": {"mon": [{"start": "05:00", "end": "21:00"}], ...}},
 "power": {"supplies": 2, "rated_a": 18, "budget_pct": 85},
 "pin_salt": "32 hex chars", "pin_hash": "64 hex chars", "pin_iter": 10000}
```

Missing keys take the defaults in section 7.3; with no PIN the three `pin_` keys are `null`.
A stored `schedule` or `power` that the API would refuse (a file edited by hand) is read as
no schedule (the display stays on) or as the default supplies.

`athletes.json`: `{"rev": int, "athletes": [ ... ]}`, each athlete exactly as returned in
section 6, with `pr_dates` a map from lift name to `YYYY-MM-DD` and `pr_prev` a map from lift
name to a number or `null`. A file written before `pr_prev` existed has no `pr_prev`; it reads
as `{}`.

`playlist.json`: `{"rev": int, "slides": [ ... ]}`, as in section 7.

`announcements.json`: `{"rev": int, "announcements": [ ... ]}`, each announcement exactly as
returned in section 6a.

`instagram.json`: the object in section 8.1 without `token_set`, `busy`, `accounts` and
`accounts_error` (those live in RAM), plus `"access_token": string`. On the device the token
is in NVS instead and the file has no `access_token` (section 12).

`media_index.json`: `{"<name>.gif": {"frames": int|null, "peak_a": number|null, "uploaded": "YYYY-MM-DD"|null}}`.
`uploaded` is the wall's local date at upload (`null` if the clock wasn't valid). A GIF in
`media/` without an entry is listed with `null`s.

### 13.3 Start-up and recovery

On every start, before serving:

1. Delete `media/*.part` (interrupted uploads) and `*.tmp` (interrupted config writes).
2. Read each JSON file. If one is missing, write its defaults. If one doesn't parse, isn't
   valid UTF-8, or isn't a JSON object: rename it to `<name>.json.bad` (replacing an older
   `.bad`), write defaults, add `{"file": "<name>.json", "renamed_to": "<name>.json.bad",
   "time": now}` to `config_errors`, and add a line to `recent_errors`. Defaults:
   - settings: section 7.3, **no PIN** (a corrupt `settings.json` therefore acts like the BOOT
     PIN reset)
   - athletes: `{"rev": 1, "athletes": []}`
   - playlist: `{"rev": 1, "slides": []}`
   - announcements: `{"rev": 1, "announcements": []}`
   - instagram: enabled, empty token and account, `refresh_minutes: 15`, no stories or posts
   - media_index: rebuilt from the GIFs in `media/`, with `frames` and `peak_a` `null` and
     `uploaded` from the file's modification date
3. Delete generation directories under `ig/` that `instagram.json` doesn't point at.

The same recovery applies if a file is found corrupt while running.

## 14. Limits

| What | Limit |
| --- | --- |
| JSON request body | 256 KB (`413`) |
| Media upload | 8 MB (`413`), 256x96 GIF only |
| Athletes | 500 |
| Slides | 100 |
| Screen time | 5 to 999 s, whole seconds |
| New PRs screens | 20 athletes per pass |
| Announcements | 50, each 1 to 200 characters with at most 6 line breaks; 20 screens per pass |
| Weights | 0.5 to 500 kg in 0.5 kg steps, or blank |
| Name / notes / title | 40 / 200 / 32 characters |
| Media file name | 52 characters including `.gif` |
| PIN | 6 to 12 digits |
| `peak_a` | 0 to 40 A |
| Schedule | 0 to 2 time ranges a day, minute steps |
| Power supplies | 1 or 2, 5 to 100 A each, budget 50 to 95% |
| Storage | 23 MB total, 90% usable |
| Instagram | 5 stories, 2 posts, one job at a time, refresh every 5 to 1440 min |
| `recent_errors` | Last 20 |

## 15. Firmware update (device only)

### `POST /api/firmware`

Body: the application image (`firmware.bin` from `pio run -e wall`), raw, with
`Content-Length` (`Content-Type: application/octet-stream`). Section 4 applies first. Then:

| Answer | When |
| --- | --- |
| `413 too_large` | Body larger than the OTA slot (`firmware.max_bytes` in `/api/status`, 4 MB) |
| `400 invalid` | Not an ESP32-S3 app image for this project (image magic, chip, app descriptor, project name), or damaged/incomplete (checksum or SHA-256 fails when the image is closed) |
| `409 busy` | Another firmware upload is being written (`"busy": "firmware"`) |
| `500 storage_error` | The slot couldn't be written |
| `200 {"ok": true, "version": "...", "rebooting": true}` | Written and selected; the wall restarts about 1.5 s later |

The body streams into the inactive OTA slot as it arrives; nothing is written before the PIN
is accepted. Rollback: the new image starts on trial (`firmware.pending_verify: true`). It is
marked good after a minute of health (network up or setup access point open, the render task
has drawn, the API task alive). If it resets before that (crash, watchdog, power cut) the
bootloader goes back to the previous image; if it isn't healthy within ten minutes it rolls
itself back.

```sh
curl -H "X-Admin-Pin: $PIN" -H "Content-Type: application/octet-stream" \
     --data-binary @.pio/build/wall/firmware.bin http://gymboard.local/api/firmware
```
