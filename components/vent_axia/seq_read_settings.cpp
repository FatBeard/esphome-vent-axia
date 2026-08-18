#include "sequence.h"

#include "screens.h"

namespace esphome {
namespace vent_axia {

namespace {
constexpr protocol::KeyMask SET = protocol::key_mask(protocol::Key::SET);
}  // namespace

void ReadSettings::on_start() { this->indoor_read_ok_ = false; }

Poll ReadSettings::poll() {
  switch (this->step_) {
    // --- Summer Mode, user menu entry 2 ---
    case NAV_SUMMER:
      this->nav_started_ms_ = this->runner_->now_ms();
      this->goto_menu_.reset(2);
      return this->await(this->goto_menu_, WAIT_SUMMER);

    case WAIT_SUMMER: {
      if (this->runner_->display().screen_kind() == screens::ScreenKind::SUMMER_MODE) {
        const auto v = read_fresh_value(this->runner_->display(), this->nav_started_ms_, parse_summer_mode_field);
        if (v.has_value()) {
          if (this->on_switch_) {
            this->on_switch_(SwitchKey::SUMMER_MODE, *v != 0);
          }
          if (this->log().info) {
            this->log().info(std::string("ReadSettings: Summer Mode is ") + (*v != 0 ? "On" : "Off"));
          }
          return this->goto_step(NAV_INDOOR);
        }
      }
      if (this->elapsed() >= SUMMER_TIMEOUT_MS) {
        if (this->log().warn) {
          this->log().warn("ReadSettings: Summer Mode unreadable (line1='" + this->runner_->display().text_line1() +
                           "')");
        }
        return this->goto_step(NAV_INDOOR);
      }
      return Poll::RUNNING;
    }

    // --- Indoor Temp (the bypass target), user menu entry 3 ---
    case NAV_INDOOR:
      this->nav_started_ms_ = this->runner_->now_ms();
      this->goto_menu_.reset(3);
      return this->await(this->goto_menu_, WAIT_INDOOR);

    case WAIT_INDOOR: {
      if (this->runner_->display().screen_kind() == screens::ScreenKind::INDOOR_TEMP) {
        const auto v = read_fresh_value(this->runner_->display(), this->nav_started_ms_, parse_temp_field);
        if (v.has_value()) {
          if (this->on_number_) {
            this->on_number_(NumberKey::BYPASS_INDOOR_TEMP, *v);
          }
          if (this->log().info) {
            this->log().info("ReadSettings: Bypass minimum indoor temperature is " + std::to_string(*v) + " C");
          }
          this->indoor_read_ok_ = true;
          return this->goto_step(OPEN_OUTDOOR);
        }
      }
      if (this->elapsed() >= INDOOR_TIMEOUT_MS) {
        if (this->log().warn) {
          this->log().warn("ReadSettings: Indoor Temp unreadable (line1='" + this->runner_->display().text_line1() +
                           "')");
        }
        // indoor_read_ok_ stays false -- the outdoor hop is skipped below.
        return this->goto_step(OPEN_OUTDOOR);
      }
      return Poll::RUNNING;
    }

    // --- Outdoor Temp, off the end of the menu: only reachable through
    // Indoor Temp's editor, and only attempted if Indoor Temp read cleanly
    // above -- see indoor_read_ok_'s comment. Skipping straight to HOME here
    // is safe because no editor has been opened yet on this path.
    case OPEN_OUTDOOR:
      if (!this->indoor_read_ok_) {
        return this->goto_step(HOME);
      }
      return this->await(this->open_editor_, HOP_COMMIT);
      // OpenEditor's own failure (Set never opened anything, after one
      // retry) cascades straight to on_finish(FAILED) without reaching
      // EXIT_CHAIN -- correct, because nothing opened means there is
      // nothing to walk out of.

    case HOP_COMMIT:
      // Commits Indoor Temp's value UNTOUCHED (nothing has adjusted it) and
      // steps the chain onto Outdoor Temp -- see PLAN.md's editing model.
      return this->tap_then_(SET, WAIT_OUTDOOR_SCREEN);

    // Ordering deliberately inverted from every other screen above: the
    // screen being LEFT is an open editor, which republishes every ~350ms,
    // not a settled one -- see PLAN.md "The one place ordering still
    // matters". Waiting for line1 to say Outdoor Temp FIRST, and only then
    // arming the fresh-value wait below, is what stops Indoor Temp's still-
    // blinking value from being read as Outdoor Temp's.
    case WAIT_OUTDOOR_SCREEN:
      if (this->runner_->display().screen_kind() == screens::ScreenKind::OUTDOOR_TEMP) {
        this->nav_started_ms_ = this->runner_->now_ms();
        return this->goto_step(WAIT_OUTDOOR_VALUE);
      }
      if (this->elapsed() >= OUTDOOR_SCREEN_TIMEOUT_MS) {
        // Did not land on Outdoor Temp -- the chain's shape is not
        // guaranteed (PLAN.md: a commit has been observed closing the
        // editor outright rather than advancing). Not a reason to skip
        // ExitEditChain: an editor may still be open on whatever screen
        // this is, so the funnel step runs regardless -- see class comment.
        if (this->log().warn) {
          this->log().warn("ReadSettings: did not land on Outdoor Temp after the Indoor Temp hop (line1='" +
                           this->runner_->display().text_line1() + "')");
        }
        return this->goto_step(EXIT_CHAIN);
      }
      return Poll::RUNNING;

    case WAIT_OUTDOOR_VALUE: {
      const auto v = read_fresh_value(this->runner_->display(), this->nav_started_ms_, parse_temp_field);
      if (v.has_value()) {
        if (this->on_number_) {
          this->on_number_(NumberKey::BYPASS_OUTDOOR_TEMP, *v);
        }
        if (this->log().info) {
          this->log().info("ReadSettings: Bypass minimum outdoor temperature is " + std::to_string(*v) + " C");
        }
        return this->goto_step(EXIT_CHAIN);
      }
      if (this->elapsed() >= OUTDOOR_VALUE_TIMEOUT_MS) {
        if (this->log().warn) {
          this->log().warn("ReadSettings: Outdoor Temp value unreadable");
        }
        return this->goto_step(EXIT_CHAIN);
      }
      return Poll::RUNNING;
    }

    // Reached on both outcomes of the hop -- see class comment.
    case EXIT_CHAIN:
      return this->await(this->exit_chain_, HOME);

    case HOME:
      this->goto_menu_.reset(0);
      return this->await(this->goto_menu_, FINISHED);

    case FINISHED:
      return Poll::DONE;

    // step_ somehow outside the Step enum -- a bug, not a legitimate landing
    // state (every real step above has its own explicit case, including
    // FINISHED). Previously fell through to the same `return Poll::DONE;` as
    // FINISHED, reporting success for a read that never actually ran to
    // completion; FAILED routes through Runner::recover() instead.
    default:
      if (this->log().error) {
        this->log().error("ReadSettings: invalid step " + std::to_string(static_cast<int>(this->step_)));
      }
      return Poll::FAILED;
  }
}

void ReadSettings::on_finish(Poll result) {
  (void) result;
  // Backstop release -- every primitive above already releases whatever it
  // itself asserted (it only ever taps), same reasoning as
  // FetchDiagnostics::on_finish().
  this->runner_->release();
}

}  // namespace vent_axia
}  // namespace esphome
