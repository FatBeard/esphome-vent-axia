#include "sequence.h"

#include "screens.h"

namespace esphome {
namespace vent_axia {

namespace {
constexpr protocol::KeyMask UP = protocol::key_mask(protocol::Key::UP);
constexpr protocol::KeyMask DOWN = protocol::key_mask(protocol::Key::DOWN);
}  // namespace

void ResetFilter::on_start() {
  // Opus review (Finding 1): snapshot the CURRENT filter-hours reading
  // before the hold does anything, so VERIFY can tell "the reading moved"
  // apart from "filter_hours_source_ is just answering the same stale value
  // it always has" -- see hours_before_'s own comment (sequence.h) for why
  // that distinction matters and what it does/does not buy. A plain field
  // assignment, not gated on filter_hours_source_ being set: if it isn't,
  // this stores nullopt, and VERIFY's own guard already treats an unset
  // source as "no reading" independently of hours_before_.
  this->hours_before_ = this->filter_hours_source_ ? this->filter_hours_source_() : std::nullopt;
}

Poll ResetFilter::poll() {
  switch (this->step_) {
    // No retry, no wait-and-see -- this is the ONE guard standing between a
    // stray press and an unrecoverable write (this class's own comment,
    // sequence.h), same reasoning as SetAirflowMode's own CHECK_CURRENT
    // (seq_set_airflow_mode.cpp): a menu or diagnostic screen does not clear
    // itself within a sequence's patience, so a wrong screen here means
    // refuse outright, not wait it out. have_frame() is checked first: with
    // no frame at all, line1() is whatever the Display was default- or
    // last-constructed with, which must never be read as "the status
    // screen" just because it happens not to match a known menu prefix.
    //
    // DELIBERATE DIVERGENCE from mhrv_orig/controls.yaml's own reset_filter
    // script, worth recording rather than leaving as though refusing were
    // simply inherited from SetAirflowMode's CHECK_CURRENT: the old script
    // did not refuse on a wrong screen, it ran goto_menu 0 first and only
    // aborted if THAT failed to reach the status screen. This sequence
    // refuses outright instead, because actively leaving a menu here means
    // LeaveMenu's one Up tap followed by waiting out the unit's own ~2min
    // timeout (sequence.h's LeaveMenu class comment) -- a long, key-pressing
    // preamble in front of the one irreversible write in this component,
    // for a button a human presses deliberately, where a simple retry costs
    // nothing. Refusing fast and asking the human to try again once the
    // display has settled is the smaller risk than spending up to two
    // minutes pressing keys before an operation that cannot be undone.
    case CHECK_STATUS: {
      if (!this->runner_->display().have_frame() ||
          screens::is_menu_screen(this->runner_->display().line1())) {
        if (this->log_.error) {
          // Actionable, not just diagnostic -- this is what a human sees
          // when the button appears to do nothing. The display returns to
          // the status loop on its own (the unit's own menu timeout, or
          // whatever sequence currently has it moves on); press again once
          // it has.
          this->log_.error(
              "ResetFilter: refusing -- display is not on the status screen (line1='" +
              this->runner_->display().line1() +
              "'). The display returns to the status loop on its own -- press this button again once it has.");
        }
        return Poll::FAILED;
      }
      if (this->log_.info) {
        this->log_.info("ResetFilter: holding Up+Down on '" + this->runner_->display().line1() + "'");
      }
      return this->goto_step(HOLD);
    }

    // Fixed 5.5s hold, not a HoldUntil -- same reasoning as
    // SetAirflowMode::hold_main_(): "the timer ran out" is the whole success
    // condition here, there is no predicate to watch the display for.
    // Holding Up and Down together is fine -- the protocol ORs whichever
    // bits are pressed into one shared mask, the same mechanism
    // FetchDiagnostics' own Up+Main entry combo already relies on
    // (mhrv_orig/controls.yaml's own comment, carried forward here).
    case HOLD:
      this->runner_->press(UP | DOWN);  // never Set -- always accepted, see Runner::press()
      if (this->elapsed() < HOLD_MS) {
        return Poll::RUNNING;
      }
      this->runner_->release();
      return this->goto_step(RELEASE_SETTLE);

    // Release, then RELEASE_SETTLE_MS (1000ms -- see its own comment,
    // sequence.h) of silence before FETCH's own Up+Main entry hold.
    case RELEASE_SETTLE:
      if (this->elapsed() < RELEASE_SETTLE_MS) {
        return Poll::RUNNING;
      }
      // Logged, never acted on. This model's manual describes no
      // confirmation prompt -- the hold is documented as the whole
      // procedure -- but some other Vent-Axia models are known to answer
      // this same gesture with a "Reset Filter?" prompt needing a second
      // keypress, and this unit has never actually been observed either
      // way (mhrv_orig/controls.yaml's own comment on this exact line).
      // Firing a guessed confirm press at whatever happens to be on screen
      // would risk pressing a key into a context nobody has ever seen,
      // worse than simply leaving an unconfirmed reset alone -- so if a
      // prompt ever does show up here, this line is where a human first
      // sees it, not a code path that reacts to it.
      if (this->log_.info) {
        this->log_.info("ResetFilter: after hold, line1='" + this->runner_->display().line1() + "' line2='" +
                         this->runner_->display().line2() + "'");
      }
      return this->goto_step(FETCH);

    // The only real confirmation is diagnostic page 23 -- it should have
    // gone from 00000 to the unit's whole service interval. diagnostics_scan_
    // is a DEDICATED FetchDiagnostics instance, not the hub's shared
    // button/schedule one -- see its own comment (sequence.h) for why. A
    // child failure here (could not even reach the diagnostic menu)
    // cascades to FAIL this whole sequence too, same as every other await()
    // in this component -- but by this point the irreversible hold has
    // already happened; only the CONFIRMATION failed, not the reset itself.
    case FETCH:
      return this->await(this->diagnostics_scan_, VERIFY);

    // Four distinguishable outcomes, each logged differently -- mhrv_orig's
    // own three-way log, extended with one more case Opus review (Finding 1)
    // found it needed: a stale-but-nonzero reading (filter_hours_source_
    // answering the same value it did before the hold, e.g. because THIS
    // run's scan missed page 23) must not be reported as a fresh
    // confirmation just because it happens to be nonzero -- see
    // hours_before_'s own comment (sequence.h) for why that particular lie
    // is the one this sequence must not tell. Collapsing any of these four
    // into "pass"/"fail" would hide followup a human needs. Never returns
    // FAILED: the hold already happened in step 2 (HOLD), so there is
    // nothing left here to protect by failing the sequence -- see this
    // class's own comment (sequence.h).
    case VERIFY: {
      const std::optional<int> hours = this->filter_hours_source_ ? this->filter_hours_source_() : std::nullopt;
      if (!hours.has_value()) {
        if (this->log_.warn) {
          this->log_.warn("ResetFilter: no reading from page 23, cannot confirm the reset took");
        }
      } else if (*hours == 0) {
        if (this->log_.warn) {
          this->log_.warn("ResetFilter: still at 0 hours -- the reset did not take");
        }
      } else if (this->hours_before_.has_value() && *hours == *this->hours_before_) {
        // Nonzero, but IDENTICAL to the pre-hold snapshot -- most likely
        // page 23 was not actually re-read by this run's scan, not that the
        // hold silently failed to change a value it should have changed.
        // WARN, not INFO: this is "cannot confirm", spelled out with both
        // numbers so a human can tell it apart from the plain no-reading
        // case above. One case this still cannot resolve, documented rather
        // than hidden (hours_before_'s own comment carries the full
        // reasoning): a reset pressed while the timer already sits at the
        // full interval leaves the reading unchanged even though the hold
        // may genuinely have worked, and that is indistinguishable here
        // from a scan that simply missed the page.
        if (this->log_.warn) {
          this->log_.warn("ResetFilter: reading unchanged at " + std::to_string(*hours) +
                           " hours since before the hold -- likely not re-read this run, cannot confirm");
        }
      } else if (this->log_.info) {
        this->log_.info("ResetFilter: reset confirmed, " + std::to_string(*hours) + " hours to go");
      }
      return this->goto_step(FINISHED);
    }

    case FINISHED:
    default:
      return Poll::DONE;
  }
}

void ResetFilter::on_finish(Poll result) {
  (void) result;
  // Backstop release -- HOLD already releases on its own successful exit,
  // but this guarantees it regardless of which step this run happened to
  // fail or time out on (e.g. the root timeout firing mid-HOLD, or a FAILED
  // CHECK_STATUS that never pressed anything in the first place --
  // release() on an already-idle keypad is always safe, see
  // Runner::release()).
  this->runner_->release();
}

}  // namespace vent_axia
}  // namespace esphome
