// Admin PIN: hashing (API.md 4.1) and the lockout state machine (API.md 4.4).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace netcore {

constexpr int kPinIterations = 10000;
constexpr size_t kPinSaltBytes = 16;
constexpr size_t kMaxPinHeader = 64;  // longer header values are wrong without hashing

struct Sha256 {
  uint32_t h[8];
  uint8_t block[64];
  uint64_t bytes = 0;
  size_t used = 0;
  Sha256();
  void update(const void* data, size_t n);
  void final(uint8_t out[32]);
};

void sha256(const void* data, size_t n, uint8_t out[32]);

// PBKDF2-HMAC-SHA256 with a 32-byte output (one block). The HMAC inner and outer states
// are computed once, so each iteration costs two compressions instead of four.
void pbkdf2Sha256(const std::string& password, const uint8_t* salt, size_t saltLen, uint32_t iterations,
                  uint8_t out[32]);

bool constantTimeEqual(const uint8_t* a, const uint8_t* b, size_t n);

// Fresh pin_salt / pin_hash for `pin`. `random` fills a buffer with secure random bytes.
struct PinRecord {
  std::string salt;  // 32 lowercase hex
  std::string hash;  // 64 lowercase hex
  int iterations = kPinIterations;
};
PinRecord makePinRecord(const std::string& pin, const std::function<void(uint8_t*, size_t)>& random);

// The full check: format of the stored record, header length, constant-time compare.
bool pinMatches(const std::string& pin, const std::string& saltHex, const std::string& hashHex, long long iterations);

// Wrong-PIN accounting per client and for everyone. Time is a monotonic clock in
// milliseconds supplied by the caller (tests inject one), never the wall clock.
class PinGuard {
 public:
  static constexpr int kFreeFails = 5;
  static constexpr int64_t kFirstLockMs = 30 * 1000;
  static constexpr int64_t kMaxLockMs = 15 * 60 * 1000;
  static constexpr int kGlobalFails = 20;
  static constexpr int64_t kGlobalWindowMs = 10 * 60 * 1000;
  static constexpr int64_t kGlobalLockMs = 15 * 60 * 1000;

  // maxClients: API.md allows fewer than the mock's 64 but at least 16.
  PinGuard(std::function<int64_t()> clockMs, size_t maxClients = 32);

  int waitSeconds(const std::string& ip) const;  // whole seconds, rounded up; 0 = not locked
  void fail(const std::string& ip);
  void succeed(const std::string& ip);
  size_t clientCount() const { return clients_.size(); }

 private:
  struct Client {
    std::string ip;
    int fails = 0;
    int64_t until = 0;
  };
  std::function<int64_t()> clock_;
  size_t maxClients_;
  std::vector<Client> clients_;  // oldest entry first
  std::deque<int64_t> recent_;   // wrong PINs from anyone in the last 10 minutes
  int64_t globalUntil_ = 0;
};

}  // namespace netcore
