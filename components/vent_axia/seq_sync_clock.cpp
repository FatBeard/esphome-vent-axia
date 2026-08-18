#include "sequence.h"

#include "screens.h"

namespace esphome {
namespace vent_axia {

namespace {
constexpr protocol::KeyMask SET = protocol::key_mask(protocol::Key::SET);
constexpr uint32_t TAP_MS = 50;  // "one tap = one menu step", PLAN.md §2

const char *const kDayNames[7] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};

std::string pad2(int v) { return (v < 10 ? "0" : "") + std::to_string(v); }
}  // namespace

bool SyncClock::day_target_(int &out) const {
  int dow = 0, hour = 0, minute = 0;
  if (!this->time_source_ || !this->time_source_(dow, hour, minute)) {
    return false;
  }
  out = dow;
  return true;
}

bool SyncClock::hour_target_(int &out) const {
  int dow = 0, hour = 0, minute = 0;
  if (!this->time_source_ || !this->time_source_(dow, hour, minute)) {
    return false;
  }
  out = hour;
  return true;
}

bool SyncClock::minute_target_(int &out) const {
  int dow = 0, hour = 0, minute = 0;
  if (!this->time_source_ || !this->time_source_(dow, hour, minute)) {
    return false;
  }
  out = minute;
  return true;
}

std::string SyncClock::describe_target_() const {
  int dow = 0, hour = 0, minute = 0;
  if (!this->time_source_ || !this->time_source_(dow, hour, minute)) {
    return "unknown";
  }
  return std::string(kDayNames[dow]) + " " + pad2(hour) + ":" + pad2(minute);
}

Poll SyncClock::poll() {
  switch (this->step_) {
    // Menu index 1 -- Set Clock (sequence.h's GotoMenu class comment's menu
    // map). Blind, same as every other GotoMenu use: it does not look at the
    // display, it just presses the hard-stop-then-count sequence that lands
    // on index 1 regardless of where the display started.
    case NAVIGATE:
      this->nav_started_ms_ = this->runner_->now_ms();
      this->goto_menu_.reset(1);
      return this->await(this->goto_menu_, VERIFY);

    // Right screen, and a clock reading that actually parsed AND is fresh --
    // newer than nav_started_ms_, same "Reading a value off the screen"
    // reasoning as WriteSetting::VERIFY, and the same 3000ms budget. Nothing
    // has been committed yet at this point (Set has not been pressed), so a
    // straight FAILED here is safe: the display is still on a menu screen
    // and Runner::recover()'s own single Up tap is exactly the right unwind.
    //
    // The old script's own delay: 700ms ("let at least one fully rendered
    // frame arrive") is subsumed by read_fresh_value()'s freshness test
    // rather than reproduced as a second, separate wait: a frame that both
    // parses AND postdates nav_started_ms_ already IS "a fully rendered
    // frame that arrived after we got here", which is the thing that delay
    // was waiting for.
    case VERIFY: {
      if (this->runner_->display().screen_kind() != screens::ScreenKind::SET_CLOCK) {
        if (this->elapsed() < VERIFY_TIMEOUT_MS) {
          return Poll::RUNNING;
        }
        if (this->log_.warn) {
          this->log_.warn("SyncClock: aborting, could not reach Set Clock (line1='" +
                           this->runner_->display().text_line1() + "')");
        }
        return Poll::FAILED;
      }
      // Any one of the three clock parsers proves the frame is fully
      // rendered and fresh; which field it happens to name doesn't matter
      // here, only that the screen is readable at all.
      if (read_fresh_value(this->runner_->display(), this->nav_started_ms_, parse_clock_day_field).has_value()) {
        return this->goto_step(CHECK_TIME);
      }
      if (this->elapsed() >= VERIFY_TIMEOUT_MS) {
        if (this->log_.warn) {
          this->log_.warn("SyncClock: aborting, could not read the unit's clock (line2='" +
                           this->runner_->display().text_line2() + "')");
        }
        return Poll::FAILED;
      }
      return Poll::RUNNING;
    }

    // Pulled once here, purely to fail fast (before Set is ever pressed) and
    // to log what is about to happen -- the same pair of facts the old
    // script logged right before it started pressing Set. ADJUST_DAY/HOUR/
    // MINUTE below each pull the SAME time source again, live, once per
    // iteration (AdjustField::TargetFn) -- this step is not a cache of that
    // reading, it exists only for the fail-fast-and-log purpose.
    case CHECK_TIME: {
      int dow = 0, hour = 0, minute = 0;
      if (!this->time_source_ || !this->time_source_(dow, hour, minute)) {
        if (this->log_.error) {
          this->log_.error(
              "SyncClock: aborting, no time source available -- refusing to write an unknown time to the unit");
        }
        return Poll::FAILED;
      }
      if (this->log_.info) {
        this->log_.info("SyncClock: unit says '" + this->runner_->display().text_line2() + "', should be " +
                         this->describe_target_());
      }
      return this->goto_step(OPEN);
    }

    // Improvement on the old YAML's blind press_set: confirms the editor
    // actually opened (the day field blinks) and retries once -- see
    // OpenEditor's own class comment for why a dropped Set here matters more
    // than anywhere else in this sequence: every Up/Down from here on is
    // interpreted as an adjustment, not navigation, so a dropped Set would
    // have them silently walk the menu instead.
    case OPEN:
      return this->await(this->open_editor_, ADJUST_DAY);

    // Day does NOT wrap (direction_no_wrap, not direction_wrap_24/60) -- Up
    // on Sun does nothing on the real unit. Guard 8: the old script's own
    // day field needs at most 6 presses.
    case ADJUST_DAY:
      this->adjust_field_.reset(
          parse_clock_day_field, direction_no_wrap, [this](int &out) { return this->day_target_(out); }, DAY_GUARD);
      return this->await(this->adjust_field_, SET_DAY);

    // Each further Set accepts the current field and advances to the next --
    // never Tap(), which would need a temporary Sequence with nowhere
    // long-lived to live; this mirrors WriteSetting's own COMMIT/
    // WAIT_COMMIT_TAP shape.
    case SET_DAY:
      if (!this->runner_->tap(SET, TAP_MS)) {
        return Poll::FAILED;  // refused by the Set interlock -- see Runner::tap()
      }
      return this->goto_step(WAIT_SET_DAY);

    case WAIT_SET_DAY:
      return this->runner_->keypad_busy() ? Poll::RUNNING : this->goto_step(ADJUST_HOUR);

    // Hour wraps at 24 and takes the shortest path -- direction_wrap_24, not
    // direction_no_wrap. Guard 14: the old script's own hour field needs at
    // most 12 presses.
    case ADJUST_HOUR:
      this->adjust_field_.reset(
          parse_clock_hour_field, direction_wrap_24, [this](int &out) { return this->hour_target_(out); },
          HOUR_GUARD);
      return this->await(this->adjust_field_, SET_HOUR);

    case SET_HOUR:
      if (!this->runner_->tap(SET, TAP_MS)) {
        return Poll::FAILED;
      }
      return this->goto_step(WAIT_SET_HOUR);

    case WAIT_SET_HOUR:
      return this->runner_->keypad_busy() ? Poll::RUNNING : this->goto_step(ADJUST_MINUTE);

    // Minute wraps at 60, same shortest-path reasoning as hour. Guard 34:
    // the old script's own minute field needs at most 30 presses -- the
    // largest of the three, because minute is the field most likely to have
    // drifted furthest, and per-iteration TargetFn re-reads mean a real
    // minute rollover mid-adjustment is followed rather than chased to a
    // value that is already stale.
    case ADJUST_MINUTE:
      this->adjust_field_.reset(
          parse_clock_minute_field, direction_wrap_60, [this](int &out) { return this->minute_target_(out); },
          MINUTE_GUARD);
      return this->await(this->adjust_field_, COMMIT);

    // The fourth Set: commits the minute field and drops out of the editor
    // entirely (unlike SET_DAY/SET_HOUR, which only advance to the next
    // field).
    case COMMIT:
      if (!this->runner_->tap(SET, TAP_MS)) {
        return Poll::FAILED;
      }
      return this->goto_step(WAIT_COMMIT);

    case WAIT_COMMIT:
      return this->runner_->keypad_busy() ? Poll::RUNNING : this->goto_step(SETTLE);

    // NOT just the old script's delay before a log line anymore -- see
    // SETTLE_MS's own comment. Long enough that EXIT_CHAIN's editor_open()
    // check below actually means something: a successful commit ALSO
    // changes line2 (the editor closing repaints the settled screen), so a
    // shorter wait would misread that as "still open" and run ExitEditChain
    // needlessly.
    case SETTLE:
      if (this->elapsed() < SETTLE_MS) {
        return Poll::RUNNING;
      }
      if (this->log_.info) {
        this->log_.info("SyncClock: unit now says '" + this->runner_->display().text_line2() + "', should be " +
                         this->describe_target_());
      }
      return this->goto_step(EXIT_CHAIN);

    // Finding 1's fix: if the fourth (commit) Set was dropped, the editor is
    // still open here -- LEAVE's own LeaveMenu would otherwise tap Up into
    // it, adjusting the minute field instead of navigating out (PLAN.md
    // §3). ExitEditChain is the same walk-out primitive WriteSetting/
    // ReadSettings already use elsewhere in this component; it only ever
    // presses Set. It runs at all only when editor_open() is actually true
    // here, so a normal successful commit -- the editor already closed by
    // the time SETTLE finished -- falls straight through to LEAVE without
    // this step transmitting anything.
    case EXIT_CHAIN:
      if (!this->runner_->display().editor_open(this->runner_->now_ms())) {
        return this->goto_step(LEAVE);
      }
      return this->await(this->exit_chain_, LEAVE);

    // Deliberately LeaveMenu (at most one Up, then wait out the unit's own
    // timeout) and NOT GotoMenu(0): if the editor somehow did not close,
    // mashing Up five times would corrupt the setting -- see LeaveMenu's own
    // class comment. The old script used leave_menu for the same reason.
    // EXIT_CHAIN above is what actually recovers a dropped commit Set;
    // LeaveMenu's own TAP step (Finding 1's structural backstop) is what
    // keeps this step safe even if EXIT_CHAIN's own check were ever wrong.
    case LEAVE:
      return this->await(this->leave_menu_, FINISHED);

    case FINISHED:
    default:
      return Poll::DONE;
  }
}

void SyncClock::on_finish(Poll result) {
  (void) result;
  // Backstop release -- every primitive above already releases whatever it
  // itself asserted (it only ever taps), same reasoning as every other
  // sequence's on_finish() in this file. Main is never pressed anywhere in
  // this sequence (on this unit Main is Boost, mhrv_orig/controls.yaml's own
  // comment), so there is nothing keypad-specific special about this release
  // beyond what every other sequence already does.
  this->runner_->release();
}

}  // namespace vent_axia
}  // namespace esphome
