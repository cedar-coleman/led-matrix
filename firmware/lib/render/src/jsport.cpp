#include "jsport.h"

#include <math.h>
#include <stdio.h>

namespace wall {
namespace render {

JsString fromUtf8(const std::string& s) {
  JsString out;
  out.reserve(s.size());
  const auto* p = reinterpret_cast<const unsigned char*>(s.data());
  const size_t n = s.size();
  for (size_t i = 0; i < n;) {
    const unsigned c = p[i];
    uint32_t cp = 0xFFFD;
    size_t len = 1;
    if (c < 0x80) {
      cp = c;
    } else if (c >= 0xC2 && c <= 0xF4) {
      const size_t need = c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
      if (i + need <= n) {
        uint32_t v = c & (need == 2 ? 0x1F : need == 3 ? 0x0F : 0x07);
        bool ok = true;
        for (size_t k = 1; k < need; k++) {
          if ((p[i + k] & 0xC0) != 0x80) {
            ok = false;
            break;
          }
          v = (v << 6) | (p[i + k] & 0x3F);
        }
        const uint32_t minimum = need == 2 ? 0x80 : need == 3 ? 0x800 : 0x10000;
        if (ok && v >= minimum && v <= 0x10FFFF && !(v >= 0xD800 && v <= 0xDFFF)) {
          cp = v;
          len = need;
        }
      }
    }
    if (cp >= 0x10000) {
      cp -= 0x10000;
      out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
      out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
    } else {
      out.push_back(static_cast<char16_t>(cp));
    }
    i += len;
  }
  return out;
}

std::string toUtf8(const JsString& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    uint32_t cp = s[i];
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
      i++;
    } else if (cp >= 0xD800 && cp <= 0xDFFF) {
      cp = 0xFFFD;
    }
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }
  return out;
}

JsString jsSlice(const JsString& s, long from, long to) {
  const long len = static_cast<long>(s.size());
  if (from < 0) from = from + len < 0 ? 0 : from + len;
  if (to < 0) to = to + len < 0 ? 0 : to + len;
  if (from > len) from = len;
  if (to > len) to = len;
  if (to <= from) return JsString();
  return s.substr(static_cast<size_t>(from), static_cast<size_t>(to - from));
}

double jsRound(double x) {
  const double f = floor(x);
  return (x - f >= 0.5) ? f + 1 : f;
}

std::string toFixed1(double x) {
  char buf[48];
  // x * 20 lands on an odd integer exactly when x is a tie at one decimal. fma gives the
  // product's rounding error, so a product that merely rounds onto an odd integer
  // (1.15 is really 1.1499999...) is not mistaken for a tie.
  const double y = x * 20.0;
  if (fma(x, 20.0, -y) == 0.0 && y == floor(y) && fmod(y, 2.0) == 1.0) {
    const long long n = static_cast<long long>((y + 1) / 2);  // tenths, rounded up
    snprintf(buf, sizeof buf, "%lld.%lld", n / 10, n % 10);
    return buf;
  }
  snprintf(buf, sizeof buf, "%.1f", x);
  return buf;
}

std::string intString(double v) {
  char buf[32];
  snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v));
  return buf;
}

int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
  // Howard Hinnant's days_from_civil; d past the month's end simply carries on, which is
  // how V8 treats "2026-02-30".
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468 + static_cast<int64_t>(d) - 1;
}

namespace {

bool digits(const char*& p, int count, int* out) {
  int v = 0;
  for (int i = 0; i < count; i++) {
    if (p[i] < '0' || p[i] > '9') return false;
    v = v * 10 + (p[i] - '0');
  }
  p += count;
  *out = v;
  return true;
}

}  // namespace

bool parseIsoMs(const std::string& s, int64_t* ms) {
  const char* p = s.c_str();
  int year = 0, month = 1, day = 1, hour = 0, minute = 0, second = 0, milli = 0;
  if (!digits(p, 4, &year)) return false;
  if (*p == '-') {
    p++;
    if (!digits(p, 2, &month)) return false;
    if (*p == '-') {
      p++;
      if (!digits(p, 2, &day)) return false;
    }
  }
  int64_t offsetMin = 0;
  if (*p == 'T' || *p == ' ') {
    p++;
    if (!digits(p, 2, &hour) || *p++ != ':' || !digits(p, 2, &minute)) return false;
    if (*p == ':') {
      p++;
      if (!digits(p, 2, &second)) return false;
      if (*p == '.') {
        p++;
        if (*p < '0' || *p > '9') return false;
        int scale = 100;
        while (*p >= '0' && *p <= '9') {
          milli += (*p - '0') * scale;
          scale /= 10;
          p++;
        }
      }
    }
    if (*p == 'Z') {
      p++;
    } else if (*p == '+' || *p == '-') {
      const int sign = *p++ == '-' ? -1 : 1;
      int oh = 0, om = 0;
      if (!digits(p, 2, &oh)) return false;
      if (*p == ':') p++;
      if (!digits(p, 2, &om)) return false;
      if (oh > 23 || om > 59) return false;
      offsetMin = sign * (oh * 60 + om);
    }
  }
  if (*p != '\0') return false;
  if (month < 1 || month > 12 || day < 1 || day > 31) return false;
  if (minute > 59 || second > 59) return false;
  if (hour > 24 || (hour == 24 && (minute || second || milli))) return false;
  const int64_t days = daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
  *ms = ((days * 24 + hour) * 60 + minute - offsetMin) * 60000 + static_cast<int64_t>(second) * 1000 + milli;
  return true;
}

}  // namespace render
}  // namespace wall
