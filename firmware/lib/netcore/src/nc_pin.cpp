#include "nc_pin.h"

#include <string.h>

#include <algorithm>

#include "nc_text.h"

namespace netcore {

namespace {

const uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

const uint32_t kInit[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void compress(uint32_t h[8], const uint8_t block[64]) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++) {
    w[i] = static_cast<uint32_t>(block[4 * i]) << 24 | static_cast<uint32_t>(block[4 * i + 1]) << 16 |
           static_cast<uint32_t>(block[4 * i + 2]) << 8 | block[4 * i + 3];
  }
  for (int i = 16; i < 64; i++) {
    const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
  for (int i = 0; i < 64; i++) {
    const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kK[i] + w[i];
    const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
  }
  h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
}

void putBe32(uint8_t* p, uint32_t v) {
  p[0] = v >> 24, p[1] = v >> 16, p[2] = v >> 8, p[3] = v;
}

// SHA-256 of a 32-byte message that follows one already-absorbed 64-byte block, from
// that state: the message, padding and length fit in a single block.
void finish32(const uint32_t state[8], const uint8_t msg[32], uint8_t out[32]) {
  uint8_t block[64] = {};
  memcpy(block, msg, 32);
  block[32] = 0x80;
  putBe32(block + 60, (64 + 32) * 8);
  uint32_t h[8];
  memcpy(h, state, sizeof h);
  compress(h, block);
  for (int i = 0; i < 8; i++) putBe32(out + 4 * i, h[i]);
}

}  // namespace

Sha256::Sha256() { memcpy(h, kInit, sizeof h); }

void Sha256::update(const void* data, size_t n) {
  const auto* p = static_cast<const uint8_t*>(data);
  bytes += n;
  while (n) {
    const size_t take = std::min(n, sizeof block - used);
    memcpy(block + used, p, take);
    used += take, p += take, n -= take;
    if (used == sizeof block) {
      compress(h, block);
      used = 0;
    }
  }
}

void Sha256::final(uint8_t out[32]) {
  const uint64_t bits = bytes * 8;
  const uint8_t pad = 0x80, zero = 0;
  update(&pad, 1);
  while (used != 56) update(&zero, 1);
  uint8_t len[8];
  for (int i = 0; i < 8; i++) len[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
  update(len, 8);
  for (int i = 0; i < 8; i++) putBe32(out + 4 * i, h[i]);
}

void sha256(const void* data, size_t n, uint8_t out[32]) {
  Sha256 s;
  s.update(data, n);
  s.final(out);
}

void pbkdf2Sha256(const std::string& password, const uint8_t* salt, size_t saltLen, uint32_t iterations,
                  uint8_t out[32]) {
  uint8_t key[64] = {};
  if (password.size() > 64) {
    sha256(password.data(), password.size(), key);
  } else {
    memcpy(key, password.data(), password.size());
  }
  uint8_t ipad[64], opad[64];
  for (int i = 0; i < 64; i++) ipad[i] = key[i] ^ 0x36, opad[i] = key[i] ^ 0x5c;
  uint32_t inner[8], outer[8];
  memcpy(inner, kInit, sizeof inner);
  memcpy(outer, kInit, sizeof outer);
  compress(inner, ipad);
  compress(outer, opad);

  // U1 = HMAC(P, salt || INT(1)); the salt is short but handle any length generally.
  Sha256 s;
  s.update(ipad, 64);
  s.update(salt, saltLen);
  const uint8_t one[4] = {0, 0, 0, 1};
  s.update(one, 4);
  uint8_t u[32];
  s.final(u);
  finish32(outer, u, u);
  memcpy(out, u, 32);
  for (uint32_t it = 1; it < iterations; it++) {
    finish32(inner, u, u);
    finish32(outer, u, u);
    for (int i = 0; i < 32; i++) out[i] ^= u[i];
  }
}

bool constantTimeEqual(const uint8_t* a, const uint8_t* b, size_t n) {
  uint8_t diff = 0;
  for (size_t i = 0; i < n; i++) diff |= a[i] ^ b[i];
  return diff == 0;
}

PinRecord makePinRecord(const std::string& pin, const std::function<void(uint8_t*, size_t)>& random) {
  uint8_t salt[kPinSaltBytes], hash[32];
  random(salt, sizeof salt);
  pbkdf2Sha256(pin, salt, sizeof salt, kPinIterations, hash);
  PinRecord r;
  r.salt = hex(salt, sizeof salt);
  r.hash = hex(hash, sizeof hash);
  return r;
}

bool pinMatches(const std::string& pin, const std::string& saltHex, const std::string& hashHex, long long iterations) {
  if (pin.size() > kMaxPinHeader || iterations < 1 || iterations > 10000000) return false;
  // The mock accepts any salt length; settings.json always holds 16 bytes, but don't
  // refuse a longer one written by hand.
  if (saltHex.size() % 2 || saltHex.size() > 256) return false;
  std::vector<uint8_t> salt(saltHex.size() / 2);
  uint8_t want[32], got[32];
  if (!unhex(saltHex, salt.data(), salt.size()) || !unhex(hashHex, want, sizeof want)) return false;
  pbkdf2Sha256(pin, salt.data(), salt.size(), static_cast<uint32_t>(iterations), got);
  return constantTimeEqual(got, want, sizeof want);
}

PinGuard::PinGuard(std::function<int64_t()> clockMs, size_t maxClients)
    : clock_(std::move(clockMs)), maxClients_(maxClients < 16 ? 16 : maxClients) {}

int PinGuard::waitSeconds(const std::string& ip) const {
  const int64_t now = clock_();
  int64_t left = globalUntil_ - now;
  for (const Client& c : clients_) {
    if (c.ip == ip) left = std::max(left, c.until - now);
  }
  return left > 0 ? static_cast<int>((left + 999) / 1000) : 0;
}

void PinGuard::fail(const std::string& ip) {
  const int64_t now = clock_();
  Client c;
  c.ip = ip;
  for (auto it = clients_.begin(); it != clients_.end(); ++it) {
    if (it->ip == ip) {
      c = *it;
      clients_.erase(it);  // re-inserted at the back: "oldest entry" is the least recently failed
      break;
    }
  }
  c.fails++;
  if (c.fails >= kFreeFails) {
    const int doublings = c.fails - kFreeFails;
    // 30 s doubles past 900 s by the 6th doubling; stop shifting before it overflows.
    const int64_t lock = doublings >= 6 ? kMaxLockMs : std::min(kFirstLockMs << doublings, kMaxLockMs);
    c.until = now + lock;
  }
  clients_.push_back(c);
  while (clients_.size() > maxClients_) clients_.erase(clients_.begin());  // the global rule still covers an IP flood

  recent_.push_back(now);
  while (!recent_.empty() && recent_.front() <= now - kGlobalWindowMs) recent_.pop_front();
  if (static_cast<int>(recent_.size()) >= kGlobalFails) {
    globalUntil_ = now + kGlobalLockMs;
    recent_.clear();
  }
}

void PinGuard::succeed(const std::string& ip) {
  clients_.erase(std::remove_if(clients_.begin(), clients_.end(), [&](const Client& c) { return c.ip == ip; }),
                 clients_.end());
}

}  // namespace netcore
