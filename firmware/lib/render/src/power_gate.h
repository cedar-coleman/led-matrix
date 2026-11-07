// What the render task does about display_set_power (wall_shared.h) on each tick: a pure
// state machine, so the host tests check that the wall idles while off and comes back on
// the same screen with the time it had left.
#pragma once

#include <stdint.h>

namespace wall {
namespace render {

// The current screen's timers, in millis(). Wrap-around is fine: only differences are used.
struct ScreenClock {
  uint32_t startMs = 0;      // when the screen started, moved on by pauses and power-off
  uint32_t pausedAtMs = 0;
  uint32_t lastDrawMs = 0;   // last story redraw
  uint32_t nextFrameMs = 0;  // next GIF frame is due
  uint32_t idleUntilMs = 0;  // 0: not waiting after every screen failed
  bool paused = false;

  uint32_t elapsedMs(uint32_t nowMs) const { return (paused ? pausedAtMs : nowMs) - startMs; }
};

class PowerGate {
 public:
  enum class Step : uint8_t {
    kRun,     // on: carry on as usual
    kBlank,   // just went off (or something was drawn while off): turn the panels black now
    kIdle,    // off: draw nothing, decode nothing, push nothing, take no content or controls
    kResume,  // just came back: the clock has moved on by the time off; show the held frame
  };

  Step tick(bool wantOn, uint32_t nowMs, ScreenClock* clock);

  // A setup or error message was drawn while off; blank again once it goes.
  void shown() { blanked_ = false; }

  bool off() const { return off_; }

 private:
  bool off_ = false;
  bool blanked_ = false;
  uint32_t offAtMs_ = 0;
};

}  // namespace render
}  // namespace wall
