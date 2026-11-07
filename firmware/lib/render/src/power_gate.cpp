#include "power_gate.h"

namespace wall {
namespace render {

PowerGate::Step PowerGate::tick(bool wantOn, uint32_t nowMs, ScreenClock* clock) {
  if (!wantOn) {
    if (!off_) {
      off_ = true;
      blanked_ = false;
      offAtMs_ = nowMs;
    }
    if (blanked_) return Step::kIdle;
    blanked_ = true;
    return Step::kBlank;
  }
  if (!off_) return Step::kRun;
  off_ = false;
  // Time off doesn't count against the screen, like a pause. While paused the elapsed time
  // is already frozen at pausedAtMs, and the later resume moves startMs on by the whole
  // pause, off time included.
  const uint32_t away = nowMs - offAtMs_;
  if (!clock->paused) clock->startMs += away;
  clock->lastDrawMs += away;
  clock->nextFrameMs += away;
  if (clock->idleUntilMs) {
    clock->idleUntilMs += away;
    if (!clock->idleUntilMs) clock->idleUntilMs = 1;  // 0 means "not waiting"
  }
  return Step::kResume;
}

}  // namespace render
}  // namespace wall
