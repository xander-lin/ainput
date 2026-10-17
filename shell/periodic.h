#pragma once

#include <fcitx-utils/eventloopinterface.h>

/// Re-arms a repeating fcitx5 time event.
///
/// fcitx5 creates time events as ONESHOT: a callback returning true keeps the
/// source alive but does NOT schedule it again. Every periodic timer inside
/// fcitx5 itself calls setNextInterval() + setOneShot() from its own callback.
/// Forgetting this makes the timer fire exactly once and then stay silent —
/// which is invisible in normal use, because nothing visibly breaks; the
/// safety net just is not there.
inline void rearmPeriodic(fcitx::EventSourceTime *timer, uint64_t intervalUsec) {
    if (!timer) {
        return;
    }
    timer->setNextInterval(intervalUsec);
    timer->setOneShot();
}
