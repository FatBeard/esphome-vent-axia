#include "sequence.h"

#include "screens.h"
#include "status.h"

namespace esphome {
namespace vent_axia {

namespace {
constexpr protocol::KeyMask MAIN = protocol::key_mask(protocol::Key::MAIN);
}  // namespace

uint8_t SetAirflowMode::presses_for_(AirflowTarget target) {
  switch (target) {
    case AirflowTarget::NORMAL:
      return 0;
    case AirflowTarget::BOOST_30:
      return 1;
    case AirflowTarget::BOOST_60:
      return 2;
    case AirflowTarget::BOOST_CONTINUOUS:
      // The manual's press table, measured on this unit
      // (mhrv_orig/vent-axia-esphome-project.md): 1 = Boost 30, 2 = Boost 60,
      // 3 = continuous, 4 = back to Normal.
      //
      // Three taps in one batch is the case key_gap exists for: 400ms "is
      // still fast enough for the Boost press-counter: three taps land
      // exactly on 'continuous', not on three separate 30-minute boosts"
      // (vent-axia-esphome-project.md). Keypad's key_gap_ms_ (400ms default)
      // is what enforces that, NOT the Runner's tap_duration_ms() below --
      // that is the duration of each individual press (50ms by default), a
      // different quantity. Do not be tempted by mhrv_orig/controls.yaml's
      // "Presses 250ms apart are counted individually and correctly": that
      // comment predates the measurement that overturned it (250ms drops
      // roughly one press in ten -- CLAUDE.md's key_gap invariant), and
      // 250ms would silently merge taps here.
      return 3;
    case AirflowTarget::PURGE:
    default:
      return 0;  // unreachable -- PURGE is handled entirely by CHECK_CURRENT/hold_main_, never reaches APPLY
  }
}

void SetAirflowMode::on_start() {
  this->guard_ = 0;
  // Reset alongside guard_ -- see STUCK_TAP_LIMIT's own comment (sequence.h)
  // for why this belongs here and not the constructor: this Sequence is a
  // long-lived hub member reused across runs, and a run's stuck-detection
  // state must not carry over from whatever the previous run last observed.
  this->last_airflow_percent_.reset();
  this->last_countdown_minutes_.reset();
  this->have_stuck_sample_ = false;
  this->stuck_taps_ = 0;
}

Poll SetAirflowMode::hold_main_(uint8_t next_step) {
  // never Set -- always accepted, see Runner::press(). Re-asserted every
  // tick rather than once, same self-healing reasoning as HoldUntil::poll()
  // (sequence.cpp): press() is a documented no-op when re-asserting a mask
  // already held.
  this->runner_->press(MAIN);
  if (this->elapsed() < PURGE_HOLD_MS) {
    return Poll::RUNNING;
  }
  this->runner_->release();
  return this->goto_step(next_step);
}

Poll SetAirflowMode::after_probe_(bool boosting_now) {
  if (!boosting_now) {
    // Confirmed Normal (or never actually left it) -- apply the target's
    // own presses from here. 0 presses (target == NORMAL, or a target that
    // was already reached) skips straight to FINISHED rather than queuing
    // an empty batch of taps.
    const uint8_t presses = presses_for_(this->target_);
    return presses > 0 ? this->goto_step(APPLY_TAP) : this->goto_step(FINISHED);
  }
  // Stuck-tap detection (STUCK_TAP_LIMIT's own comment, sequence.h): sample
  // the SAME instantaneous read PROBE_CHECK/PROBE_WAIT just used to conclude
  // "still boosting" -- status::parse_line_values() on the CURRENT frame,
  // deliberately not status_'s sticky purging()-style tracker. Same asymmetry
  // PROBE_CHECK's own defensive re-check below already relies on: staleness
  // here only costs one wasted comparison against a value about to be
  // overwritten anyway, whereas a STICKY read could paper over a genuinely
  // stuck counter for up to StatusTracker::ALTERNATION_TIMEOUT_MS, exactly
  // the false confidence this check exists to avoid.
  const status::LineValues probe_values =
      status::parse_line_values(this->runner_->display().raw_line1(), this->runner_->display().raw_line2());
  if (this->have_stuck_sample_ && probe_values.airflow_percent == this->last_airflow_percent_ &&
      probe_values.countdown_minutes == this->last_countdown_minutes_) {
    this->stuck_taps_++;
  } else {
    this->stuck_taps_ = 0;
  }
  this->have_stuck_sample_ = true;
  this->last_airflow_percent_ = probe_values.airflow_percent;
  this->last_countdown_minutes_ = probe_values.countdown_minutes;

  if (this->stuck_taps_ >= STUCK_TAP_LIMIT) {
    // Named cause, not the generic "refusing to guess" wording below -- this
    // path has POSITIVE evidence of what's wrong (line2 provably did not
    // move across two taps that line1's own continued alternation shows the
    // unit was still alive and responding to). Captured live on
    // 192.168.1.200, 13 Aug 2026: a toilet boost held on by a light-linked
    // switched live.
    if (this->log().error) {
      this->log().error(
          "SetAirflowMode: boost appears held on by an external switched live (a wired wall/toilet switch) -- "
          "the display has not moved across " +
          std::to_string(static_cast<int>(this->stuck_taps_)) +
          " consecutive Main taps, so this cannot be cleared from Home Assistant until that switch releases");
    }
    return Poll::FAILED;
  }

  if (this->guard_ >= NORMALISE_GUARD) {
    // mirrors mhrv_orig's own boost_set, which silently skipped applying
    // anything in exactly this situation -- refusing is safer than layering
    // the target's own presses on top of a press count that is no longer
    // known, same reasoning as AdjustField's own guard.
    if (this->log().error) {
      this->log().error("SetAirflowMode: could not normalise back to Normal after " +
                        std::to_string(static_cast<int>(NORMALISE_GUARD)) +
                        " Main taps -- refusing to guess the unit's boost state from here");
    }
    return Poll::FAILED;
  }
  this->guard_++;
  // never Set -- always accepted, see Runner::tap()
  this->runner_->tap(MAIN, this->runner_->tap_duration_ms());
  return this->goto_step(NORMALISE_WAIT);
}

Poll SetAirflowMode::poll() {
  switch (this->step_) {
    // No keys pressed. First PROVE the display is actually on the status
    // loop before trusting anything read off it, or pressing Main at all --
    // "Main is never a menu key" applies to being PARKED on a menu/
    // diagnostic screen just as much as to pressing one deliberately. This
    // is a reachable start state, not a hypothetical one: Runner::recover()'s
    // own exit tap does not wait for the unit to actually leave the menu,
    // the unit's own ~2-minute timeout may still be running, and a human at
    // the unit's own keypad can park it there at any moment. From a menu/
    // diagnostic screen, classify_line() can never read BOOST_AIRFLOW and
    // purging() can never be true, so without this check the probe below
    // would conclude "Normal" with zero evidence and go on to tap Main
    // against a boost counter whose position was never actually read -- the
    // same failure WriteSetting's VERIFY step and ResetFilter's own screen
    // check both exist to rule out elsewhere in this component. No retry
    // window: a menu screen does not clear itself within a sequence's
    // patience -- the unit's own timeout is minutes.
    case CHECK_CURRENT: {
      if (!this->runner_->display().have_frame() ||
          screens::is_menu_screen(this->runner_->display().raw_line1())) {
        if (this->log().error) {
          this->log().error("SetAirflowMode: refusing -- display is not on the status loop (line1='" +
                            this->runner_->display().text_line1() + "')");
        }
        return Poll::FAILED;
      }

      // status_'s STICKY, alternation-aware purging() -- not a raw
      // single-frame parse. The status loop alternates, which is exactly why
      // StatusTracker models purging_ as a Flag with its own
      // ALTERNATION_TIMEOUT_MS rather than trusting one frame, and the
      // purge layout being otherwise unresolved makes a single-frame miss
      // MORE likely, not less. A missing tracker or a not-yet-known reading
      // (nullopt -- no status frame decoded yet) must FAIL, not be read as
      // "not purging" -- "Blank != zero" applies to this derived boolean the
      // same as a parsed field: guessing "not purging" here could CANCEL a
      // purge that is actually still running.
      if (this->status_ == nullptr) {
        if (this->log().error) {
          this->log().error("SetAirflowMode: refusing -- no StatusTracker configured (set_status() was never called)");
        }
        return Poll::FAILED;
      }
      const std::optional<bool> currently_purging = this->status_->purging();
      if (!currently_purging.has_value()) {
        if (this->log().error) {
          this->log().error("SetAirflowMode: refusing -- purge state not yet known (no status frame decoded yet)");
        }
        return Poll::FAILED;
      }

      if (this->target_ == AirflowTarget::PURGE) {
        // Purge is idempotent: if it's already showing, there is nothing to
        // press. Otherwise it is a direct hold from wherever the boost
        // counter happens to sit -- the manual describes Purge as reachable
        // from any state, not something requiring Normal first. Accepted
        // edge (class comment): a purge that ended within the last ~12s
        // (ALTERNATION_TIMEOUT_MS) still reads *currently_purging == true
        // here, so this lands on the no-op branch and does nothing rather
        // than re-opening Purge -- the safe direction of the trade.
        return *currently_purging ? this->goto_step(FINISHED) : this->goto_step(OPEN_PURGE);
      }
      return *currently_purging ? this->goto_step(CANCEL_PURGE) : this->goto_step(PROBE_CHECK);
    }

    case CANCEL_PURGE:
      return this->hold_main_(CANCEL_SETTLE);

    // Explicit silence before the first subsequent tap -- CLAUDE.md's
    // "release is silence... a hold-to-hold transition needs an explicit
    // gap (~400ms) or the unit reads one unbroken press" applies just as
    // much to a hold followed by a TAP as to two holds back to back:
    // Keypad's own automatic key_gap only fires BETWEEN two taps it queued
    // itself (keypad.h), never after a press()-driven hold's release(), so
    // without this the very next Main tap (whether PROBE_CHECK concludes
    // instantly or the eventual normalising tap) could land close enough
    // behind the cancel hold's release to read as one unbroken press.
    case CANCEL_SETTLE:
      return this->elapsed() >= CANCEL_SETTLE_MS ? this->goto_step(PROBE_CHECK) : Poll::RUNNING;

    case OPEN_PURGE:
      return this->hold_main_(FINISHED);

    // mhrv_orig's boost_probe (class comment, step 3), plus a defensive
    // re-check of the Purge flag: matters for the post-CANCEL_PURGE path
    // specifically -- if the 5.5s hold somehow did not actually cancel Purge
    // (untested on real hardware), classify_line() below would read Purge's
    // line1 as "not boosting" and go on to tap Main believing it is
    // adjusting the Normal/Boost counter -- an ambiguous, untested
    // interaction safer to refuse than guess at. A no-op for the direct
    // (never-purging) entry path, which already confirmed this at
    // CHECK_CURRENT.
    //
    // Deliberately the INSTANTANEOUS status::parse_line_values() here, NOT
    // status_'s sticky purging() CHECK_CURRENT uses above -- intentional,
    // not an inconsistency. The two checks answer different questions with
    // different costs for being stale: CHECK_CURRENT asks "is it currently
    // purging", where a stale TRUE just costs one extra safe no-op (the
    // accepted edge, class comment) but a stale FALSE risks a
    // wrong-direction cancel hold -- the sticky flag's bias toward TRUE is
    // exactly the safe direction there. This check asks "did the cancel
    // hold I just ran actually fail", where the OPPOSITE bias is safe: a
    // stale sticky TRUE would stay true for up to ALTERNATION_TIMEOUT_MS
    // (~12s) after a genuinely successful cancel and would turn every
    // correct cancel into a false failure here, whereas a single frame
    // showing "Purge" immediately after a hold meant to change that is
    // real, trustworthy evidence the cancel failed.
    case PROBE_CHECK:
      if (status::parse_line_values(this->runner_->display().raw_line1(), this->runner_->display().raw_line2()).purge) {
        if (this->log().error) {
          this->log().error(
              "SetAirflowMode: still showing Purge after the cancel hold -- refusing to guess what Main taps "
              "would do from here");
        }
        return Poll::FAILED;
      }
      if (status::classify_line(this->runner_->display().raw_line1()) == status::LineMessage::BOOST_AIRFLOW) {
        return this->after_probe_(true);
      }
      return this->goto_step(PROBE_WAIT);

    // Line1 alternates roughly every 3.2-3.5s (status.h), so a single
    // unlucky sample can catch the other half of the cycle -- only a
    // genuine PROBE_TIMEOUT_MS of silence on this frame proves "not
    // boosting"; a Boost frame arriving at any point before that concludes
    // the probe early as "boosting", same as PROBE_CHECK's immediate case.
    case PROBE_WAIT:
      if (status::classify_line(this->runner_->display().raw_line1()) == status::LineMessage::BOOST_AIRFLOW) {
        return this->after_probe_(true);
      }
      return this->elapsed() >= PROBE_TIMEOUT_MS ? this->after_probe_(false) : Poll::RUNNING;

    case NORMALISE_WAIT:
      return this->runner_->keypad_busy() ? Poll::RUNNING : this->goto_step(NORMALISE_SETTLE);

    // mhrv_orig's own `delay: 1s` before re-probing -- long enough for the
    // unit's own counter and display to have genuinely caught up to the tap
    // just sent, so the next probe judges the NEW state rather than stale
    // evidence of the old one. Normalising may still pass THROUGH continuous
    // boost transiently on the way back to Normal when the TARGET is
    // something else (e.g. Boost30 -> Boost60 -> Continuous -> Normal is one
    // unavoidable lap starting from Boost30) -- deliberate and harmless, not
    // a bug: this sequence is never polled or read while merely passing
    // through it, so nothing observes or reports that transient intermediate
    // state. This note is only about that transient pass-through case, not
    // about selecting BOOST_CONTINUOUS directly, which is a first-class
    // TARGET in its own right (AirflowTarget's own comment, sequence.h).
    case NORMALISE_SETTLE:
      return this->elapsed() >= NORMALISE_SETTLE_MS ? this->goto_step(PROBE_CHECK) : Poll::RUNNING;

    // Queued as a batch, same as GotoMenu's own taps (sequence.cpp) --
    // Keypad's queue enforces key_gap between each, so this is "wait for
    // the queue to drain", not one step per press.
    case APPLY_TAP: {
      const uint8_t presses = presses_for_(this->target_);
      for (uint8_t i = 0; i < presses; i++) {
        this->runner_->tap(MAIN, this->runner_->tap_duration_ms());  // never Set -- always accepted, see Runner::tap()
      }
      return this->goto_step(APPLY_WAIT);
    }

    case APPLY_WAIT:
      return this->runner_->keypad_busy() ? Poll::RUNNING : this->goto_step(FINISHED);

    case FINISHED:
      return Poll::DONE;

    // step_ somehow outside the Step enum -- a bug, not a legitimate landing
    // state. FAILED routes through Runner::recover() rather than reporting
    // an airflow-mode change as applied when the sequence had gone off the
    // rails.
    default:
      if (this->log().error) {
        this->log().error("SetAirflowMode: invalid step " + std::to_string(static_cast<int>(this->step_)));
      }
      return Poll::FAILED;
  }
}

void SetAirflowMode::on_finish(Poll result) {
  (void) result;
  // Backstop release -- hold_main_() already releases on its own successful
  // exit and every tap already returns the keypad to idle after its own
  // key_gap, but this guarantees release regardless of which step this run
  // happened to fail or time out on (e.g. a FAILED guard exhaustion mid
  // NORMALISE_SETTLE, or the root timeout firing while CANCEL_PURGE/
  // OPEN_PURGE is still mid-hold).
  this->runner_->release();
}

}  // namespace vent_axia
}  // namespace esphome
