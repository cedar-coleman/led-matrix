#include "nc_text.h"

#include <stdio.h>
#include <stdlib.h>

namespace netcore {

namespace {

// Decodes one code point; returns bytes consumed, 0 on malformed input.
size_t decode(const unsigned char* s, size_t n, uint32_t* cp) {
  if (n == 0) return 0;
  const unsigned char c = s[0];
  if (c < 0x80) {
    *cp = c;
    return 1;
  }
  size_t len;
  uint32_t v, min;
  if ((c & 0xE0) == 0xC0) {
    len = 2, v = c & 0x1F, min = 0x80;
  } else if ((c & 0xF0) == 0xE0) {
    len = 3, v = c & 0x0F, min = 0x800;
  } else if ((c & 0xF8) == 0xF0) {
    len = 4, v = c & 0x07, min = 0x10000;
  } else {
    return 0;
  }
  if (n < len) return 0;
  for (size_t i = 1; i < len; i++) {
    if ((s[i] & 0xC0) != 0x80) return 0;
    v = (v << 6) | (s[i] & 0x3F);
  }
  // Overlong forms, surrogates and values past U+10FFFF are what Python's strict decoder rejects.
  if (v < min || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) return 0;
  *cp = v;
  return len;
}

// The code points Python's str.isspace() accepts.
bool pySpace(uint32_t c) {
  return (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x20) || c == 0x85 || c == 0xA0 || c == 0x1680 ||
         (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

bool digit(char c) { return c >= '0' && c <= '9'; }
bool lowerAlnum(char c) { return digit(c) || (c >= 'a' && c <= 'z'); }

}  // namespace

bool utf8Valid(const char* s, size_t n) {
  const auto* p = reinterpret_cast<const unsigned char*>(s);
  size_t i = 0;
  uint32_t cp;
  while (i < n) {
    const size_t k = decode(p + i, n - i, &cp);
    if (!k) return false;
    i += k;
  }
  return true;
}

size_t utf8Length(const std::string& s) {
  size_t n = 0;
  for (unsigned char c : s) n += (c & 0xC0) != 0x80;
  return n;
}

std::string pyStrip(const std::string& s) {
  const auto* p = reinterpret_cast<const unsigned char*>(s.data());
  size_t begin = 0, end = 0, i = 0;
  bool seen = false;
  uint32_t cp;
  while (i < s.size()) {
    size_t k = decode(p + i, s.size() - i, &cp);
    if (!k) k = 1, cp = 0xFFFD;
    if (!pySpace(cp)) {
      if (!seen) begin = i, seen = true;
      end = i + k;
    }
    i += k;
  }
  return seen ? s.substr(begin, end - begin) : std::string();
}

std::string pyRepr(JsonVariantConst v) {
  if (v.isNull()) return "None";
  if (v.is<bool>()) return v.as<bool>() ? "True" : "False";
  if (v.is<const char*>()) {
    const std::string s = v.as<const char*>();
    const bool dq = s.find('\'') != std::string::npos && s.find('"') == std::string::npos;
    const char q = dq ? '"' : '\'';
    std::string out(1, q);
    for (unsigned char c : s) {
      if (c == '\\') out += "\\\\";
      else if (c == static_cast<unsigned char>(q)) out += std::string("\\") + q;
      else if (c == '\n') out += "\\n";
      else if (c == '\r') out += "\\r";
      else if (c == '\t') out += "\\t";
      else if (c < 0x20 || c == 0x7F) {
        char buf[5];
        snprintf(buf, sizeof buf, "\\x%02x", c);
        out += buf;
      } else {
        out += static_cast<char>(c);
      }
    }
    return out + q;
  }
  if (isJsonInt(v)) return std::to_string(v.as<long long>());
  if (isJsonNumber(v)) {
    char buf[32];
    snprintf(buf, sizeof buf, "%.17g", v.as<double>());
    // repr() prints the shortest string that round-trips; try shorter ones first.
    for (int p = 1; p <= 17; p++) {
      snprintf(buf, sizeof buf, "%.*g", p, v.as<double>());
      if (strtod(buf, nullptr) == v.as<double>()) break;
    }
    std::string s = buf;
    if (s.find_first_of(".eni") == std::string::npos) s += ".0";
    return s;
  }
  return "...";
}

bool isId(const std::string& s) {
  if (s.empty() || s.size() > 32) return false;
  for (char c : s) {
    if (!(digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-')) return false;
  }
  return true;
}

bool isMediaName(const std::string& s) {
  // 1 leading character, 0 to 47 more, then ".gif".
  if (s.size() < 5 || s.size() > 52 || s.compare(s.size() - 4, 4, ".gif") != 0) return false;
  if (!lowerAlnum(s[0])) return false;
  for (size_t i = 1; i < s.size() - 4; i++) {
    const char c = s[i];
    if (!(lowerAlnum(c) || c == '.' || c == '_' || c == '-')) return false;
  }
  return true;
}

namespace {
bool digits(const std::string& s, size_t lo, size_t hi) {
  if (s.size() < lo || s.size() > hi) return false;
  for (char c : s) {
    if (!digit(c)) return false;
  }
  return true;
}
}  // namespace

bool isPin(const std::string& s) { return digits(s, 6, 12); }
bool isAccountId(const std::string& s) { return digits(s, 0, 32); }
bool isGeneration(const std::string& s) { return digits(s, 1, 9); }

bool isCalendarDate(const std::string& s) {
  if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
  if (!digits(s.substr(0, 4), 4, 4) || !digits(s.substr(5, 2), 2, 2) || !digits(s.substr(8, 2), 2, 2)) return false;
  const int y = atoi(s.substr(0, 4).c_str()), m = atoi(s.substr(5, 2).c_str()), d = atoi(s.substr(8, 2).c_str());
  if (y < 1 || m < 1 || m > 12 || d < 1) return false;
  static const int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
  return d <= kDays[m - 1] + (m == 2 && leap ? 1 : 0);
}

bool isAccessToken(const std::string& s) {
  if (s.size() > 1024) return false;
  for (char c : s) {
    if (!(digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-' || c == '|' ||
          c == '.')) {
      return false;
    }
  }
  return true;
}

bool parseTilePath(const std::string& rel, std::string* gen, std::string* file) {
  const size_t slash = rel.find('/');
  if (slash == std::string::npos) return false;
  const std::string g = rel.substr(0, slash), f = rel.substr(slash + 1);
  if (!isGeneration(g)) return false;
  size_t i;
  if (f.compare(0, 6, "story-") == 0) {
    i = 6;
  } else if (f.compare(0, 5, "post-") == 0) {
    i = 5;
  } else {
    return false;
  }
  size_t n = 0;
  while (i + n < f.size() && digit(f[i + n])) n++;
  if (n < 1 || n > 2 || f.compare(i + n, std::string::npos, ".bmp") != 0) return false;
  if (gen) *gen = g;
  if (file) *file = f;
  return true;
}

bool isJsonInt(JsonVariantConst v) {
  return !v.is<bool>() && (v.is<long long>() || v.is<unsigned long long>());
}

bool isJsonNumber(JsonVariantConst v) { return !v.is<bool>() && v.is<double>(); }

bool hasKey(JsonObjectConst o, const char* key) {
  for (JsonPairConst kv : o) {
    if (kv.key() == key) return true;
  }
  return false;
}

std::string hex(const uint8_t* data, size_t n) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out(n * 2, '0');
  for (size_t i = 0; i < n; i++) {
    out[2 * i] = kDigits[data[i] >> 4];
    out[2 * i + 1] = kDigits[data[i] & 15];
  }
  return out;
}

bool unhex(const std::string& s, uint8_t* out, size_t n) {
  if (s.size() != 2 * n) return false;
  auto val = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < n; i++) {
    const int hi = val(s[2 * i]), lo = val(s[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<uint8_t>(hi << 4 | lo);
  }
  return true;
}

// Howard Hinnant's civil calendar algorithms: exact for the proleptic Gregorian calendar.
int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void civilFromDays(int64_t z, int* y, unsigned* m, unsigned* d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  *d = doy - (153 * mp + 2) / 5 + 1;
  *m = mp < 10 ? mp + 3 : mp - 9;
  *y = static_cast<int>(static_cast<int64_t>(yoe) + era * 400 + (*m <= 2));
}

std::string civilDate(int64_t days) {
  int y;
  unsigned m, d;
  civilFromDays(days, &y, &m, &d);
  char buf[16];
  snprintf(buf, sizeof buf, "%04d-%02u-%02u", y, m, d);
  return buf;
}

std::string isoUtc(int64_t unix) {
  const int64_t days = unix >= 0 ? unix / 86400 : -((-unix + 86399) / 86400);
  const int64_t secs = unix - days * 86400;
  int y;
  unsigned m, d;
  civilFromDays(days, &y, &m, &d);
  char buf[32];
  snprintf(buf, sizeof buf, "%04d-%02u-%02uT%02d:%02d:%02dZ", y, m, d, static_cast<int>(secs / 3600),
           static_cast<int>(secs / 60 % 60), static_cast<int>(secs % 60));
  return buf;
}

}  // namespace netcore
