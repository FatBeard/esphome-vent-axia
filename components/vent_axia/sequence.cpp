#include "sequence.h"

#include <utility>

#include "screens.h"

namespace esphome {
namespace vent_axia {

namespace {
// Shared by GotoMenu, LeaveMenu and Runner::recover() -- all three issue
// plain menu-navigation taps, as distinct from a sequence-specific hold
// duration (FetchDiagnostics' 8s Down, say). 50ms matches the hub's own
// tap_duration default (vent_axia.h): "one tap = one menu step".
constexpr uint32_t MENU_TAP_MS = 50;
}  // namespace

// ------------------------------------------------------------- Sequence --

Poll Sequence::goto_step(uint8_t s) {
  this->step_ = s;
  this->entered_ = this->runner_->now_ms();
  return Poll::RUNNING;
}

uint32_t Sequence::elapsed() const { return this->runner_->now_ms() - this->entered_; }

Poll Sequence::await(Sequence &child, uint8_t on_ok) {
  if (!this->runner_->push_child_(child, on_ok)) {
    // Stack overflow -- see push_child_()'s comment. Nothing was pushed, so
    // this simply reports the failure synchronously exactly as if the child
    // itself had failed on its very first poll(): the caller (this
    // Sequence's own poll()) returns FAILED, which Runner::loop() then
    // cascades the same way as any other child failure.
    return Poll::FAILED;
  }
  return Poll::RUNNING;
}

// --------------------------------------------------------------- Runner --

bool Runner::request(Sequence &seq) {
  if (this->busy()) {
    if (this->log_.warn) {
      this->log_.warn("sequence: refusing to start " + std::string(seq.name()) + " -- " +
                       std::string(this->running_name()) + " is already running");
    }
    return false;
  }
  if (!this->link_up_) {
    if (this->log_.warn) {
      this->log_.warn("sequence: refusing to start " + std::string(seq.name()) + " -- link is down");
    }
    return false;
  }
  // Order matters: root_started_at_ms_ must be set before push_child_() so
  // the very first loop() tick's timeout check (now_ms - root_started_at_ms_)
  // reads 0, not a stale value from a previous run.
  this->root_started_at_ms_ = this->now_ms_;
  this->push_child_(seq, 0);  // a root has no parent to resume -- resume_step is unused
  return true;
}

void Runner::loop(uint32_t now_ms) {
  this->now_ms_ = now_ms;
  if (this->depth_ == 0) {
    return;  // nothing running
  }

  // The per-root backstop (PLAN.md §2 "Sequence timeout"), measured against
  // the ROOT's own timeout_ms() regardless of which frame is currently on
  // top of the stack: individual steps are expected to carry their own,
  // much shorter timeouts (HoldUntil takes one explicitly) -- this only
  // catches the case where something above failed to, or several short
  // steps together ran long enough to add up to a real problem.
  Sequence *root = this->stack_[0].seq;
  if (now_ms - this->root_started_at_ms_ >= root->timeout_ms()) {
    if (this->log_.error) {
      this->log_.error("sequence: " + std::string(root->name()) + " timed out after " +
                        std::to_string(root->timeout_ms()) + "ms, aborting");
    }
    this->finish_top_(Poll::FAILED);  // cascades through every nested frame -- see its comment
    return;
  }

  Sequence *top = this->stack_[this->depth_ - 1].seq;
  const Poll result = top->poll();
  if (result != Poll::RUNNING) {
    this->finish_top_(result);
  }
}

bool Runner::push_child_(Sequence &child, uint8_t resume_step) {
  if (this->depth_ >= MAX_DEPTH) {
    // Should never happen in practice -- see MAX_DEPTH's comment. Fails
    // loudly and leaves the stack untouched rather than writing past its end.
    if (this->log_.error) {
      this->log_.error("sequence: stack overflow pushing " + std::string(child.name()) + ", aborting run");
    }
    return false;
  }
  child.runner_ = this;
  child.step_ = 0;
  child.entered_ = this->now_ms_;
  this->stack_[this->depth_] = Frame{&child, resume_step};
  this->depth_++;
  child.on_start();
  return true;
}

bool Runner::tap(protocol::KeyMask mask, uint32_t duration_ms) {
  if (this->refuse_if_set_interlocked_(mask)) {
    return false;
  }
  this->keypad_.tap(mask, duration_ms);
  return true;
}

bool Runner::press(protocol::KeyMask mask) {
  if (this->refuse_if_set_interlocked_(mask)) {
    return false;
  }
  this->keypad_.press(mask);
  return true;
}

bool Runner::refuse_if_set_interlocked_(protocol::KeyMask mask) const {
  const bool wants_set = (mask & protocol::key_mask(protocol::Key::SET)) != 0;
  if (!wants_set || this->display_.screen_kind() != screens::ScreenKind::DIAGNOSTIC) {
    return false;
  }
  // Loud on purpose (PLAN.md §7): page 27 ("Reset") writes and has never
  // been tried, so a Set reaching the unit while any diagnostic page is
  // showing is exactly the mistake this exists to catch, not routine
  // operation to log quietly.
  if (this->log_.error) {
    this->log_.error(
        "sequence: refusing Set -- display is on a diagnostic page (page 27, \"Reset\", writes and has never "
        "been tried -- see PLAN.md §7)");
  }
  return true;
}

void Runner::finish_top_(Poll result) {
  const Frame finished = this->stack_[this->depth_ - 1];
  this->depth_--;
  finished.seq->on_finish(result);  // ALWAYS runs -- the one release site, see Sequence::on_finish()

  if (this->depth_ == 0) {
    // The root itself just finished. recover() is the shared abort path,
    // run on any failure however it happened -- a plain poll() FAILED, a
    // failed child that cascaded all the way up, or the timeout above.
    if (result == Poll::FAILED) {
      if (this->on_failure_) {
        this->on_failure_(finished.seq->name());
      }
      this->recover();
    }
    return;
  }

  if (result == Poll::FAILED) {
    // A child's failure is not survivable for its parent: the parent never
    // gets polled again, its own on_finish(FAILED) fires immediately via
    // this recursive call, and so on up the stack until the root is
    // reached. Bounded by MAX_DEPTH, so this can never run away.
    this->finish_top_(Poll::FAILED);
    return;
  }

  // Success: resume the parent at the step it named in its await() call,
  // starting fresh on the very next tick -- same bookkeeping goto_step()
  // uses, so elapsed() reads correctly from here.
  Sequence *parent = this->stack_[this->depth_ - 1].seq;
  parent->step_ = finished.resume_step;
  parent->entered_ = this->now_ms_;
}

void Runner::recover() {
  // Shared abort path (PLAN.md §2): the one place guaranteed to run after
  // ANY root sequence fails, however it failed, so this is the backstop for
  // "must release the keypad on every exit path". Individual primitives
  // already release whatever they themselves asserted (HoldUntil::on_finish
  // in particular) -- this exists for anything they didn't get the chance
  // to, and costs nothing to call redundantly (release() with nothing
  // asserted is a no-op, keypad.h).
  this->keypad_.release();

  if (!screens::is_menu_screen(this->display_.line1())) {
    return;  // already back on the status loop -- nothing to walk out of
  }

  // The VERIFIED exit gesture, not "mash Up": PLAN.md is explicit that a
  // second Up press while an editor is still open silently adjusts the
  // value under the cursor (observed on the real unit: a 14°C setpoint
  // walked to 19°C). Exactly one tap is issued here, then this returns --
  // there is no sequence left on the stack to wait on the unit's own
  // ~2-minute menu timeout closing whatever was open, and nothing here
  // needs to block on that: every sequence that navigates (GotoMenu
  // especially) is written to work correctly from an unknown starting
  // screen, so the next request() does not need to wait for this unwind to
  // finish before it can proceed.
  this->keypad_.tap(protocol::key_mask(protocol::Key::UP), MENU_TAP_MS);
}

// ------------------------------------------------------------------ Tap --

void Tap::on_start() { this->sent_ = this->runner_->tap(this->mask_, this->duration_ms_); }

Poll Tap::poll() {
  if (!this->sent_) {
    return Poll::FAILED;  // refused by the Set interlock -- see Runner::tap()
  }
  return this->runner_->keypad_busy() ? Poll::RUNNING : Poll::DONE;
}

// ------------------------------------------------------------ HoldUntil --

HoldUntil::HoldUntil(protocol::KeyMask mask, Predicate predicate, uint32_t timeout_ms) {
  this->reset(mask, std::move(predicate), timeout_ms);
}

void HoldUntil::reset(protocol::KeyMask mask, Predicate predicate, uint32_t timeout_ms) {
  this->mask_ = mask;
  this->predicate_ = std::move(predicate);
  this->timeout_ms_ = timeout_ms;
}

Poll HoldUntil::poll() {
  // Re-asserted every tick rather than once in on_start() -- see the class
  // comment: press() is a documented no-op when re-asserting the mask it is
  // already holding, so this is safe and makes the hold self-healing.
  if (!this->runner_->press(this->mask_)) {
    return Poll::FAILED;  // refused by the Set interlock -- see Runner::press()
  }
  if (this->predicate_ && this->predicate_()) {
    return Poll::DONE;
  }
  if (this->elapsed() >= this->timeout_ms_) {
    return Poll::FAILED;
  }
  return Poll::RUNNING;
}

void HoldUntil::on_finish(Poll result) {
  (void) result;
  this->runner_->release();  // every exit path -- see the class comment
}

// -------------------------------------------------------------- GotoMenu --

Poll GotoMenu::poll() {
  switch (this->step_) {
    // 5 Up taps is the hard stop at the top of the menu, not a count of "how
    // far up to go": Up past index 0 does nothing, so this always lands on
    // 0 regardless of where the display started (PLAN.md §3) -- deliberately
    // NOT counted relative to an assumed current position. Queued as a
    // batch rather than one-at-a-time: Keypad's own queue enforces
    // key_gap_ms between each tap, so this step is "wait for the queue to
    // drain", not five separate steps.
    case QUEUE_UP:
      for (int i = 0; i < 5; i++) {
        this->runner_->tap(protocol::key_mask(protocol::Key::UP), MENU_TAP_MS);
      }
      return this->goto_step(WAIT_UP);

    case WAIT_UP:
      return this->runner_->keypad_busy() ? Poll::RUNNING : this->goto_step(SETTLE_UP);

    case SETTLE_UP:
      return this->elapsed() >= 500 ? this->goto_step(QUEUE_DOWN) : Poll::RUNNING;

    // index_ == 0 (status) queues nothing here, which is correct: the queue
    // is already empty, so WAIT_DOWN below falls through immediately.
    case QUEUE_DOWN:
      for (uint8_t i = 0; i < this->index_; i++) {
        this->runner_->tap(protocol::key_mask(protocol::Key::DOWN), MENU_TAP_MS);
      }
      return this->goto_step(WAIT_DOWN);

    case WAIT_DOWN:
      return this->runner_->keypad_busy() ? Poll::RUNNING : this->goto_step(SETTLE_DOWN);

    case SETTLE_DOWN:
      return this->elapsed() >= 500 ? Poll::DONE : Poll::RUNNING;

    default:
      return Poll::DONE;
  }
}

// ------------------------------------------------------------- LeaveMenu --

Poll LeaveMenu::poll() {
  switch (this->step_) {
    // Exactly one tap -- see the class comment. Never looped, never retried.
    case TAP:
      this->runner_->tap(protocol::key_mask(protocol::Key::UP), MENU_TAP_MS);
      return this->goto_step(WAIT_TAP);

    case WAIT_TAP:
      return this->runner_->keypad_busy() ? Poll::RUNNING : this->goto_step(CHECK);

    case CHECK:
      return screens::is_menu_screen(this->runner_->display().line1()) ? this->goto_step(WAIT_EXIT) : Poll::DONE;

    // The unit's own menu timeout closes whatever was open, not another Up
    // -- see the class comment for why a second tap is not an option here.
    case WAIT_EXIT:
      if (!screens::is_menu_screen(this->runner_->display().line1())) {
        return Poll::DONE;
      }
      return this->elapsed() >= WAIT_TIMEOUT_MS ? Poll::FAILED : Poll::RUNNING;

    default:
      return Poll::DONE;
  }
}

}  // namespace vent_axia
}  // namespace esphome
