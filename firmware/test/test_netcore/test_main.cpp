// Host tests for lib/netcore: the cases of tests/test_mock_server.py that exercise rules
// (validation, PIN, lockout, revisions, PR dates, time zones, Instagram planning), run
// against the code the device runs. Network and storage cases are covered on the board by
// tests/device_contract.py.
#include <unity.h>

#include <ArduinoJson.h>
#include <string.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "netcore.h"

using namespace netcore;

void setUp() {}
void tearDown() {}

namespace {

const char* const kPin = "135790";

JsonDocument parse(const char* json) {
  JsonDocument d;
  TEST_ASSERT_EQUAL_MESSAGE(0, static_cast<int>(deserializeJson(d, json).code()), json);
  return d;
}

std::string dump(JsonVariantConst v) {
  std::string s;
  serializeJson(v, s);
  return s;
}

void fakeRandom(uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; i++) p[i] = static_cast<uint8_t>(i * 7 + 1);
}

// The mock's Store.clock, in milliseconds.
struct Clock {
  int64_t ms = 1000 * 1000;
  std::function<int64_t()> fn() {
    return [this] { return ms; };
  }
};

// A settings object with kPin set.
JsonDocument settingsWithPin() {
  JsonDocument s;
  defaultSettings(s.to<JsonObject>());
  JsonDocument body = parse("{\"new_pin\": \"135790\"}");
  TEST_ASSERT_TRUE(applySettings(body.as<JsonObjectConst>(), s.as<JsonObject>(), fakeRandom).ok());
  return s;
}

ApiError putAthletes(const char* athletesJson, JsonDocument* out = nullptr) {
  JsonDocument in = parse(athletesJson);
  JsonDocument tmp;
  JsonDocument& o = out ? *out : tmp;
  o.clear();
  return validateAthletes(in.as<JsonArrayConst>(), o.to<JsonArray>());
}

ApiError putSlides(const char* slidesJson, std::set<std::string> media = {}) {
  JsonDocument in = parse(slidesJson);
  JsonDocument out;
  return validatePlaylist(in.as<JsonArrayConst>(), media, out.to<JsonArray>());
}

}  // namespace

// ---- hashing -----------------------------------------------------------------------------------

void test_sha256_vectors() {
  uint8_t out[32];
  sha256("abc", 3, out);
  TEST_ASSERT_EQUAL_STRING("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", hex(out, 32).c_str());
  const std::string a(1000, 'a');
  sha256(a.data(), a.size(), out);
  TEST_ASSERT_EQUAL_STRING("41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3", hex(out, 32).c_str());
}

// test_pin_stored_as_pbkdf2: the values come from Python's hashlib.pbkdf2_hmac.
void test_pbkdf2_matches_python() {
  uint8_t salt[16], out[32];
  TEST_ASSERT_TRUE(unhex("00112233445566778899aabbccddeeff", salt, 16));
  pbkdf2Sha256(kPin, salt, 16, 10000, out);
  TEST_ASSERT_EQUAL_STRING("1be5f73641b67b5e911dab2fda73c46b82fe0d54d04b3161324e04667d58c022", hex(out, 32).c_str());
  pbkdf2Sha256(kPin, salt, 16, 1, out);
  TEST_ASSERT_EQUAL_STRING("0046390474f1946fcecc1e4be003ceaf6aa2ae751ad63d3ebca4851ab31bf99b", hex(out, 32).c_str());
}

void test_pin_record_and_match() {
  JsonDocument s = settingsWithPin();
  TEST_ASSERT_EQUAL(10000, s["pin_iter"].as<int>());
  TEST_ASSERT_EQUAL(32, strlen(s["pin_salt"].as<const char*>()));
  TEST_ASSERT_EQUAL(64, strlen(s["pin_hash"].as<const char*>()));
  TEST_ASSERT_TRUE(pinSet(s.as<JsonObjectConst>()));
  TEST_ASSERT_TRUE(pinMatches(kPin, s["pin_salt"].as<std::string>(), s["pin_hash"].as<std::string>(), 10000));
  TEST_ASSERT_FALSE(pinMatches("135791", s["pin_salt"].as<std::string>(), s["pin_hash"].as<std::string>(), 10000));
  TEST_ASSERT_FALSE(pinMatches(std::string(65, '1'), s["pin_salt"].as<std::string>(), s["pin_hash"].as<std::string>(), 10000));
  JsonDocument fresh;
  defaultSettings(fresh.to<JsonObject>());
  TEST_ASSERT_FALSE(pinSet(fresh.as<JsonObjectConst>()));
}

// ---- authorization (API.md 4.2) -------------------------------------------------------

void test_every_write_needs_a_pin_first() {
  Clock clock;
  PinGuard guard(clock.fn());
  JsonDocument s;
  defaultSettings(s.to<JsonObject>());
  int verifyCalls = 0;
  auto verify = [&](const std::string&) {
    verifyCalls++;
    return true;
  };
  WriteRequest req;
  req.ip = "10.0.0.2";
  req.hasPinHeader = true;
  req.pin = "123456";
  ApiError e = authorizeWrite(guard, s.as<JsonObjectConst>(), req, verify);
  TEST_ASSERT_EQUAL(403, e.status);
  TEST_ASSERT_EQUAL_STRING("pin_not_set", e.code.c_str());
  TEST_ASSERT_EQUAL_STRING("Set an admin PIN first", e.message.c_str());
  TEST_ASSERT_FALSE(isFirstPinBody(parse("{\"new_pin\": \"135790\", \"brightness\": 40}").as<JsonVariantConst>()));
  TEST_ASSERT_FALSE(isFirstPinBody(parse("[\"new_pin\"]").as<JsonVariantConst>()));
  TEST_ASSERT_TRUE(isFirstPinBody(parse("{\"new_pin\": \"135790\"}").as<JsonVariantConst>()));
  req.firstPinRequest = true;
  TEST_ASSERT_TRUE(authorizeWrite(guard, s.as<JsonObjectConst>(), req, verify).ok());  // the mock: from anywhere
  req.deviceRules = true;  // the device: only from the setup access point
  e = authorizeWrite(guard, s.as<JsonObjectConst>(), req, verify);
  TEST_ASSERT_EQUAL(403, e.status);
  TEST_ASSERT_EQUAL_STRING("pin_not_set", e.code.c_str());
  TEST_ASSERT_EQUAL_STRING("Set the admin PIN from the wall's setup Wi-Fi", e.message.c_str());
  req.fromSetupAp = true;
  TEST_ASSERT_TRUE(authorizeWrite(guard, s.as<JsonObjectConst>(), req, verify).ok());
  TEST_ASSERT_EQUAL(0, verifyCalls);  // no PIN set: nothing to hash
}

void test_pin_guards_writes() {
  Clock clock;
  PinGuard guard(clock.fn());
  JsonDocument s = settingsWithPin();
  auto verify = [&](const std::string& pin) {
    return pinMatches(pin, s["pin_salt"].as<std::string>(), s["pin_hash"].as<std::string>(), 10000);
  };
  WriteRequest req;
  req.ip = "10.0.0.2";
  ApiError e = authorizeWrite(guard, s.as<JsonObjectConst>(), req, verify);
  TEST_ASSERT_EQUAL(401, e.status);
  TEST_ASSERT_EQUAL_STRING("pin_required", e.code.c_str());
  req.hasPinHeader = true;
  req.pin = "000000";
  e = authorizeWrite(guard, s.as<JsonObjectConst>(), req, verify);
  TEST_ASSERT_EQUAL(401, e.status);
  TEST_ASSERT_EQUAL_STRING("pin_wrong", e.code.c_str());
  TEST_ASSERT_EQUAL_STRING("That PIN is not right", e.message.c_str());
  req.pin = kPin;
  TEST_ASSERT_TRUE(authorizeWrite(guard, s.as<JsonObjectConst>(), req, verify).ok());
  JsonDocument pub;
  settingsPublic(s.as<JsonObjectConst>(), pub.to<JsonObject>());
  const std::string text = dump(pub.as<JsonVariantConst>());
  TEST_ASSERT_TRUE(text.find("pin_hash") == std::string::npos);
  TEST_ASSERT_TRUE(text.find("pin_salt") == std::string::npos);
  TEST_ASSERT_TRUE(text.find("pin_iter") == std::string::npos);
  TEST_ASSERT_TRUE(pub["pin_set"].as<bool>());
}

// LockoutTest.test_client_lock_doubles_and_resets, through authorizeWrite.
struct LockFixture {
  Clock clock;
  PinGuard guard{clock.fn()};
  JsonDocument s = settingsWithPin();
  int verifyCalls = 0;
  ApiError attempt(const char* pin, const char* ip = "127.0.0.1") {
    WriteRequest req;
    req.ip = ip;
    req.hasPinHeader = true;
    req.pin = pin;
    return authorizeWrite(guard, s.as<JsonObjectConst>(), req, [&](const std::string& p) {
      verifyCalls++;
      return p == kPin;  // the hash itself is tested above; keep the lockout tests fast
    });
  }
  ApiError wrong() { return attempt("000000"); }
  ApiError right() { return attempt(kPin); }
  void advance(double seconds) { clock.ms += static_cast<int64_t>(seconds * 1000); }
};

void assertLocked(const ApiError& e, int seconds) {
  TEST_ASSERT_EQUAL(429, e.status);
  TEST_ASSERT_EQUAL_STRING("locked", e.code.c_str());
  TEST_ASSERT_EQUAL(seconds, e.retryAfter);
  TEST_ASSERT_EQUAL_STRING(("Too many wrong PINs. Try again in " + std::to_string(seconds) + " s.").c_str(),
                           e.message.c_str());
  JsonDocument d;
  e.toJson(d.to<JsonObject>());
  TEST_ASSERT_EQUAL(seconds, d["retry_after_s"].as<int>());
}

void test_client_lock_doubles_and_resets() {
  LockFixture f;
  for (int i = 0; i < 4; i++) TEST_ASSERT_EQUAL(401, f.wrong().status);
  TEST_ASSERT_TRUE(f.right().ok());  // a correct PIN resets the count
  for (int i = 0; i < 4; i++) TEST_ASSERT_EQUAL(401, f.wrong().status);
  TEST_ASSERT_EQUAL_STRING("pin_wrong", f.wrong().code.c_str());  // 5th wrong: answered, then locked
  const int calls = f.verifyCalls;
  assertLocked(f.right(), 30);  // locked: even the right PIN isn't checked
  TEST_ASSERT_EQUAL(calls, f.verifyCalls);
  f.advance(29.5);
  assertLocked(f.right(), 1);
  f.advance(0.5);
  TEST_ASSERT_EQUAL(401, f.wrong().status);  // 6th wrong: 60 s
  assertLocked(f.wrong(), 60);
  f.advance(60);
  TEST_ASSERT_EQUAL(401, f.wrong().status);  // 7th: 120 s
  assertLocked(f.right(), 120);
  for (int i = 0; i < 6; i++) {  // 240, 480, 900 (cap), 900 ...
    f.advance(1000);
    f.wrong();
  }
  assertLocked(f.right(), 900);
  f.advance(900);
  TEST_ASSERT_TRUE(f.right().ok());
  for (int i = 0; i < 4; i++) TEST_ASSERT_EQUAL(401, f.wrong().status);
  TEST_ASSERT_TRUE(f.right().ok());
}

void test_missing_pin_is_not_a_wrong_pin() {
  LockFixture f;
  for (int i = 0; i < 12; i++) {
    WriteRequest req;
    req.ip = "127.0.0.1";
    ApiError e = authorizeWrite(f.guard, f.s.as<JsonObjectConst>(), req, [](const std::string&) { return false; });
    TEST_ASSERT_EQUAL_STRING("pin_required", e.code.c_str());
  }
  WriteRequest empty;  // an empty header carries no guess either
  empty.ip = "127.0.0.1";
  empty.hasPinHeader = true;
  TEST_ASSERT_EQUAL_STRING("pin_required",
                           authorizeWrite(f.guard, f.s.as<JsonObjectConst>(), empty, [](const std::string&) { return false; }).code.c_str());
  TEST_ASSERT_EQUAL(0, f.guard.waitSeconds("127.0.0.1"));
}

void test_long_pin_header_is_wrong_without_hashing() {
  LockFixture f;
  const std::string longPin(65, '1');
  ApiError e = f.attempt(longPin.c_str());
  TEST_ASSERT_EQUAL_STRING("pin_wrong", e.code.c_str());
  TEST_ASSERT_EQUAL(0, f.verifyCalls);
}

void test_global_lock() {
  LockFixture f;
  for (int i = 0; i < 19; i++) f.guard.fail("10.0.0." + std::to_string(i));
  TEST_ASSERT_TRUE(f.right().ok());
  f.advance(601);  // the first 19 have aged out of the 10 minute window
  for (int i = 0; i < 19; i++) f.guard.fail("10.0.1." + std::to_string(i));
  TEST_ASSERT_TRUE(f.right().ok());
  f.guard.fail("10.0.2.1");  // 20 within 10 minutes
  assertLocked(f.right(), 900);
  assertLocked(f.attempt(kPin, "192.168.1.50"), 900);  // every client
  f.advance(900);
  TEST_ASSERT_TRUE(f.right().ok());
}

void test_client_table_is_bounded() {
  LockFixture f;
  for (int i = 0; i < 500; i++) {
    f.guard.fail("10.1." + std::to_string(i / 250) + "." + std::to_string(i % 250));
    f.advance(31);  // stay under the global limit
  }
  TEST_ASSERT_TRUE(f.guard.clientCount() <= 32);
  TEST_ASSERT_TRUE(f.guard.clientCount() >= 16);
}

// PinGuardTest.test_schedule
void test_guard_schedule() {
  Clock clock;
  clock.ms = 0;
  PinGuard g(clock.fn());
  const int want[12] = {0, 0, 0, 0, 30, 60, 120, 240, 480, 900, 900, 900};
  for (int i = 0; i < 12; i++) {
    g.fail("a");
    TEST_ASSERT_EQUAL_MESSAGE(want[i], g.waitSeconds("a"), std::to_string(i).c_str());
    clock.ms += 1000 * 1000;  // let each lock run out, staying under the global limit
  }
  TEST_ASSERT_EQUAL(0, g.waitSeconds("b"));
  g.succeed("a");
  g.fail("a");
  TEST_ASSERT_EQUAL(0, g.waitSeconds("a"));
}

// ---- revisions (API.md 5) ------------------------------------------------------------------

void test_rev_check() {
  const long long rev = 4;
  TEST_ASSERT_TRUE(checkRev(parse("{\"rev\": 4, \"athletes\": []}").as<JsonVariantConst>(), rev).ok());
  for (const char* stale : {"{\"rev\": 3, \"athletes\": []}", "{\"rev\": 6, \"athletes\": []}", "{\"athletes\": []}",
                            "{\"rev\": \"4\", \"athletes\": []}", "{\"rev\": 4.0, \"athletes\": []}",
                            "{\"rev\": true, \"athletes\": []}", "{\"rev\": null, \"athletes\": []}"}) {
    ApiError e = checkRev(parse(stale).as<JsonVariantConst>(), rev);
    TEST_ASSERT_EQUAL_MESSAGE(409, e.status, stale);
    TEST_ASSERT_EQUAL_STRING("stale", e.code.c_str());
    TEST_ASSERT_EQUAL_STRING("Someone else saved changes. Reload to see them.", e.message.c_str());
    JsonDocument d;
    e.toJson(d.to<JsonObject>());
    TEST_ASSERT_EQUAL(4, d["rev"].as<int>());
  }
  TEST_ASSERT_EQUAL(1, revOf(parse("{\"athletes\": []}").as<JsonVariantConst>()));
  TEST_ASSERT_EQUAL(1, revOf(parse("{\"rev\": \"7\"}").as<JsonVariantConst>()));
  TEST_ASSERT_EQUAL(7, revOf(parse("{\"rev\": 7}").as<JsonVariantConst>()));
}

void test_shape_errors() {
  TEST_ASSERT_TRUE(requireObject(parse("{\"rev\": 1, \"athletes\": []}").as<JsonVariantConst>(), "athletes").ok());
  ApiError e = requireObject(parse("{\"rev\": 1, \"athletes\": {}}").as<JsonVariantConst>(), "athletes");
  TEST_ASSERT_EQUAL(400, e.status);
  TEST_ASSERT_EQUAL_STRING("Expected {\"rev\": n, \"athletes\": [...]}", e.message.c_str());
  JsonDocument nul;
  TEST_ASSERT_EQUAL_STRING("Expected an object", requireObject(nul.as<JsonVariantConst>()).message.c_str());
}

// ---- bodies ------------------------------------------------------------------------------------

void test_body_parsing() {
  JsonDocument d;
  TEST_ASSERT_TRUE(parseJsonBody("", 0, d).ok());
  TEST_ASSERT_TRUE(d.isNull());
  const char* good = " {\"a\": [1, 2]}\n";
  TEST_ASSERT_TRUE(parseJsonBody(good, strlen(good), d).ok());
  for (const char* bad : {"{", "{} x", "{\"a\": 1,}", "nope", "{\"a\": \"\xff\"}"}) {
    ApiError e = parseJsonBody(bad, strlen(bad), d);
    TEST_ASSERT_EQUAL_MESSAGE(400, e.status, bad);
    TEST_ASSERT_EQUAL_STRING("Body is not valid JSON", e.message.c_str());
    TEST_ASSERT_EQUAL_STRING("invalid", e.code.c_str());
  }
  // Config files: CorruptConfigTest's three shapes of damage.
  for (const char* bad : {"{\"rev\": 3, \"athletes\": [", "[1, 2]", "\xff\xfe garbage", "not json", ""}) {
    TEST_ASSERT_FALSE_MESSAGE(parseConfigObject(bad, strlen(bad), d), bad);
  }
  TEST_ASSERT_TRUE(parseConfigObject("{\"rev\": 1}", 10, d));
}

void test_weights_half_kilo() {
  struct Case {
    const char* v;
    bool ok;
  } cases[] = {{"136.5", true}, {"120", true}, {"null", true}, {"136.3", false},
               {"0", false},    {"501", false}, {"\"90\"", false}, {"true", false}, {"500", true}, {"0.5", true}};
  for (const Case& c : cases) {
    const std::string body = std::string("[{\"id\": \"a1\", \"name\": \"x\", \"notes\": \"\", \"visible\": true, \"snatch\": ") + c.v + "}]";
    TEST_ASSERT_EQUAL_MESSAGE(c.ok, putAthletes(body.c_str()).ok(), c.v);
  }
  ApiError e = putAthletes("[{\"id\": \"a1\", \"name\": \" Ann \", \"snatch\": 136.3}]");
  TEST_ASSERT_EQUAL_STRING(" Ann : snatch must be blank or 0.5 to 500 kg in 0.5 kg steps", e.message.c_str());
}

void test_duplicate_ids_rejected() {
  ApiError e = putAthletes("[{\"id\": \"a1\", \"name\": \"x\"}, {\"id\": \"a1\", \"name\": \"y\"}]");
  TEST_ASSERT_EQUAL(400, e.status);
  TEST_ASSERT_EQUAL_STRING("invalid", e.code.c_str());
  TEST_ASSERT_EQUAL_STRING("Athlete 2: missing or duplicate id", e.message.c_str());
  TEST_ASSERT_EQUAL(400, putAthletes("[{\"id\": \"a 1\", \"name\": \"x\"}]").status);
  TEST_ASSERT_EQUAL(400, putAthletes("[{\"id\": \"a1\\n\", \"name\": \"x\"}]").status);
  TEST_ASSERT_EQUAL(400, putAthletes("[{\"id\": \"123456789012345678901234567890123\", \"name\": \"x\"}]").status);
  TEST_ASSERT_EQUAL_STRING("Athlete 1 is not an object", putAthletes("[3]").message.c_str());
}

void test_athlete_cleaning() {
  JsonDocument out;
  TEST_ASSERT_TRUE(putAthletes("[{\"id\": \"a1\", \"name\": \"  Ann  \", \"snatch\": 80, \"clean_jerk\": 100.5, "
                               "\"visible\": 0, \"extra\": 1, \"pr_dates\": {\"snatch\": \"2020-01-01\"}}]", &out).ok());
  TEST_ASSERT_EQUAL_STRING(
      "[{\"id\":\"a1\",\"name\":\"Ann\",\"visible\":true,\"snatch\":80,\"clean_jerk\":100.5,\"total\":null,"
      "\"back_squat\":null,\"front_squat\":null,\"notes\":\"\"}]",
      dump(out.as<JsonVariantConst>()).c_str());
  TEST_ASSERT_TRUE(putAthletes("[{\"id\": \"a1\", \"name\": \"x\", \"visible\": false}]", &out).ok());
  TEST_ASSERT_FALSE(out[0]["visible"].as<bool>());
  // 40 code points is fine even in multi-byte UTF-8; 41 is not.
  std::string forty, fortyOne;
  for (int i = 0; i < 40; i++) forty += "\xc3\xa9";
  fortyOne = forty + "e";
  TEST_ASSERT_TRUE(putAthletes(("[{\"id\": \"a1\", \"name\": \"" + forty + "\"}]").c_str()).ok());
  TEST_ASSERT_EQUAL_STRING("Athlete 1: name must be 1 to 40 characters",
                           putAthletes(("[{\"id\": \"a1\", \"name\": \"" + fortyOne + "\"}]").c_str()).message.c_str());
  TEST_ASSERT_EQUAL(400, putAthletes("[{\"id\": \"a1\", \"name\": \"   \"}]").status);
  TEST_ASSERT_EQUAL(400, putAthletes("[{\"id\": \"a1\", \"name\": \"x\", \"notes\": null}]").status);
  const std::string notes(201, 'n');
  TEST_ASSERT_EQUAL_STRING("x: notes must be text up to 200 characters",
                           putAthletes(("[{\"id\": \"a1\", \"name\": \"x\", \"notes\": \"" + notes + "\"}]").c_str()).message.c_str());
  std::string many = "[";
  for (int i = 0; i < 501; i++) many += std::string(i ? "," : "") + "{\"id\": \"a" + std::to_string(i) + "\", \"name\": \"x\"}";
  many += "]";
  TEST_ASSERT_EQUAL_STRING("At most 500 athletes", putAthletes(many.c_str()).message.c_str());
}

void test_screen_time_limits() {
  struct Case {
    const char* d;
    bool ok;
  } cases[] = {{"5", true}, {"999", true}, {"4", false}, {"1000", false}, {"7.5", false}, {"\"10\"", false}, {"true", false}, {"10.0", false}};
  for (const Case& c : cases) {
    const std::string body = std::string("[{\"id\": \"s1\", \"type\": \"whiteboard\", \"duration_s\": ") + c.d + ", \"enabled\": true}]";
    TEST_ASSERT_EQUAL_MESSAGE(c.ok, putSlides(body.c_str()).ok(), c.d);
  }
  TEST_ASSERT_EQUAL_STRING("Slide 1: duration must be a whole number of seconds from 5 to 999",
                           putSlides("[{\"id\": \"s1\", \"type\": \"whiteboard\", \"duration_s\": 4}]").message.c_str());
}

void test_slide_types_and_media() {
  for (const char* t : {"instagram_story", "instagram_posts", "whiteboard", "new_prs"}) {
    const std::string body = std::string("[{\"id\": \"s1\", \"type\": \"") + t + "\", \"duration_s\": 6, \"enabled\": true}]";
    TEST_ASSERT_TRUE_MESSAGE(putSlides(body.c_str()).ok(), t);
  }
  ApiError e = putSlides("[{\"id\": \"s1\", \"type\": \"tiktok\", \"duration_s\": 6}]");
  TEST_ASSERT_EQUAL_STRING("Slide 1: type must be one of whiteboard, media, instagram_story, instagram_posts, new_prs, announcements",
                           e.message.c_str());
  e = putSlides("[{\"id\": \"s1\", \"type\": \"media\", \"file\": \"nope.gif\", \"duration_s\": 10, \"enabled\": true}]");
  TEST_ASSERT_EQUAL(400, e.status);
  TEST_ASSERT_EQUAL_STRING("Slide 1: media file 'nope.gif' is not on the board", e.message.c_str());
  e = putSlides("[{\"id\": \"s1\", \"type\": \"media\", \"duration_s\": 10}]");
  TEST_ASSERT_EQUAL_STRING("Slide 1: media file None is not on the board", e.message.c_str());
  TEST_ASSERT_TRUE(putSlides("[{\"id\": \"s1\", \"type\": \"media\", \"file\": \"a.gif\", \"duration_s\": 10}]", {"a.gif"}).ok());
  // file is dropped for other types; enabled defaults to true.
  JsonDocument in = parse("[{\"id\": \"s1\", \"type\": \"whiteboard\", \"file\": \"a.gif\", \"duration_s\": 9, \"enabled\": \"no\"}]");
  JsonDocument out;
  TEST_ASSERT_TRUE(validatePlaylist(in.as<JsonArrayConst>(), {}, out.to<JsonArray>()).ok());
  TEST_ASSERT_EQUAL_STRING("[{\"id\":\"s1\",\"type\":\"whiteboard\",\"duration_s\":9,\"enabled\":true}]",
                           dump(out.as<JsonVariantConst>()).c_str());
  TEST_ASSERT_EQUAL_STRING("Slide 2: missing or duplicate id",
                           putSlides("[{\"id\": \"s1\", \"type\": \"whiteboard\", \"duration_s\": 9}, {\"id\": \"s1\", \"type\": \"whiteboard\", \"duration_s\": 9}]").message.c_str());
}

// ---- settings and Instagram --------------------------------------------------------------------

ApiError putSettings(JsonDocument& s, const char* body) {
  JsonDocument copy = s;  // the route works on a copy and drops it on error
  JsonDocument b = parse(body);
  ApiError e = applySettings(b.as<JsonObjectConst>(), copy.as<JsonObject>(), fakeRandom);
  if (e.ok()) s = copy;
  return e;
}

void test_settings_rules() {
  JsonDocument s = settingsWithPin();
  for (const char* bad : {"{\"new_pin\": \"12345\"}", "{\"new_pin\": \"1234567890123\"}", "{\"new_pin\": \"12a456\"}",
                          "{\"new_pin\": 123456}", "{\"new_pin\": \"\"}"}) {
    ApiError e = putSettings(s, bad);
    TEST_ASSERT_EQUAL_MESSAGE(400, e.status, bad);
    TEST_ASSERT_EQUAL_STRING("invalid", e.code.c_str());
    TEST_ASSERT_EQUAL_STRING("The PIN must be 6 to 12 digits", e.message.c_str());
  }
  for (const char* tz : {"{\"timezone\": \"Europe/London\"}", "{\"timezone\": \"PST\"}", "{\"timezone\": \"\"}", "{\"timezone\": null}"}) {
    TEST_ASSERT_EQUAL_MESSAGE(400, putSettings(s, tz).status, tz);
  }
  TEST_ASSERT_EQUAL_STRING(
      "Time zone must be one of America/Los_Angeles, America/Denver, America/Phoenix, America/Chicago, "
      "America/New_York, America/Anchorage, Pacific/Honolulu, UTC",
      putSettings(s, "{\"timezone\": \"PST\"}").message.c_str());
  TEST_ASSERT_EQUAL(400, putSettings(s, "{\"brightness\": 4}").status);
  TEST_ASSERT_EQUAL(400, putSettings(s, "{\"brightness\": 50.0}").status);
  TEST_ASSERT_EQUAL(400, putSettings(s, "{\"sort\": \"weight\"}").status);
  TEST_ASSERT_EQUAL(400, putSettings(s, "{\"pr_highlight_days\": 61}").status);
  TEST_ASSERT_EQUAL(400, putSettings(s, "{\"title\": \"123456789012345678901234567890123\"}").status);
  // Checks stop at the first bad value and nothing is written.
  TEST_ASSERT_EQUAL(400, putSettings(s, "{\"title\": \"NEW\", \"brightness\": 200}").status);
  TEST_ASSERT_EQUAL_STRING("THE STRENGTH TANK", s["title"].as<const char*>());
  TEST_ASSERT_TRUE(putSettings(s, "{\"title\": \"NEW\", \"brightness\": 40, \"timezone\": \"UTC\", \"unknown\": 1}").ok());
  TEST_ASSERT_EQUAL(40, s["brightness"].as<int>());
  const std::string oldHash = s["pin_hash"].as<std::string>();
  TEST_ASSERT_TRUE(putSettings(s, "{\"new_pin\": \"24681357\"}").ok());
  TEST_ASSERT_TRUE(pinMatches("24681357", s["pin_salt"].as<std::string>(), s["pin_hash"].as<std::string>(), 10000));
  TEST_ASSERT_FALSE(pinMatches(kPin, s["pin_salt"].as<std::string>(), s["pin_hash"].as<std::string>(), 10000));
  TEST_ASSERT_TRUE(oldHash != s["pin_hash"].as<std::string>());
}

void test_settings_defaults_fill() {
  JsonDocument s = parse("{\"brightness\": 30, \"custom\": 1}");
  fillSettingsDefaults(s.as<JsonObject>());
  JsonDocument pub;
  settingsPublic(s.as<JsonObjectConst>(), pub.to<JsonObject>());
  TEST_ASSERT_EQUAL_STRING(
      "{\"title\":\"THE STRENGTH TANK\",\"brightness\":30,\"sort\":\"board\",\"pr_highlight_days\":7,"
      "\"timezone\":\"America/Los_Angeles\",\"schedule\":{\"enabled\":false,\"days\":{"
      "\"mon\":[{\"start\":\"05:00\",\"end\":\"21:00\"}],\"tue\":[{\"start\":\"05:00\",\"end\":\"21:00\"}],"
      "\"wed\":[{\"start\":\"05:00\",\"end\":\"21:00\"}],\"thu\":[{\"start\":\"05:00\",\"end\":\"21:00\"}],"
      "\"fri\":[{\"start\":\"05:00\",\"end\":\"21:00\"}],\"sat\":[{\"start\":\"05:00\",\"end\":\"21:00\"}],"
      "\"sun\":[{\"start\":\"05:00\",\"end\":\"21:00\"}]}},"
      "\"power\":{\"supplies\":2,\"rated_a\":18,\"budget_pct\":85},\"custom\":1,\"pin_set\":false}",
      dump(pub.as<JsonVariantConst>()).c_str());
}

void test_instagram_settings() {
  JsonDocument ig;
  defaultInstagram(ig.to<JsonObject>());
  bool touched = false;
  JsonDocument b = parse("{\"access_token\": \"EAAB-test_token.1\", \"account_id\": \"17841400000000000\", \"refresh_minutes\": 30}");
  TEST_ASSERT_TRUE(applyInstagram(b.as<JsonObjectConst>(), ig.as<JsonObject>(), &touched).ok());
  TEST_ASSERT_TRUE(touched);
  TEST_ASSERT_EQUAL(30, ig["refresh_minutes"].as<int>());
  for (const char* bad : {"{\"refresh_minutes\": 4}", "{\"account_id\": \"abc\"}", "{\"access_token\": \"has spaces\"}",
                          "{\"refresh_minutes\": 1441}", "{\"account_id\": 123}"}) {
    JsonDocument bb = parse(bad);
    TEST_ASSERT_EQUAL_MESSAGE(400, applyInstagram(bb.as<JsonObjectConst>(), ig.as<JsonObject>(), &touched).status, bad);
  }
  JsonDocument en = parse("{\"enabled\": 1}");
  TEST_ASSERT_TRUE(applyInstagram(en.as<JsonObjectConst>(), ig.as<JsonObject>(), &touched).ok());
  TEST_ASSERT_FALSE(ig["enabled"].as<bool>());  // anything but true is false
  TEST_ASSERT_FALSE(touched);
  ig["last_error"] = "x";
  ig["stories"].add<JsonObject>()["file"] = "3/story-0.bmp";
  JsonDocument clear = parse("{\"access_token\": \"\"}");
  TEST_ASSERT_TRUE(applyInstagram(clear.as<JsonObjectConst>(), ig.as<JsonObject>(), &touched).ok());
  TEST_ASSERT_TRUE(ig["last_error"].isNull());
  TEST_ASSERT_TRUE(ig["last_fetch"].isNull());
  TEST_ASSERT_EQUAL(0, ig["stories"].size());
  TEST_ASSERT_FALSE(ig["sample"].as<bool>());
}

void test_control() {
  std::string action;
  TEST_ASSERT_TRUE(parseControl(parse("{\"action\": \"pause\"}").as<JsonVariantConst>(), &action).ok());
  TEST_ASSERT_EQUAL_STRING("pause", action.c_str());
  for (const char* a : {"wake", "sleep", "next", "resume"}) {
    TEST_ASSERT_TRUE(parseControl(parse((std::string("{\"action\": \"") + a + "\"}").c_str()).as<JsonVariantConst>(), &action).ok());
    TEST_ASSERT_EQUAL_STRING(a, action.c_str());
  }
  const ApiError bad = parseControl(parse("{\"action\": \"explode\"}").as<JsonVariantConst>(), &action);
  TEST_ASSERT_EQUAL(400, bad.status);
  TEST_ASSERT_EQUAL_STRING("action must be next, prev, pause, resume, wake or sleep", bad.message.c_str());
  JsonDocument nul;
  TEST_ASSERT_EQUAL(400, parseControl(nul.as<JsonVariantConst>(), &action).status);
}

// ---- media -------------------------------------------------------------------------------------

std::vector<uint8_t> gif(int w = 256, int h = 96) {
  std::vector<uint8_t> d = {'G', 'I', 'F', '8', '9', 'a'};
  d.push_back(w & 255), d.push_back(w >> 8), d.push_back(h & 255), d.push_back(h >> 8);
  d.insert(d.end(), {0, 0, 0, 0x3b});
  return d;
}

void test_upload_checks() {
  UploadParams p;
  const std::string bad = "../x.gif", ok = "a.gif";
  ApiError e = parseUploadQuery(&bad, nullptr, nullptr, &p);
  TEST_ASSERT_EQUAL(400, e.status);
  TEST_ASSERT_EQUAL_STRING("File name must be lowercase letters, digits, . _ - and end in .gif", e.message.c_str());
  TEST_ASSERT_EQUAL(400, parseUploadQuery(nullptr, nullptr, nullptr, &p).status);
  for (const char* n : {"A.gif", "a.GIF", ".a.gif", "a.gif\n", "a b.gif", ".gif"}) {
    const std::string s = n;
    TEST_ASSERT_EQUAL_MESSAGE(400, parseUploadQuery(&s, nullptr, nullptr, &p).status, n);
  }
  const std::string longest = std::string(48, 'a') + ".gif", tooLong = std::string(49, 'a') + ".gif";
  TEST_ASSERT_TRUE(parseUploadQuery(&longest, nullptr, nullptr, &p).ok());
  TEST_ASSERT_EQUAL(400, parseUploadQuery(&tooLong, nullptr, nullptr, &p).status);

  const std::string peak = "12.345", frames = "2";
  TEST_ASSERT_TRUE(parseUploadQuery(&ok, &peak, &frames, &p).ok());
  TEST_ASSERT_TRUE(p.hasPeak);
  TEST_ASSERT_TRUE(p.peak == 12.35);
  TEST_ASSERT_EQUAL(2, p.frames);
  TEST_ASSERT_TRUE(parseUploadQuery(&ok, nullptr, nullptr, &p).ok());
  TEST_ASSERT_FALSE(p.hasPeak);
  TEST_ASSERT_EQUAL(-1, p.frames);
  for (const char* good : {"40", "0", " 3 "}) {
    const std::string s = good;
    TEST_ASSERT_TRUE_MESSAGE(parseUploadQuery(&ok, &s, nullptr, &p).ok(), good);
  }
  for (const char* b : {"40.01", "-1", "nan", "inf", "abc", "", "0x10"}) {
    const std::string s = b;
    e = parseUploadQuery(&ok, &s, nullptr, &p);
    TEST_ASSERT_EQUAL_MESSAGE(400, e.status, b);
    TEST_ASSERT_EQUAL_STRING("invalid", e.code.c_str());
    TEST_ASSERT_EQUAL_STRING("peak_a must be a number from 0 to 40", e.message.c_str());
  }
  for (const char* f : {"x", "1234567", "-1"}) {
    const std::string s = f;
    TEST_ASSERT_TRUE(parseUploadQuery(&ok, nullptr, &s, &p).ok());
    TEST_ASSERT_EQUAL(-1, p.frames);
  }

  std::vector<uint8_t> g = gif();
  TEST_ASSERT_TRUE(checkGifHeader(g.data(), g.size()).ok());
  g = gif(64, 32);
  e = checkGifHeader(g.data(), g.size());
  TEST_ASSERT_EQUAL_STRING("GIF must be 256x96, got 64x32", e.message.c_str());
  const char* notGif = "not a gif";
  TEST_ASSERT_EQUAL_STRING("Not a GIF file", checkGifHeader(reinterpret_cast<const uint8_t*>(notGif), 9).message.c_str());
  g = gif();
  TEST_ASSERT_EQUAL(400, checkGifHeader(g.data(), 12).status);  // under 13 bytes
}

void test_storage_reserve() {
  Storage s;
  s.total = 23 * 1024 * 1024;
  TEST_ASSERT_EQUAL_UINT64(21705523, s.usable());  // int(23 MiB * 0.9)
  JsonDocument d;
  storageJson(s, d.to<JsonObject>());
  TEST_ASSERT_EQUAL_STRING("{\"used_bytes\":0,\"total_bytes\":24117248,\"usable_bytes\":21705523}",
                           dump(d.as<JsonVariantConst>()).c_str());
}

// test_instagram_tiles_served_as_bmp's path checks, after percent-decoding.
void test_tile_paths() {
  std::string gen, file;
  TEST_ASSERT_TRUE(parseTilePath("12/story-0.bmp", &gen, &file));
  TEST_ASSERT_EQUAL_STRING("12", gen.c_str());
  TEST_ASSERT_EQUAL_STRING("story-0.bmp", file.c_str());
  TEST_ASSERT_TRUE(parseTilePath("123456789/post-10.bmp", nullptr, nullptr));
  for (const char* bad : {"../settings.json", "12/../../settings.json", "12/../../settings.json", "story-0.bmp",
                          "12/story-0.gif", "12/x.bmp", "../12/story-0.bmp",
                          "../settings.json", "1234567890/story-0.bmp", "12/story-100.bmp", "12/story-.bmp",
                          "12/story-0.bmp/x", "/12/story-0.bmp", "12/story-0.bmp\n"}) {
    TEST_ASSERT_FALSE_MESSAGE(parseTilePath(bad, nullptr, nullptr), bad);
  }
  TEST_ASSERT_FALSE(parseTilePath(std::string("12/story-0.bmp\0", 15), nullptr, nullptr));
  TEST_ASSERT_EQUAL_STRING("/ig/12/story-0.565", tileRawPath("12/story-0.bmp").c_str());
  TEST_ASSERT_EQUAL_STRING("", tileRawPath("../story-0.bmp").c_str());
}

// ---- time zones and PR dates -------------------------------------------------------------

struct TzRow {
  int zone;
  long long unix;
  int offset;
  const char* date;
};
// Every DST transition in 2026 and 2027 (one hour before, one second before, at, one
// second after) plus three ordinary times, per zone, from Python's zoneinfo.
const TzRow kTzRows[] = {
    {0, 1772960399LL, -28800, "2026-03-08"},
    {0, 1772963999LL, -28800, "2026-03-08"},
    {0, 1772964000LL, -25200, "2026-03-08"},
    {0, 1772964001LL, -25200, "2026-03-08"},
    {0, 1793519999LL, -25200, "2026-11-01"},
    {0, 1793523599LL, -25200, "2026-11-01"},
    {0, 1793523600LL, -28800, "2026-11-01"},
    {0, 1793523601LL, -28800, "2026-11-01"},
    {0, 1805014799LL, -28800, "2027-03-14"},
    {0, 1805018399LL, -28800, "2027-03-14"},
    {0, 1805018400LL, -25200, "2027-03-14"},
    {0, 1805018401LL, -25200, "2027-03-14"},
    {0, 1825574399LL, -25200, "2027-11-07"},
    {0, 1825577999LL, -25200, "2027-11-07"},
    {0, 1825578000LL, -28800, "2027-11-07"},
    {0, 1825578001LL, -28800, "2027-11-07"},
    {0, 1793935800LL, -28800, "2026-11-05"},
    {0, 1767225600LL, -28800, "2025-12-31"},
    {0, 1798761599LL, -28800, "2026-12-31"},
    {1, 1772956799LL, -25200, "2026-03-08"},
    {1, 1772960399LL, -25200, "2026-03-08"},
    {1, 1772960400LL, -21600, "2026-03-08"},
    {1, 1772960401LL, -21600, "2026-03-08"},
    {1, 1793516399LL, -21600, "2026-11-01"},
    {1, 1793519999LL, -21600, "2026-11-01"},
    {1, 1793520000LL, -25200, "2026-11-01"},
    {1, 1793520001LL, -25200, "2026-11-01"},
    {1, 1805011199LL, -25200, "2027-03-14"},
    {1, 1805014799LL, -25200, "2027-03-14"},
    {1, 1805014800LL, -21600, "2027-03-14"},
    {1, 1805014801LL, -21600, "2027-03-14"},
    {1, 1825570799LL, -21600, "2027-11-07"},
    {1, 1825574399LL, -21600, "2027-11-07"},
    {1, 1825574400LL, -25200, "2027-11-07"},
    {1, 1825574401LL, -25200, "2027-11-07"},
    {1, 1793935800LL, -25200, "2026-11-05"},
    {1, 1767225600LL, -25200, "2025-12-31"},
    {1, 1798761599LL, -25200, "2026-12-31"},
    {2, 1793935800LL, -25200, "2026-11-05"},
    {2, 1767225600LL, -25200, "2025-12-31"},
    {2, 1798761599LL, -25200, "2026-12-31"},
    {3, 1772953199LL, -21600, "2026-03-08"},
    {3, 1772956799LL, -21600, "2026-03-08"},
    {3, 1772956800LL, -18000, "2026-03-08"},
    {3, 1772956801LL, -18000, "2026-03-08"},
    {3, 1793512799LL, -18000, "2026-11-01"},
    {3, 1793516399LL, -18000, "2026-11-01"},
    {3, 1793516400LL, -21600, "2026-11-01"},
    {3, 1793516401LL, -21600, "2026-11-01"},
    {3, 1805007599LL, -21600, "2027-03-14"},
    {3, 1805011199LL, -21600, "2027-03-14"},
    {3, 1805011200LL, -18000, "2027-03-14"},
    {3, 1805011201LL, -18000, "2027-03-14"},
    {3, 1825567199LL, -18000, "2027-11-07"},
    {3, 1825570799LL, -18000, "2027-11-07"},
    {3, 1825570800LL, -21600, "2027-11-07"},
    {3, 1825570801LL, -21600, "2027-11-07"},
    {3, 1793935800LL, -21600, "2026-11-05"},
    {3, 1767225600LL, -21600, "2025-12-31"},
    {3, 1798761599LL, -21600, "2026-12-31"},
    {4, 1772949599LL, -18000, "2026-03-08"},
    {4, 1772953199LL, -18000, "2026-03-08"},
    {4, 1772953200LL, -14400, "2026-03-08"},
    {4, 1772953201LL, -14400, "2026-03-08"},
    {4, 1793509199LL, -14400, "2026-11-01"},
    {4, 1793512799LL, -14400, "2026-11-01"},
    {4, 1793512800LL, -18000, "2026-11-01"},
    {4, 1793512801LL, -18000, "2026-11-01"},
    {4, 1805003999LL, -18000, "2027-03-14"},
    {4, 1805007599LL, -18000, "2027-03-14"},
    {4, 1805007600LL, -14400, "2027-03-14"},
    {4, 1805007601LL, -14400, "2027-03-14"},
    {4, 1825563599LL, -14400, "2027-11-07"},
    {4, 1825567199LL, -14400, "2027-11-07"},
    {4, 1825567200LL, -18000, "2027-11-07"},
    {4, 1825567201LL, -18000, "2027-11-07"},
    {4, 1793935800LL, -18000, "2026-11-05"},
    {4, 1767225600LL, -18000, "2025-12-31"},
    {4, 1798761599LL, -18000, "2026-12-31"},
    {5, 1772963999LL, -32400, "2026-03-08"},
    {5, 1772967599LL, -32400, "2026-03-08"},
    {5, 1772967600LL, -28800, "2026-03-08"},
    {5, 1772967601LL, -28800, "2026-03-08"},
    {5, 1793523599LL, -28800, "2026-11-01"},
    {5, 1793527199LL, -28800, "2026-11-01"},
    {5, 1793527200LL, -32400, "2026-11-01"},
    {5, 1793527201LL, -32400, "2026-11-01"},
    {5, 1805018399LL, -32400, "2027-03-14"},
    {5, 1805021999LL, -32400, "2027-03-14"},
    {5, 1805022000LL, -28800, "2027-03-14"},
    {5, 1805022001LL, -28800, "2027-03-14"},
    {5, 1825577999LL, -28800, "2027-11-07"},
    {5, 1825581599LL, -28800, "2027-11-07"},
    {5, 1825581600LL, -32400, "2027-11-07"},
    {5, 1825581601LL, -32400, "2027-11-07"},
    {5, 1793935800LL, -32400, "2026-11-05"},
    {5, 1767225600LL, -32400, "2025-12-31"},
    {5, 1798761599LL, -32400, "2026-12-31"},
    {6, 1793935800LL, -36000, "2026-11-05"},
    {6, 1767225600LL, -36000, "2025-12-31"},
    {6, 1798761599LL, -36000, "2026-12-31"},
    {7, 1793935800LL, 0, "2026-11-06"},
    {7, 1767225600LL, 0, "2026-01-01"},
    {7, 1798761599LL, 0, "2026-12-31"},
};

void test_timezones_match_zoneinfo() {
  for (const TzRow& r : kTzRows) {
    const std::string what = std::string(kZones[r.zone].iana) + " @" + std::to_string(r.unix);
    TEST_ASSERT_EQUAL_MESSAGE(r.offset, utcOffsetAt(kZones[r.zone].posix, r.unix), what.c_str());
    TEST_ASSERT_EQUAL_STRING_MESSAGE(r.date, localDate(r.zone, r.unix).c_str(), what.c_str());
  }
  TEST_ASSERT_EQUAL(0, zoneIndex("America/Los_Angeles"));
  TEST_ASSERT_EQUAL(7, zoneIndex("UTC"));
  TEST_ASSERT_EQUAL(-1, zoneIndex("Europe/London"));
}

// PrDateTest: 03:30 UTC on 7 Nov 2026 is still 6 Nov in California.
const long long kNow = 1794022200;  // 2026-11-07T03:30:00Z

void test_local_date_example() {
  TEST_ASSERT_EQUAL_STRING("2026-11-07T03:30:00Z", isoUtc(kNow).c_str());
  TEST_ASSERT_EQUAL_STRING("2026-11-06", localDate(zoneIndex("America/Los_Angeles"), kNow).c_str());
  TEST_ASSERT_EQUAL_STRING("2026-11-07", localDate(zoneIndex("UTC"), kNow).c_str());
  TEST_ASSERT_EQUAL_STRING("2026-11-06", localDate(zoneIndex("Pacific/Honolulu"), kNow).c_str());
}

// Runs one PUT /api/athletes against `stored`, as put_athletes does after the rev check.
JsonDocument save(JsonDocument& stored, const char* athletes, const std::string& today) {
  JsonDocument in = parse(athletes);
  JsonDocument out;
  JsonArray arr = out.to<JsonArray>();
  TEST_ASSERT_TRUE(validateAthletes(in.as<JsonArrayConst>(), arr).ok());
  stampPrs(arr, stored.as<JsonArrayConst>(), today);
  stored = out;
  return out;
}

const char* kAnn = "{\"id\": \"a1\", \"name\": \"Ann\", \"snatch\": 80, \"clean_jerk\": null, \"total\": null, "
                   "\"back_squat\": 150, \"front_squat\": null, \"notes\": \"\", \"visible\": true";

std::string ann(const char* extra = "") { return std::string("[") + kAnn + extra + "}]"; }

void test_stamped_in_the_wall_zone() {
  JsonDocument stored;
  stored.to<JsonArray>();
  const std::string la = localDate(zoneIndex("America/Los_Angeles"), kNow);
  JsonDocument out = save(stored, ann().c_str(), la);
  TEST_ASSERT_EQUAL_STRING("{}", dump(out[0]["pr_dates"]).c_str());  // new athletes start with none
  out = save(stored, ann(", \"snatch\": 82.5, \"clean_jerk\": 100").c_str(), la);  // up, and filled in
  TEST_ASSERT_EQUAL_STRING("{\"snatch\":\"2026-11-06\",\"clean_jerk\":\"2026-11-06\"}", dump(out[0]["pr_dates"]).c_str());
  out = save(stored, ann(", \"snatch\": 83, \"clean_jerk\": 100").c_str(), localDate(zoneIndex("UTC"), kNow));
  TEST_ASSERT_EQUAL_STRING("{\"snatch\":\"2026-11-07\",\"clean_jerk\":\"2026-11-06\"}", dump(out[0]["pr_dates"]).c_str());
}

void test_lower_or_same_keeps_dates_and_client_dates_are_ignored() {
  JsonDocument stored;
  stored.to<JsonArray>();
  const std::string today = "2026-11-06";
  save(stored, ann().c_str(), today);
  save(stored, ann(", \"snatch\": 85").c_str(), today);
  JsonDocument out = save(
      stored, ann(", \"snatch\": 70, \"back_squat\": 150, \"pr_dates\": {\"snatch\": \"2020-01-01\", \"back_squat\": \"2026-11-06\"}").c_str(),
      today);
  TEST_ASSERT_EQUAL_STRING("{\"snatch\":\"2026-11-06\"}", dump(out[0]["pr_dates"]).c_str());
  out = save(stored, ann(", \"snatch\": null").c_str(), today);
  TEST_ASSERT_EQUAL_STRING("{\"snatch\":\"2026-11-06\"}", dump(out[0]["pr_dates"]).c_str());
  const std::string two = std::string("[") + kAnn + ", \"snatch\": null}, {\"id\": \"a2\", \"name\": \"Bo\", \"snatch\": 60, "
                                                    "\"pr_dates\": {\"snatch\": \"2026-11-06\"}}]";
  out = save(stored, two.c_str(), today);
  TEST_ASSERT_EQUAL_STRING("{}", dump(out[1]["pr_dates"]).c_str());
}

// The mock's test_new_prs_slide_type: per-athlete seconds within 5..999, and file dropped.
void test_new_prs_slide_type() {
  for (const char* d : {"5", "999", "8"}) {
    const std::string body = std::string("[{\"id\": \"p1\", \"type\": \"new_prs\", \"duration_s\": ") + d + "}]";
    TEST_ASSERT_TRUE_MESSAGE(putSlides(body.c_str()).ok(), d);
  }
  for (const char* d : {"4", "1000", "8.5", "true"}) {
    const std::string body = std::string("[{\"id\": \"p1\", \"type\": \"new_prs\", \"duration_s\": ") + d + "}]";
    TEST_ASSERT_FALSE_MESSAGE(putSlides(body.c_str()).ok(), d);
  }
  JsonDocument in = parse("[{\"id\": \"p1\", \"type\": \"new_prs\", \"duration_s\": 8, \"file\": \"x.gif\", \"enabled\": false}]");
  JsonDocument out;
  TEST_ASSERT_TRUE(validatePlaylist(in.as<JsonArrayConst>(), {}, out.to<JsonArray>()).ok());
  TEST_ASSERT_EQUAL_STRING("[{\"id\":\"p1\",\"type\":\"new_prs\",\"duration_s\":8,\"enabled\":false}]",
                           dump(out.as<JsonVariantConst>()).c_str());
  TEST_ASSERT_EQUAL_STRING("Slide 1: type must be one of whiteboard, media, instagram_story, instagram_posts, new_prs, announcements",
                           putSlides("[{\"id\": \"p1\", \"type\": \"new_pr\", \"duration_s\": 8}]").message.c_str());
}

// tests/announcement_cases.json: the cases the mock's AnnouncementTest runs, so both give
// the same answer, message for message.
void test_announcement_validation_matches_the_mock() {
  std::string here = __FILE__;
  here = here.substr(0, here.find_last_of('/'));
  std::string text;
  for (const std::string& p : {std::string("../tests/announcement_cases.json"), here + "/../../../tests/announcement_cases.json"}) {
    std::ifstream f(p);
    if (!f) continue;
    std::stringstream ss;
    ss << f.rdbuf();
    text = ss.str();
    break;
  }
  TEST_ASSERT_FALSE_MESSAGE(text.empty(), "tests/announcement_cases.json not found");
  JsonDocument doc;
  TEST_ASSERT_EQUAL(0, static_cast<int>(deserializeJson(doc, text).code()));
  int n = 0;
  for (JsonObjectConst c : doc["cases"].as<JsonArrayConst>()) {
    const char* name = c["name"];
    JsonDocument out;
    const ApiError e = validateAnnouncements(c["announcements"].as<JsonArrayConst>(), out.to<JsonArray>());
    if (c["error"].is<const char*>()) {
      TEST_ASSERT_EQUAL_MESSAGE(400, e.status, name);
      TEST_ASSERT_EQUAL_STRING_MESSAGE("invalid", e.code.c_str(), name);
      TEST_ASSERT_EQUAL_STRING_MESSAGE(c["error"].as<const char*>(), e.message.c_str(), name);
    } else {
      TEST_ASSERT_TRUE_MESSAGE(e.ok(), (std::string(name) + ": " + e.message).c_str());
      TEST_ASSERT_EQUAL_STRING_MESSAGE(dump(c["saved"]).c_str(), dump(out.as<JsonVariantConst>()).c_str(), name);
    }
    n++;
  }
  TEST_ASSERT_TRUE(n >= 40);

  for (const char* d : {"2026-11-06", "2028-02-29", "2000-02-29", "0001-01-01", "9999-12-31"}) TEST_ASSERT_TRUE_MESSAGE(isCalendarDate(d), d);
  for (const char* d : {"2026-02-29", "1900-02-29", "2026-04-31", "0000-01-01", "2026-00-01", "2026-11-00", "2026-11-6", "2026/11/06", ""}) {
    TEST_ASSERT_FALSE_MESSAGE(isCalendarDate(d), d);
  }
}

// The mock's AnnouncementTest.test_slide_type: per-announcement seconds, file dropped.
void test_announcements_slide_type() {
  for (const char* d : {"5", "999", "8"}) {
    const std::string body = std::string("[{\"id\": \"n\", \"type\": \"announcements\", \"duration_s\": ") + d + "}]";
    TEST_ASSERT_TRUE_MESSAGE(putSlides(body.c_str()).ok(), d);
  }
  for (const char* d : {"4", "1000", "8.5", "true"}) {
    const std::string body = std::string("[{\"id\": \"n\", \"type\": \"announcements\", \"duration_s\": ") + d + "}]";
    TEST_ASSERT_FALSE_MESSAGE(putSlides(body.c_str()).ok(), d);
  }
  JsonDocument in = parse("[{\"id\": \"n\", \"type\": \"announcements\", \"duration_s\": 12, \"file\": \"x.gif\", \"enabled\": false}]");
  JsonDocument out;
  TEST_ASSERT_TRUE(validatePlaylist(in.as<JsonArrayConst>(), {}, out.to<JsonArray>()).ok());
  TEST_ASSERT_EQUAL_STRING("[{\"id\":\"n\",\"type\":\"announcements\",\"duration_s\":12,\"enabled\":false}]",
                           dump(out.as<JsonVariantConst>()).c_str());
  TEST_ASSERT_EQUAL_STRING("Slide 1: type must be one of whiteboard, media, instagram_story, instagram_posts, new_prs, announcements",
                           putSlides("[{\"id\": \"n\", \"type\": \"announcement\", \"duration_s\": 8}]").message.c_str());
  TEST_ASSERT_EQUAL_STRING("announcements", slideTypeName(wall::SlideType::kAnnouncements));
  TEST_ASSERT_EQUAL_STRING("new_prs", slideTypeName(wall::SlideType::kNewPrs));
}

// The mock's test_previous_value_recorded_with_the_date, step for step.
void test_previous_value_recorded_with_the_date() {
  JsonDocument stored;
  stored.to<JsonArray>();
  const std::string today = "2026-11-06";
  JsonDocument out = save(stored, ann().c_str(), today);
  TEST_ASSERT_EQUAL_STRING("{}", dump(out[0]["pr_prev"]).c_str());
  out = save(stored, ann(", \"snatch\": 82.5, \"clean_jerk\": 100").c_str(), today);  // up from 80, and filled in
  TEST_ASSERT_EQUAL_STRING("{\"snatch\":80,\"clean_jerk\":null}", dump(out[0]["pr_prev"]).c_str());
  for (const char* change : {", \"snatch\": 70", ", \"snatch\": 70, \"back_squat\": 150", ", \"snatch\": null"}) {
    out = save(stored, ann((std::string(", \"clean_jerk\": 100") + change).c_str()).c_str(), today);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("{\"snatch\":80,\"clean_jerk\":null}", dump(out[0]["pr_prev"]).c_str(), change);
  }
  out = save(stored, ann(", \"snatch\": 75, \"clean_jerk\": 100").c_str(), today);
  TEST_ASSERT_EQUAL_STRING("{\"snatch\":null,\"clean_jerk\":null}", dump(out[0]["pr_prev"]).c_str());
  out = save(stored, ann(", \"snatch\": 77.5, \"clean_jerk\": 100.5").c_str(), today);
  TEST_ASSERT_EQUAL_STRING("{\"snatch\":75,\"clean_jerk\":100}", dump(out[0]["pr_prev"]).c_str());
  // Kept next to pr_dates, in the order the file is written.
  TEST_ASSERT_EQUAL_STRING("{\"snatch\":\"2026-11-06\",\"clean_jerk\":\"2026-11-06\"}", dump(out[0]["pr_dates"]).c_str());
  std::string keys;
  for (JsonPairConst kv : out[0].as<JsonObjectConst>()) keys += std::string(kv.key().c_str()) + " ";
  TEST_ASSERT_EQUAL_STRING("id name visible snatch clean_jerk total back_squat front_squat notes pr_dates pr_prev ", keys.c_str());
}

void test_client_previous_values_are_ignored() {
  JsonDocument stored;
  stored.to<JsonArray>();
  const std::string today = "2026-11-06";
  save(stored, ann().c_str(), today);
  JsonDocument out =
      save(stored, ann(", \"snatch\": 90, \"pr_prev\": {\"snatch\": 1, \"back_squat\": 2, \"bench\": 3}").c_str(), today);
  TEST_ASSERT_EQUAL_STRING("{\"snatch\":80}", dump(out[0]["pr_prev"]).c_str());
  out = save(stored, ann(", \"snatch\": 90, \"pr_prev\": {}").c_str(), today);
  TEST_ASSERT_EQUAL_STRING("{\"snatch\":80}", dump(out[0]["pr_prev"]).c_str());
  const std::string two = std::string("[") + kAnn + ", \"snatch\": 90}, {\"id\": \"a2\", \"name\": \"Bo\", \"snatch\": 60, "
                                                    "\"pr_prev\": {\"snatch\": 50}}]";
  out = save(stored, two.c_str(), today);
  TEST_ASSERT_EQUAL_STRING("{}", dump(out[1]["pr_prev"]).c_str());
  // A file written before pr_prev existed: the first PR after the update records one.
  JsonDocument old = parse("[{\"id\": \"a1\", \"name\": \"Ann\", \"snatch\": 80, \"back_squat\": 150, \"pr_dates\": {\"snatch\": \"2026-10-01\"}}]");
  out = save(old, ann(", \"snatch\": 80").c_str(), today);
  TEST_ASSERT_EQUAL_STRING("{}", dump(out[0]["pr_prev"]).c_str());
  out = save(old, ann(", \"snatch\": 81").c_str(), today);
  TEST_ASSERT_EQUAL_STRING("{\"snatch\":80}", dump(out[0]["pr_prev"]).c_str());
}

void test_no_stamp_without_valid_time() {
  JsonDocument stored;
  stored.to<JsonArray>();
  save(stored, ann().c_str(), "");
  JsonDocument out = save(stored, ann(", \"snatch\": 90").c_str(), "");
  TEST_ASSERT_EQUAL(90, out[0]["snatch"].as<int>());
  TEST_ASSERT_EQUAL_STRING("{}", dump(out[0]["pr_dates"]).c_str());
  TEST_ASSERT_EQUAL_STRING("{}", dump(out[0]["pr_prev"]).c_str());
  save(stored, ann(", \"snatch\": 95").c_str(), "2026-11-06");
  out = save(stored, ann(", \"snatch\": 100").c_str(), "");  // kept as they were, not restamped
  TEST_ASSERT_EQUAL_STRING("{\"snatch\":\"2026-11-06\"}", dump(out[0]["pr_dates"]).c_str());
  TEST_ASSERT_EQUAL_STRING("{\"snatch\":90}", dump(out[0]["pr_prev"]).c_str());
}

// ---- status ------------------------------------------------------------------------------------

void test_recent_errors_keep_the_last_20() {
  ErrorRing ring;
  for (int i = 0; i < 25; i++) ring.add("2026-11-06T12:00:00Z", "error " + std::to_string(i));
  JsonDocument d;
  ring.toJson(d.to<JsonArray>());
  TEST_ASSERT_EQUAL(20, d.size());
  TEST_ASSERT_EQUAL_STRING("error 5", d[0]["message"].as<const char*>());
  TEST_ASSERT_EQUAL_STRING("error 24", d[19]["message"].as<const char*>());
  TEST_ASSERT_EQUAL(2, d[0].size());  // exactly {"time", "message"}
}

void test_scrub_never_leaks_the_token() {
  const std::string token = "EAAB-secret.token";
  const std::string m = scrubMessage("Bad request for https://graph.facebook.com/v23.0/me?access_token=EAAB-secret.token now (EAAB-secret.token)", token);
  TEST_ASSERT_TRUE(m.find("EAAB") == std::string::npos);
  TEST_ASSERT_TRUE(m.find("https://") == std::string::npos);
  TEST_ASSERT_EQUAL_STRING("Bad request for [link] now ([token])", m.c_str());
}

// ---- content -------------------------------------------------------------------------------

void test_content_round_trip() {
  JsonDocument settings = settingsWithPin();
  settings["sort"] = "total";
  settings["brightness"] = 70;
  JsonDocument athletes = parse(
      "{\"rev\": 3, \"athletes\": [{\"id\": \"a1\", \"name\": \"Ann\", \"visible\": true, \"snatch\": 80, \"clean_jerk\": 100.5, "
      "\"total\": null, \"back_squat\": null, \"front_squat\": 120, \"notes\": \"\", \"pr_dates\": {\"snatch\": \"2026-11-06\", "
      "\"clean_jerk\": \"2026-11-05\"}, \"pr_prev\": {\"snatch\": 77.5, \"clean_jerk\": null}}, "
      "{\"id\": \"a2\", \"name\": \"Bo\", \"visible\": false, \"snatch\": null, \"clean_jerk\": null, \"total\": null, "
      "\"back_squat\": null, \"front_squat\": null, \"notes\": \"x\", \"pr_dates\": {}}]}");
  JsonDocument playlist = parse(
      "{\"rev\": 2, \"slides\": [{\"id\": \"s0\", \"type\": \"media\", \"file\": \"logo.gif\", \"duration_s\": 10, \"enabled\": true}, "
      "{\"id\": \"s1\", \"type\": \"whiteboard\", \"duration_s\": 8, \"enabled\": false}, "
      "{\"id\": \"s2\", \"type\": \"instagram_story\", \"duration_s\": 6, \"enabled\": true}, "
      "{\"id\": \"s3\", \"type\": \"new_prs\", \"duration_s\": 9, \"enabled\": true}, "
      "{\"id\": \"s4\", \"type\": \"announcements\", \"duration_s\": 11, \"enabled\": true}]}");
  JsonDocument announcements = parse(
      "{\"rev\": 5, \"announcements\": [{\"id\": \"n1\", \"text\": \"Open gym\\nSaturday\", \"enabled\": true, "
      "\"start_date\": null, \"end_date\": null}, {\"id\": \"n2\", \"text\": \"Caf\\u00e9\", \"enabled\": false, "
      "\"start_date\": \"2026-11-01\", \"end_date\": \"2026-11-30\"}]}");
  JsonDocument media = parse("{\"logo.gif\": {\"frames\": 1, \"peak_a\": 6.42, \"uploaded\": \"2026-11-06\"}}");
  JsonDocument ig = parse(
      "{\"enabled\": true, \"username\": \"gym\", \"stories\": [{\"id\": \"1\", \"timestamp\": \"2026-11-06T18:02:11+0000\", "
      "\"file\": \"12/story-0.bmp\", \"kind\": \"video\"}], \"posts\": [{\"id\": \"2\", \"timestamp\": \"t\", "
      "\"file\": \"12/post-0.bmp\", \"kind\": \"reel\", \"like_count\": 214, \"permalink\": null}]}");
  ContentSources src;
  src.settings = settings.as<JsonObjectConst>();
  src.athletes = athletes.as<JsonObjectConst>();
  src.playlist = playlist.as<JsonObjectConst>();
  src.announcements = announcements.as<JsonObjectConst>();
  src.mediaIndex = media.as<JsonObjectConst>();
  src.mediaFiles = {"logo.gif", "other.gif"};
  src.instagram = ig.as<JsonObjectConst>();
  src.igTokenSet = true;
  wall::Content c = buildContent(src, 9);
  TEST_ASSERT_EQUAL(9, c.version);
  TEST_ASSERT_EQUAL(2, c.athletes.size());
  TEST_ASSERT_TRUE(c.athletes[0].has[wall::kCleanJerk]);
  TEST_ASSERT_EQUAL_FLOAT(100.5f, c.athletes[0].kg[wall::kCleanJerk]);
  TEST_ASSERT_FALSE(c.athletes[0].has[wall::kTotal]);
  TEST_ASSERT_EQUAL_STRING("2026-11-06", c.athletes[0].pr_date[wall::kSnatch].c_str());
  TEST_ASSERT_TRUE(c.athletes[0].has_prev[wall::kSnatch]);
  TEST_ASSERT_EQUAL_FLOAT(77.5f, c.athletes[0].prev_kg[wall::kSnatch]);
  TEST_ASSERT_FALSE(c.athletes[0].has_prev[wall::kCleanJerk]);  // null: blank before
  TEST_ASSERT_FALSE(c.athletes[0].has_prev[wall::kTotal]);      // no record
  TEST_ASSERT_FALSE(c.athletes[1].has_prev[wall::kSnatch]);     // no pr_prev at all
  TEST_ASSERT_FALSE(c.athletes[1].visible);
  TEST_ASSERT_TRUE(c.settings.sort == wall::SortMode::kTotal);
  TEST_ASSERT_EQUAL(70, c.settings.brightness);
  TEST_ASSERT_EQUAL(5, c.slides.size());
  TEST_ASSERT_TRUE(c.slides[3].type == wall::SlideType::kNewPrs);
  TEST_ASSERT_TRUE(c.slides[4].type == wall::SlideType::kAnnouncements);
  TEST_ASSERT_EQUAL(11, c.slides[4].duration_s);
  TEST_ASSERT_EQUAL(2, c.announcements.size());
  TEST_ASSERT_EQUAL_STRING("Open gym\nSaturday", c.announcements[0].text.c_str());
  TEST_ASSERT_EQUAL_STRING("", c.announcements[0].start_date.c_str());
  TEST_ASSERT_FALSE(c.announcements[1].enabled);
  TEST_ASSERT_EQUAL_STRING("2026-11-30", c.announcements[1].end_date.c_str());
  TEST_ASSERT_EQUAL(9, c.slides[3].duration_s);
  TEST_ASSERT_TRUE(c.slides[0].type == wall::SlideType::kMedia);
  TEST_ASSERT_FALSE(c.slides[1].enabled);
  TEST_ASSERT_EQUAL(2, c.media.size());
  TEST_ASSERT_EQUAL_FLOAT(6.42f, c.media[0].peak_a);
  TEST_ASSERT_TRUE(c.media[1].peak_a < 0);
  TEST_ASSERT_EQUAL_STRING("/ig/12/story-0.565", c.instagram.stories[0].tile_path.c_str());
  TEST_ASSERT_TRUE(c.instagram.stories[0].kind == wall::IgKind::kVideo);
  TEST_ASSERT_EQUAL(214, c.instagram.posts[0].like_count);

  JsonDocument back;
  athletesToJson(c.athletes, back.to<JsonArray>());
  TEST_ASSERT_EQUAL_STRING(
      "[{\"id\":\"a1\",\"name\":\"Ann\",\"visible\":true,\"snatch\":80,\"clean_jerk\":100.5,\"total\":null,\"back_squat\":null,"
      "\"front_squat\":120,\"pr_dates\":{\"snatch\":\"2026-11-06\",\"clean_jerk\":\"2026-11-05\"},\"pr_prev\":{\"snatch\":77.5}},"
      "{\"id\":\"a2\",\"name\":\"Bo\",\"visible\":false,"
      "\"snatch\":null,\"clean_jerk\":null,\"total\":null,\"back_squat\":null,\"front_squat\":null,\"pr_dates\":{},\"pr_prev\":{}}]",
      dump(back.as<JsonVariantConst>()).c_str());
  JsonDocument slides;
  slidesToJson(c.slides, slides.to<JsonArray>());
  TEST_ASSERT_EQUAL_STRING(
      "[{\"id\":\"s0\",\"type\":\"media\",\"duration_s\":10,\"enabled\":true,\"file\":\"logo.gif\"},"
      "{\"id\":\"s1\",\"type\":\"whiteboard\",\"duration_s\":8,\"enabled\":false},"
      "{\"id\":\"s2\",\"type\":\"instagram_story\",\"duration_s\":6,\"enabled\":true},"
      "{\"id\":\"s3\",\"type\":\"new_prs\",\"duration_s\":9,\"enabled\":true},"
      "{\"id\":\"s4\",\"type\":\"announcements\",\"duration_s\":11,\"enabled\":true}]",
      dump(slides.as<JsonVariantConst>()).c_str());
  JsonDocument anns;
  announcementsToJson(c.announcements, anns.to<JsonArray>());
  TEST_ASSERT_EQUAL_STRING(dump(announcements["announcements"]).c_str(), dump(anns.as<JsonVariantConst>()).c_str());

  TEST_ASSERT_EQUAL(2, c.settings.power.supplies);  // no power key: today's two 18 A supplies
  TEST_ASSERT_TRUE(c.settings.power.rated_a == 18);
  TEST_ASSERT_EQUAL(85, c.settings.power.budget_pct);
  settings["power"] = parse("{\"supplies\": 1, \"rated_a\": 80.5, \"budget_pct\": 90}").as<JsonObjectConst>();
  src.settings = settings.as<JsonObjectConst>();
  c = buildContent(src, 9);
  TEST_ASSERT_EQUAL(1, c.settings.power.supplies);
  TEST_ASSERT_TRUE(c.settings.power.rated_a == 80.5);
  TEST_ASSERT_EQUAL(90, c.settings.power.budget_pct);
  settings["power"]["supplies"] = 3;  // a hand-edited file the API wouldn't accept: the default
  src.settings = settings.as<JsonObjectConst>();
  TEST_ASSERT_EQUAL(2, buildContent(src, 9).settings.power.supplies);

  src.igTokenSet = false;  // device: no token, no tiles (API.md section 12)
  c = buildContent(src, 10);
  TEST_ASSERT_EQUAL(0, c.instagram.stories.size());
  TEST_ASSERT_EQUAL(0, c.instagram.posts.size());
}

// ---- images --------------------------------------------------------------------------------

void test_cover_crop_and_bmp() {
  TEST_ASSERT_EQUAL(1, jpegScaleFor(100, 100, 78, 78));
  TEST_ASSERT_EQUAL(8, jpegScaleFor(1080, 1920, 54, 96));
  TEST_ASSERT_EQUAL(8, jpegScaleFor(640, 640, 78, 78));
  TEST_ASSERT_EQUAL(4, jpegScaleFor(600, 600, 78, 78));
  TEST_ASSERT_EQUAL(2, jpegScaleFor(120, 160, 54, 78));

  // 120 wide x 160 high: left half red, right half blue. A 54x96 story keeps the center,
  // so both colors show; a 78x78 post crops the top and bottom and keeps full width.
  const int w = 120, h = 160;
  std::vector<uint16_t> src(w * h);
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) src[y * w + x] = x < w / 2 ? 0xF800 : 0x001F;
  }
  std::vector<uint8_t> tile(54 * 96 * 3);
  coverCrop565(src.data(), w, h, w, 54, 96, tile.data());
  TEST_ASSERT_EQUAL(255, tile[0]);           // left edge red
  TEST_ASSERT_EQUAL(0, tile[2]);
  TEST_ASSERT_EQUAL(255, tile[53 * 3 + 2]);  // right edge blue
  // A 120x160 source is taller than 54:96 (0.5625 vs 0.75): the crop keeps the full
  // height and 90 of the 120 columns, centered.
  std::vector<uint8_t> post(78 * 78 * 3);
  coverCrop565(src.data(), w, h, w, 78, 78, post.data());
  TEST_ASSERT_EQUAL(255, post[(39 * 78 + 0) * 3]);
  TEST_ASSERT_EQUAL(255, post[(39 * 78 + 77) * 3 + 2]);

  std::vector<uint8_t> bmp = toBmp24(tile.data(), 54, 96);
  TEST_ASSERT_EQUAL('B', bmp[0]);
  TEST_ASSERT_EQUAL('M', bmp[1]);
  TEST_ASSERT_EQUAL(54, bmp[18] | bmp[19] << 8);
  TEST_ASSERT_EQUAL(96, bmp[22] | bmp[23] << 8);
  TEST_ASSERT_EQUAL(54 + 164 * 96, bmp.size());  // rows padded to 4 bytes
  // Bottom-up: the first stored row is the image's last row; pixel 0 is red, as BGR.
  TEST_ASSERT_EQUAL(0, bmp[54]);
  TEST_ASSERT_EQUAL(255, bmp[56]);
  std::vector<uint8_t> raw = toRgb565(tile.data(), 54, 96);
  TEST_ASSERT_EQUAL(54 * 96 * 2, raw.size());
  TEST_ASSERT_EQUAL(0x00, raw[0]);
  TEST_ASSERT_EQUAL(0xF8, raw[1]);  // red, little-endian
}

// ---- Instagram planning (FakeInstagram's answers) -----------------------------------------------

void test_instagram_plan() {
  JsonDocument stories = parse(
      "{\"data\": [{\"id\": \"st1\", \"media_type\": \"IMAGE\", \"media_url\": \"https://cdn.example/story1.jpg\", \"timestamp\": \"2026-11-06T11:00:00+0000\"},"
      "{\"id\": \"st0\", \"media_type\": \"IMAGE\", \"media_url\": \"https://cdn.example/story0.jpg\", \"timestamp\": \"2026-11-06T10:00:00+0000\"},"
      "{\"id\": \"stv\", \"media_type\": \"VIDEO\", \"media_url\": \"https://cdn.example/v.mp4\", \"timestamp\": \"2026-11-06T09:00:00+0000\"}]}");
  std::vector<IgDownload> s;
  TEST_ASSERT_TRUE(planStories(stories.as<JsonVariantConst>(), &s));
  TEST_ASSERT_EQUAL(2, s.size());  // the video has no thumbnail: skipped
  TEST_ASSERT_EQUAL_STRING("st0", s[0].id.c_str());  // oldest first
  TEST_ASSERT_EQUAL_STRING("image", s[0].kind.c_str());

  JsonDocument posts = parse(
      "{\"data\": [{\"id\": \"p0\", \"media_type\": \"VIDEO\", \"media_product_type\": \"REELS\", \"thumbnail_url\": \"https://cdn.example/post0.jpg\", "
      "\"timestamp\": \"2026-11-05T10:00:00+0000\", \"like_count\": 5, \"permalink\": \"https://instagram.com/p/x\"},"
      "{\"id\": \"p1\", \"media_type\": \"IMAGE\", \"media_url\": \"https://cdn.example/post1.jpg\", \"timestamp\": \"2026-11-04T10:00:00+0000\", "
      "\"like_count\": 3, \"permalink\": null},"
      "{\"id\": \"p2\", \"media_type\": \"CAROUSEL_ALBUM\", \"media_url\": \"https://cdn.example/post2.jpg\"}]}");
  std::vector<IgDownload> p;
  TEST_ASSERT_TRUE(planPosts(posts.as<JsonVariantConst>(), &p));
  TEST_ASSERT_EQUAL(2, p.size());
  TEST_ASSERT_EQUAL_STRING("reel", p[0].kind.c_str());
  TEST_ASSERT_EQUAL_STRING("https://cdn.example/post0.jpg", p[0].url.c_str());
  TEST_ASSERT_EQUAL_STRING("image", p[1].kind.c_str());
  TEST_ASSERT_FALSE(p[1].hasPermalink);
  TEST_ASSERT_TRUE(p[1].hasLikes);

  JsonDocument accounts = parse(
      "{\"data\": [{\"name\": \"The Strength Tank\", \"instagram_business_account\": {\"id\": \"17841400000000001\", \"username\": \"thestrengthtank\"}},"
      "{\"name\": \"Unlinked page\"}]}");
  JsonDocument out;
  TEST_ASSERT_TRUE(parseAccounts(accounts.as<JsonVariantConst>(), false, out.to<JsonArray>()));
  TEST_ASSERT_EQUAL_STRING("[{\"page\":\"The Strength Tank\",\"id\":\"17841400000000001\",\"username\":\"thestrengthtank\"}]",
                           dump(out.as<JsonVariantConst>()).c_str());

  const char* err = "{\"error\": {\"message\": \"Invalid OAuth access token.\", \"code\": 190}}";
  TEST_ASSERT_EQUAL_STRING("Instagram said: Invalid OAuth access token.", graphHttpError(400, err, strlen(err)).c_str());
  TEST_ASSERT_EQUAL_STRING("Instagram said: HTTP 502", graphHttpError(502, "<html>", 6).c_str());

  const std::string url = graphUrl(storiesPath("17841400000000001"), "EAAB|x");
  TEST_ASSERT_EQUAL_STRING(
      "https://graph.facebook.com/v23.0/17841400000000001/stories?fields=id,media_type,media_product_type,media_url,thumbnail_url,timestamp"
      "&access_token=EAAB%7Cx",
      url.c_str());
}

void test_text_helpers() {
  TEST_ASSERT_EQUAL_STRING("x", pyStrip(" \t\xc2\xa0x\xe3\x80\x80\n").c_str());
  TEST_ASSERT_EQUAL_STRING("\"it's\"", pyRepr(parse("[\"it's\"]")[0]).c_str());
  TEST_ASSERT_EQUAL_STRING("3", pyRepr(parse("[3]")[0]).c_str());
  TEST_ASSERT_EQUAL_STRING("2.5", pyRepr(parse("[2.5]")[0]).c_str());
  TEST_ASSERT_TRUE(isPin("123456"));
  TEST_ASSERT_FALSE(isPin("\xd9\xa1\xd9\xa2\xd9\xa3\xd9\xa4\xd9\xa5\xd9\xa6"));  // Arabic-Indic digits
  TEST_ASSERT_TRUE(isJsonInt(parse("[3]")[0]));
  TEST_ASSERT_FALSE(isJsonInt(parse("[3.0]")[0]));
  TEST_ASSERT_FALSE(isJsonInt(parse("[true]")[0]));
  TEST_ASSERT_FALSE(isJsonNumber(parse("[false]")[0]));
  TEST_ASSERT_TRUE(isJsonNumber(parse("[1e2]")[0]));
}


// ---- display schedule and power (API.md 7.3, 7.4) -------------------------------------------------
// The same cases as ScheduleTest in tests/test_mock_server.py, with the instants worked out
// by Python's zoneinfo.

namespace {

const char* const kWeekdays = "[{\"start\": \"05:00\", \"end\": \"21:00\"}]";

// Mon to Fri 05:00-21:00, Saturday two ranges (given out of order), Sunday as `sun`.
std::string week(const char* sun = "[]", const char* sat = nullptr) {
  std::string d = std::string("{\"enabled\": true, \"days\": {");
  for (const char* k : {"mon", "tue", "wed", "thu", "fri"}) d += std::string("\"") + k + "\": " + kWeekdays + ", ";
  d += std::string("\"sat\": ") +
       (sat ? sat : "[{\"start\": \"16:00\", \"end\": \"18:00\"}, {\"start\": \"08:00\", \"end\": \"12:00\"}]");
  return d + ", \"sun\": " + sun + "}}";
}

Schedule sched(const std::string& json) {
  JsonDocument d = parse(json.c_str());
  JsonDocument clean;
  TEST_ASSERT_TRUE_MESSAGE(validateSchedule(d.as<JsonVariantConst>(), clean.to<JsonObject>()).ok(), json.c_str());
  return scheduleFromJson(clean.as<JsonVariantConst>());
}

const int kLA = 0;

// Runs evaluateSchedule and renders what /api/status would say.
std::string stateAt(const Schedule& s, int64_t now, Override* ov, bool clock = true, int zone = kLA) {
  JsonDocument out;
  scheduleStatusJson(evaluateSchedule(s, zone, clock, now, ov), zone, out.to<JsonObject>());
  return dump(out.as<JsonVariantConst>());
}

std::string st(bool on, const char* state, const char* next) {
  return std::string("{\"display_on\":") + (on ? "true" : "false") + ",\"schedule_state\":\"" + state +
         "\",\"next_change\":" + (next ? std::string("\"") + next + "\"" : "null") + "}";
}

}  // namespace

void test_schedule_validation() {
  struct Case {
    std::string body;
    const char* message;
    const char* day;
  };
  auto day = [](const char* key, const char* windows) {
    JsonDocument d = parse(week().c_str());
    d["days"][key] = parse(windows).as<JsonVariantConst>();
    return dump(d.as<JsonVariantConst>());
  };
  const char* fmt = "times must be HH:MM, 00:00 to 23:59 (an end can be 24:00)";
  const std::string fmtThu = std::string("Thursday: ") + fmt;
  const Case cases[] = {
      {"\"x\"", "Schedule must be an object with enabled and days", ""},
      {"{\"enabled\": 1, \"days\": {}}", "Schedule enabled must be true or false", ""},
      {"{\"enabled\": true}", "Schedule days must list mon, tue, wed, thu, fri, sat and sun", ""},
      {"{\"enabled\": false, \"days\": {\"mon\": []}}", "Schedule days must list mon, tue, wed, thu, fri, sat and sun", ""},
      {day("tue", "[{\"start\": \"05:00\", \"end\": \"06:00\"}, {\"start\": \"07:00\", \"end\": \"08:00\"}, "
                  "{\"start\": \"09:00\", \"end\": \"10:00\"}]"),
       "Tuesday: give a list of 0 to 2 time ranges", "tue"},
      {day("wed", "{\"start\": \"05:00\", \"end\": \"21:00\"}"), "Wednesday: give a list of 0 to 2 time ranges", "wed"},
      {day("thu", "[{\"start\": \"5:00\", \"end\": \"21:00\"}]"), fmtThu.c_str(), "thu"},
      {day("thu", "[{\"start\": \"24:00\", \"end\": \"24:00\"}]"), fmtThu.c_str(), "thu"},
      {day("thu", "[{\"start\": \"05:00\", \"end\": \"24:01\"}]"), fmtThu.c_str(), "thu"},
      {day("thu", "[{\"start\": \"05:00\"}]"), fmtThu.c_str(), "thu"},
      {day("thu", "[\"05:00-21:00\"]"), fmtThu.c_str(), "thu"},
      {day("thu", "[{\"start\": \"05:00\\n\", \"end\": \"21:00\"}]"), fmtThu.c_str(), "thu"},
      {day("thu", "[{\"start\": \"05:60\", \"end\": \"21:00\"}]"), fmtThu.c_str(), "thu"},
      {day("fri", "[{\"start\": \"09:00\", \"end\": \"09:00\"}]"), "Friday: a time range must end after it starts", "fri"},
      {day("fri", "[{\"start\": \"21:00\", \"end\": \"05:00\"}]"), "Friday: a time range must end after it starts", "fri"},
      {day("sun", "[{\"start\": \"05:00\", \"end\": \"12:00\"}, {\"start\": \"11:59\", \"end\": \"13:00\"}]"),
       "Sunday: the two time ranges overlap", "sun"},
  };
  for (const Case& c : cases) {
    JsonDocument b = parse(c.body.c_str());
    JsonDocument out;
    const ApiError e = validateSchedule(b.as<JsonVariantConst>(), out.to<JsonObject>());
    TEST_ASSERT_EQUAL_MESSAGE(400, e.status, c.body.c_str());
    TEST_ASSERT_EQUAL_STRING("invalid", e.code.c_str());
    TEST_ASSERT_EQUAL_STRING_MESSAGE(c.message, e.message.c_str(), c.body.c_str());
    TEST_ASSERT_EQUAL_STRING(c.day, e.day.c_str());
    JsonDocument body;
    e.toJson(body.to<JsonObject>());
    TEST_ASSERT_EQUAL(*c.day ? 3 : 2, body.size());
  }
  // Through PUT /api/settings: nothing is written on a bad schedule.
  JsonDocument s = settingsWithPin();
  const std::string before = dump(s.as<JsonVariantConst>());
  TEST_ASSERT_EQUAL(400, putSettings(s, ("{\"title\": \"NEW\", \"schedule\": " + day("mon", "[1]") + "}").c_str()).status);
  TEST_ASSERT_EQUAL_STRING(before.c_str(), dump(s.as<JsonVariantConst>()).c_str());
  // Touching ranges are fine, 24:00 ends a day, ranges come back sorted, extra keys go.
  const std::string ok = day("sun", "[{\"start\": \"12:00\", \"end\": \"24:00\", \"note\": \"x\"}, {\"start\": \"00:00\", \"end\": \"12:00\"}]");
  TEST_ASSERT_TRUE(putSettings(s, ("{\"schedule\": " + ok + "}").c_str()).ok());
  TEST_ASSERT_EQUAL_STRING("[{\"start\":\"00:00\",\"end\":\"12:00\"},{\"start\":\"12:00\",\"end\":\"24:00\"}]",
                           dump(s["schedule"]["days"]["sun"]).c_str());
  TEST_ASSERT_EQUAL_STRING("[{\"start\":\"08:00\",\"end\":\"12:00\"},{\"start\":\"16:00\",\"end\":\"18:00\"}]",
                           dump(s["schedule"]["days"]["sat"]).c_str());
  TEST_ASSERT_EQUAL_STRING("{\"enabled\":true,\"days\":{", dump(s["schedule"]).substr(0, 24).c_str());
  // A damaged stored schedule reads as off, never as a dark wall.
  JsonDocument broken = parse("{\"enabled\": true, \"days\": {\"mon\": []}}");
  TEST_ASSERT_FALSE(scheduleFromJson(broken.as<JsonVariantConst>()).enabled);
}

void test_power_setting() {
  JsonDocument s = settingsWithPin();
  const struct {
    const char* body;
    const char* message;
  } cases[] = {
      {"\"x\"", "Power must be an object with supplies, rated_a and budget_pct"},
      {"{\"supplies\": 3, \"rated_a\": 18, \"budget_pct\": 85}", "Power supplies must be 1 or 2"},
      {"{\"supplies\": 1.0, \"rated_a\": 18, \"budget_pct\": 85}", "Power supplies must be 1 or 2"},
      {"{\"supplies\": true, \"rated_a\": 18, \"budget_pct\": 85}", "Power supplies must be 1 or 2"},
      {"{\"supplies\": 1, \"rated_a\": 4.9, \"budget_pct\": 85}", "Supply rating must be 5 to 100 A"},
      {"{\"supplies\": 1, \"rated_a\": 100.5, \"budget_pct\": 85}", "Supply rating must be 5 to 100 A"},
      {"{\"supplies\": 1, \"rated_a\": \"80\", \"budget_pct\": 85}", "Supply rating must be 5 to 100 A"},
      {"{\"supplies\": 1, \"rated_a\": true, \"budget_pct\": 85}", "Supply rating must be 5 to 100 A"},
      {"{\"supplies\": 1, \"budget_pct\": 85}", "Supply rating must be 5 to 100 A"},
      {"{\"supplies\": 1, \"rated_a\": 80, \"budget_pct\": 96}", "Power budget must be 50 to 95 percent"},
      {"{\"supplies\": 1, \"rated_a\": 80, \"budget_pct\": 49}", "Power budget must be 50 to 95 percent"},
      {"{\"supplies\": 1, \"rated_a\": 80, \"budget_pct\": 85.0}", "Power budget must be 50 to 95 percent"},
  };
  for (const auto& c : cases) {
    const ApiError e = putSettings(s, (std::string("{\"power\": ") + c.body + "}").c_str());
    TEST_ASSERT_EQUAL_MESSAGE(400, e.status, c.body);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(c.message, e.message.c_str(), c.body);
  }
  TEST_ASSERT_EQUAL_STRING("{\"supplies\":2,\"rated_a\":18,\"budget_pct\":85}", dump(s["power"]).c_str());
  TEST_ASSERT_TRUE(putSettings(s, "{\"power\": {\"supplies\": 1, \"rated_a\": 80, \"budget_pct\": 85, \"x\": 1}}").ok());
  TEST_ASSERT_EQUAL_STRING("{\"supplies\":1,\"rated_a\":80,\"budget_pct\":85}", dump(s["power"]).c_str());
  TEST_ASSERT_TRUE(putSettings(s, "{\"power\": {\"supplies\": 2, \"rated_a\": 30.5, \"budget_pct\": 50}}").ok());
  TEST_ASSERT_EQUAL_STRING("{\"supplies\":2,\"rated_a\":30.5,\"budget_pct\":50}", dump(s["power"]).c_str());
}

void test_local_iso_and_offset_changes() {
  TEST_ASSERT_EQUAL_STRING("2026-11-06T21:00:00-08:00", localIso(kLA, 1794027600).c_str());
  TEST_ASSERT_EQUAL_STRING("2026-11-07T05:00:00+00:00", localIso(zoneIndex("UTC"), 1794027600).c_str());
  TEST_ASSERT_EQUAL_STRING("2026-11-07T00:00:00-05:00", localIso(zoneIndex("America/New_York"), 1794027600).c_str());
  TEST_ASSERT_EQUAL_STRING("2026-03-08T03:00:00-07:00", localIso(kLA, 1772964000).c_str());
  const char* la = kZones[kLA].posix;
  TEST_ASSERT_EQUAL(1772964000LL, nextOffsetChange(la, 1772960400));  // spring forward, 10:00 UTC
  TEST_ASSERT_EQUAL(1793523600LL, nextOffsetChange(la, 1772964000));  // the switch itself is not "after"
  TEST_ASSERT_EQUAL(1805018400LL, nextOffsetChange(la, 1794024000));  // November: next March
  TEST_ASSERT_EQUAL(-1LL, nextOffsetChange(kZones[zoneIndex("America/Phoenix")].posix, 1794024000));
  TEST_ASSERT_EQUAL(-1LL, nextOffsetChange(kZones[zoneIndex("UTC")].posix, 1794024000));
}

void test_schedule_states_through_a_week() {
  const Schedule s = sched(week());
  Override ov;
  const struct {
    int64_t now;
    std::string want;
  } cases[] = {
      {1794024000, st(true, "on", "2026-11-06T21:00:00-08:00")},     // Fri 20:00
      {1794027600, st(false, "off", "2026-11-07T08:00:00-08:00")},   // Fri 21:00
      {1794085200, st(false, "off", "2026-11-07T16:00:00-08:00")},   // Sat 13:00
      {1794103199, st(true, "on", "2026-11-07T18:00:00-08:00")},     // Sat 17:59:59
      {1794106800, st(false, "off", "2026-11-09T05:00:00-08:00")},   // Sat 19:00; Sunday is off
      {1794229140, st(false, "off", "2026-11-09T05:00:00-08:00")},   // Mon 04:59
  };
  for (const auto& c : cases) TEST_ASSERT_EQUAL_STRING(c.want.c_str(), stateAt(s, c.now, &ov).c_str());
  TEST_ASSERT_EQUAL_STRING(st(true, "on", "2026-11-09T21:00:00-05:00").c_str(),
                           stateAt(s, 1794229140, &ov, true, zoneIndex("America/New_York")).c_str());
  // Disabled, no clock, never changing.
  Schedule off = s;
  off.enabled = false;
  TEST_ASSERT_EQUAL_STRING(st(true, "always_on", nullptr).c_str(), stateAt(off, 1794024000, &ov).c_str());
  const Schedule none = sched("{\"enabled\": true, \"days\": {\"mon\": [], \"tue\": [], \"wed\": [], \"thu\": [], "
                              "\"fri\": [], \"sat\": [], \"sun\": []}}");
  TEST_ASSERT_EQUAL_STRING(st(false, "off", nullptr).c_str(), stateAt(none, 1794024000, &ov).c_str());
  TEST_ASSERT_EQUAL_STRING(st(true, "no_clock", nullptr).c_str(), stateAt(none, 1794024000, &ov, false).c_str());
}

void test_schedule_overrides() {
  const Schedule s = sched(week());
  // wake at Fri 22:00: on by hand until the schedule turns on at 08:00, which keeps it on to 12:00.
  Override ov = overrideFor(s, kLA, true, 1794031200, true);
  TEST_ASSERT_TRUE(ov.active && ov.on);
  TEST_ASSERT_EQUAL(1794067200LL, ov.until);
  TEST_ASSERT_EQUAL_STRING(st(true, "override_on", "2026-11-07T12:00:00-08:00").c_str(), stateAt(s, 1794031200, &ov).c_str());
  TEST_ASSERT_EQUAL_STRING(st(true, "override_on", "2026-11-07T12:00:00-08:00").c_str(), stateAt(s, 1794067199, &ov).c_str());
  TEST_ASSERT_EQUAL_STRING(st(true, "on", "2026-11-07T12:00:00-08:00").c_str(), stateAt(s, 1794067200, &ov).c_str());
  TEST_ASSERT_FALSE(ov.active);  // dropped once its time came
  TEST_ASSERT_EQUAL_STRING(st(false, "off", "2026-11-07T16:00:00-08:00").c_str(), stateAt(s, 1794081600, &ov).c_str());

  // sleep at Fri 20:00 lasts to 21:00, when the schedule takes over and keeps it off.
  ov = overrideFor(s, kLA, true, 1794024000, false);
  TEST_ASSERT_EQUAL_STRING(st(false, "override_off", "2026-11-07T08:00:00-08:00").c_str(), stateAt(s, 1794024000, &ov).c_str());
  TEST_ASSERT_EQUAL_STRING(st(false, "off", "2026-11-07T08:00:00-08:00").c_str(), stateAt(s, 1794027600, &ov).c_str());
  TEST_ASSERT_EQUAL_STRING(st(true, "on", "2026-11-07T12:00:00-08:00").c_str(), stateAt(s, 1794067200, &ov).c_str());

  // wake while already on: nothing to override.
  TEST_ASSERT_FALSE(overrideFor(s, kLA, true, 1794024000, true).active);
  TEST_ASSERT_FALSE(overrideFor(s, kLA, true, 1794031200, false).active);

  // No schedule in force: sleep holds until a wake, however long.
  Schedule off = s;
  off.enabled = false;
  ov = overrideFor(off, kLA, true, 1794024000, false);
  TEST_ASSERT_TRUE(ov.active && ov.until < 0);
  TEST_ASSERT_EQUAL_STRING(st(false, "override_off", nullptr).c_str(), stateAt(off, 1794024000 + 30 * 86400LL, &ov).c_str());
  TEST_ASSERT_FALSE(overrideFor(off, kLA, true, 1794024000, true).active);
  ov = overrideFor(s, kLA, false, 1794024000, false);  // no clock
  TEST_ASSERT_EQUAL_STRING(st(false, "override_off", nullptr).c_str(), stateAt(s, 1794024000, &ov, false).c_str());
}

void test_schedule_daylight_saving_and_midnight() {
  // Spring forward, Sunday 8 March 2026: 02:00 PST jumps to 03:00 PDT, so a range starting
  // at 02:30 begins at the jump.
  Override ov;
  Schedule s = sched(week("[{\"start\": \"02:30\", \"end\": \"05:00\"}]"));
  TEST_ASSERT_EQUAL_STRING(st(false, "off", "2026-03-08T03:00:00-07:00").c_str(), stateAt(s, 1772960400, &ov).c_str());
  TEST_ASSERT_EQUAL_STRING(st(true, "on", "2026-03-08T05:00:00-07:00").c_str(), stateAt(s, 1772964000, &ov).c_str());
  // A range that lies wholly inside the skipped hour never happens.
  s = sched(week("[{\"start\": \"02:10\", \"end\": \"02:50\"}]"));
  TEST_ASSERT_EQUAL_STRING(st(false, "off", "2026-03-09T05:00:00-07:00").c_str(), stateAt(s, 1772960400, &ov).c_str());
  // Fall back, Sunday 1 November 2026: 01:30 to 01:45 happens twice.
  s = sched(week("[{\"start\": \"01:30\", \"end\": \"01:45\"}]"));
  TEST_ASSERT_EQUAL_STRING(st(false, "off", "2026-11-01T01:30:00-07:00").c_str(), stateAt(s, 1793516400, &ov).c_str());
  TEST_ASSERT_EQUAL_STRING(st(true, "on", "2026-11-01T01:45:00-07:00").c_str(), stateAt(s, 1793516400 + 5400, &ov).c_str());
  TEST_ASSERT_EQUAL_STRING(st(false, "off", "2026-11-01T01:30:00-08:00").c_str(), stateAt(s, 1793523000, &ov).c_str());
  // A range to 24:00 and one from 00:00 the next day are one stretch: no change at midnight.
  s = sched(week("[{\"start\": \"00:00\", \"end\": \"02:00\"}]", "[{\"start\": \"20:00\", \"end\": \"24:00\"}]"));
  TEST_ASSERT_EQUAL_STRING(st(true, "on", "2026-11-08T02:00:00-08:00").c_str(), stateAt(s, 1794121200, &ov).c_str());
}

namespace {

// The mock's next_flip: check every minute. Exact here because boundaries are whole local
// minutes and the eight zones change offset on whole UTC hours.
int64_t scanFlip(const Schedule& s, const char* posix, int64_t from, bool value) {
  if (scheduledOn(s, posix, from) != value) return from;
  for (int64_t t = (from >= 0 ? from / 60 : -((-from + 59) / 60)) * 60 + 60; t <= from + kScheduleHorizonS; t += 60) {
    if (scheduledOn(s, posix, t) != value) return t;
  }
  return -1;
}

}  // namespace

void test_next_flip_matches_a_minute_scan() {
  // Random schedules, with boundaries drawn mostly from the hours daylight saving moves,
  // started from instants around both 2026 switches and an ordinary week.
  uint32_t seed = 12345;
  auto rnd = [&seed](uint32_t n) {
    seed = seed * 1103515245u + 12345u;
    return (seed >> 8) % n;
  };
  const int16_t picks[] = {0, 60, 90, 105, 120, 135, 150, 179, 180, 181, 1439, 1440};
  const int zones[] = {kLA, zoneIndex("America/New_York"), zoneIndex("America/Phoenix"), zoneIndex("UTC")};
  const int64_t starts[] = {1772920800, 1772960400, 1772962200, 1772964000, 1793480400, 1793516400, 1793521800,
                            1793523000, 1793527200, 1794024000};
  int compared = 0;
  for (int round = 0; round < 60; round++) {
    Schedule s;
    s.enabled = true;
    for (DayWindows& d : s.days) {
      d.count = static_cast<uint8_t>(rnd(3));
      int16_t m[4];
      for (int16_t& v : m) v = rnd(2) ? picks[rnd(12)] : static_cast<int16_t>(rnd(1441));
      std::sort(m, m + 4);
      d.start[0] = m[0], d.end[0] = m[1], d.start[1] = m[2], d.end[1] = m[3];
      for (int k = 0; k < d.count; k++) {
        if (d.end[k] <= d.start[k] || d.start[k] >= 1440) d.count = static_cast<uint8_t>(k);  // keep it valid
      }
    }
    const int zone = zones[round % 4];
    const char* posix = kZones[zone].posix;
    for (const int64_t from : starts) {
      const int64_t t = from + static_cast<int64_t>(rnd(7200)) - 3600;
      for (const bool value : {scheduledOn(s, posix, t), !scheduledOn(s, posix, t)}) {
        const int64_t want = scanFlip(s, posix, t, value), got = nextFlip(s, posix, t, value);
        if (want != got) {
          const std::string what = std::string(kZones[zone].iana) + " from " + std::to_string(t) + ": scan " +
                                   std::to_string(want) + ", nextFlip " + std::to_string(got);
          TEST_FAIL_MESSAGE(what.c_str());
        }
        compared++;
      }
    }
  }
  TEST_ASSERT_EQUAL(60 * 10 * 2, compared);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_sha256_vectors);
  RUN_TEST(test_pbkdf2_matches_python);
  RUN_TEST(test_pin_record_and_match);
  RUN_TEST(test_every_write_needs_a_pin_first);
  RUN_TEST(test_pin_guards_writes);
  RUN_TEST(test_client_lock_doubles_and_resets);
  RUN_TEST(test_missing_pin_is_not_a_wrong_pin);
  RUN_TEST(test_long_pin_header_is_wrong_without_hashing);
  RUN_TEST(test_global_lock);
  RUN_TEST(test_client_table_is_bounded);
  RUN_TEST(test_guard_schedule);
  RUN_TEST(test_rev_check);
  RUN_TEST(test_shape_errors);
  RUN_TEST(test_body_parsing);
  RUN_TEST(test_weights_half_kilo);
  RUN_TEST(test_duplicate_ids_rejected);
  RUN_TEST(test_athlete_cleaning);
  RUN_TEST(test_screen_time_limits);
  RUN_TEST(test_slide_types_and_media);
  RUN_TEST(test_settings_rules);
  RUN_TEST(test_settings_defaults_fill);
  RUN_TEST(test_instagram_settings);
  RUN_TEST(test_control);
  RUN_TEST(test_schedule_validation);
  RUN_TEST(test_power_setting);
  RUN_TEST(test_local_iso_and_offset_changes);
  RUN_TEST(test_schedule_states_through_a_week);
  RUN_TEST(test_schedule_overrides);
  RUN_TEST(test_schedule_daylight_saving_and_midnight);
  RUN_TEST(test_next_flip_matches_a_minute_scan);
  RUN_TEST(test_upload_checks);
  RUN_TEST(test_storage_reserve);
  RUN_TEST(test_tile_paths);
  RUN_TEST(test_timezones_match_zoneinfo);
  RUN_TEST(test_local_date_example);
  RUN_TEST(test_stamped_in_the_wall_zone);
  RUN_TEST(test_lower_or_same_keeps_dates_and_client_dates_are_ignored);
  RUN_TEST(test_no_stamp_without_valid_time);
  RUN_TEST(test_previous_value_recorded_with_the_date);
  RUN_TEST(test_client_previous_values_are_ignored);
  RUN_TEST(test_new_prs_slide_type);
  RUN_TEST(test_announcement_validation_matches_the_mock);
  RUN_TEST(test_announcements_slide_type);
  RUN_TEST(test_recent_errors_keep_the_last_20);
  RUN_TEST(test_scrub_never_leaks_the_token);
  RUN_TEST(test_content_round_trip);
  RUN_TEST(test_cover_crop_and_bmp);
  RUN_TEST(test_instagram_plan);
  RUN_TEST(test_text_helpers);
  return UNITY_END();
}
