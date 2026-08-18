#include "sequence.h"

#include <utility>

#include "parser.h"
#include "screens.h"

namespace esphome {
namespace vent_axia {

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

const Keypad::LogSink &Sequence::log() const { return this->runner_->log(); }

Poll Sequence::tap_then_(protocol::KeyMask mask, uint8_t next_step) {
  // this->runner_->tap_duration_ms(), NOT a hardcoded duration -- see this
  // method's own header comment and Runner::tap_duration_ms()'s: every
  // sequence tap goes through here (or one of the handful of batch-tap
  // sites this helper does not fit, see sequence.h's Tap-removal comment)
  // so raising tap_duration reaches all of them, not just the four manual
  // key buttons.
  if (!this->runner_->tap(mask, this->runner_->tap_duration_ms())) {
    return Poll::FAILED;  // refused by the Set interlock -- see Runner::tap()
  }
  this->tap_resume_step_ = next_step;
  return this->goto_step(WAIT_TAP_STEP);
}

Poll Sequence::pump() {
  if (this->step_ == WAIT_TAP_STEP) {
    return this->runner_->keypad_busy() ? Poll::RUNNING : this->goto_step(this->tap_resume_step_);
  }
  return this->poll();
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

  // Mid-run link loss (PLAN.md §7 "Link loss"). request() only refuses to
  // START a run while the link is down (see its own comment) -- nothing
  // rechecks link_up_ once depth_ > 0, so a drop partway through was
  // previously only ever caught by the ROOT timeout above running all the
  // way out (up to 450s for SyncClock, 480s for WriteSetting). By then the
  // display has stopped updating too, so every predicate that reads it has
  // stalled -- AdjustField::CHECK in particular: its guard_count_ only
  // increments after a SUCCESSFUL parse (sequence.cpp's own AdjustField::
  // poll(), step 3), so a display that has frozen mid-run never advances it
  // either, and the run just keeps queueing taps at a unit that is not
  // listening until the root timeout finally does the job this check now
  // does much sooner. No extra debounce needed here: the hub computes
  // link_up as `have_frame_ && (now - last_frame_at_ms_) < LINK_TIMEOUT_MS`
  // (30s), so link_up_ going false already means a full 30s of total
  // silence, not a blip.
  if (!this->link_up_) {
    if (this->log_.error) {
      this->log_.error("sequence: " + std::string(root->name()) + " aborting -- link dropped mid-run");
    }
    this->finish_top_(Poll::FAILED);  // cascades through every nested frame -- see its comment
    return;
  }

  Sequence *top = this->stack_[this->depth_ - 1].seq;
  const Poll result = top->pump();
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

  if (!screens::is_menu_screen(this->display_.raw_line1())) {
    return;  // already back on the status loop -- nothing to walk out of
  }

  // If an editor is still open, do NOTHING further. Up inside an editor
  // adjusts the value under the cursor rather than navigating -- that is the
  // 14 C walked to 19 C failure, and it applies to the FIRST Up just as much
  // as to any later one. Only Set is safe inside an editor, and pressing Set
  // here would commit whatever half-finished value the failed sequence left
  // behind, which is worse than doing nothing.
  //
  // Doing nothing is the right answer: the unit closes an abandoned editor
  // itself after ~2 minutes and commits nothing when it does, so a failed
  // write leaves the setting untouched -- exactly what a failed write should
  // do. This path became reachable in stage 6, where a guard-exhausted
  // AdjustField can abandon an open editor.
  if (this->display_.editor_open(this->now_ms_)) {
    if (this->log_.warn) {
      this->log_.warn("recover: editor still open, leaving it to the unit's own ~2min timeout -- pressing "
                      "anything here would either adjust or commit a half-finished value");
    }
    return;
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
  this->keypad_.tap(protocol::key_mask(protocol::Key::UP), this->tap_duration_ms());
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
        this->runner_->tap(protocol::key_mask(protocol::Key::UP), this->runner_->tap_duration_ms());
      }
      return this->goto_step(WAIT_UP);

    case WAIT_UP:
      return this->runner_->keypad_busy() ? Poll::RUNNING : this->goto_step(SETTLE_UP);

    case SETTLE_UP:
      return this->elapsed() >= SETTLE_MS ? this->goto_step(QUEUE_DOWN) : Poll::RUNNING;

    // index_ == 0 (status) queues nothing here, which is correct: the queue
    // is already empty, so WAIT_DOWN below falls through immediately.
    case QUEUE_DOWN:
      for (uint8_t i = 0; i < this->index_; i++) {
        this->runner_->tap(protocol::key_mask(protocol::Key::DOWN), this->runner_->tap_duration_ms());
      }
      return this->goto_step(WAIT_DOWN);

    case WAIT_DOWN:
      return this->runner_->keypad_busy() ? Poll::RUNNING : this->goto_step(SETTLE_DOWN);

    case SETTLE_DOWN:
      return this->elapsed() >= SETTLE_MS ? Poll::DONE : Poll::RUNNING;

    // step_ somehow outside the Step enum -- a bug, not a legitimate landing
    // state (every real step above, QUEUE_UP through SETTLE_DOWN, has its own
    // explicit case; SETTLE_DOWN itself returns DONE directly rather than
    // falling through to a FINISHED case). Previously returned DONE here,
    // reporting a menu navigation as landed when it never actually ran to
    // completion; FAILED routes through Runner::recover() instead.
    default:
      if (this->log().error) {
        this->log().error("GotoMenu: invalid step " + std::to_string(static_cast<int>(this->step_)));
      }
      return Poll::FAILED;
  }
}

// ------------------------------------------------------------- LeaveMenu --

Poll LeaveMenu::poll() {
  switch (this->step_) {
    // At most one tap -- see the class comment. If an editor is still open
    // here (most likely a dropped commit Set upstream), Up would adjust the
    // field under the cursor instead of navigating out -- the same trap
    // Runner::recover() has guarded against since stage 6 (PLAN.md §3's
    // 14C->19C observation). Skip straight to WAIT_EXIT and let the unit's
    // own ~2-minute timeout close the editor without touching it.
    case TAP:
      if (this->runner_->display().editor_open(this->runner_->now_ms())) {
        return this->goto_step(WAIT_EXIT);
      }
      return this->tap_then_(protocol::key_mask(protocol::Key::UP), CHECK);

    case CHECK:
      return screens::is_menu_screen(this->runner_->display().raw_line1()) ? this->goto_step(WAIT_EXIT) : Poll::DONE;

    // The unit's own menu timeout closes whatever was open, not another Up
    // -- see the class comment for why a second tap is not an option here.
    case WAIT_EXIT:
      if (!screens::is_menu_screen(this->runner_->display().raw_line1())) {
        return Poll::DONE;
      }
      return this->elapsed() >= WAIT_TIMEOUT_MS ? Poll::FAILED : Poll::RUNNING;

    // step_ somehow outside the Step enum -- a bug, not a legitimate landing
    // state (TAP, CHECK and WAIT_EXIT above are every real step, and each has
    // its own explicit case). Previously returned DONE here, reporting the
    // menu as successfully left when it never actually was; FAILED routes
    // through Runner::recover() instead.
    default:
      if (this->log().error) {
        this->log().error("LeaveMenu: invalid step " + std::to_string(static_cast<int>(this->step_)));
      }
      return Poll::FAILED;
  }
}

// ---------------------------------------------------------- editing model --

void OpenEditor::on_start() { this->attempt_ = 0; }

Poll OpenEditor::poll() {
  switch (this->step_) {
    case TAP:
      return this->tap_then_(protocol::key_mask(protocol::Key::SET), SETTLE);

    case SETTLE:
      return this->elapsed() >= SETTLE_MS ? this->goto_step(CHECK) : Poll::RUNNING;

    case CHECK:
      if (this->runner_->display().editor_open(this->runner_->now_ms())) {
        return Poll::DONE;
      }
      this->attempt_++;
      // Exactly one retry -- see class comment. MAX_ATTEMPTS is 2, so this
      // fires once (attempt_ goes 0 -> 1 -> retry) and fails on the second
      // miss.
      return this->attempt_ >= MAX_ATTEMPTS ? Poll::FAILED : this->goto_step(TAP);

    // step_ somehow outside the Step enum -- a bug, not a legitimate landing
    // state (TAP, SETTLE and CHECK above are every real step, and each has
    // its own explicit case). Previously returned DONE here, reporting an
    // editor as confirmed open when it never actually was checked -- exactly
    // the "a dropped Set... those same presses are navigation instead"
    // failure OpenEditor exists to catch, just reached a different way;
    // FAILED routes through Runner::recover() instead.
    default:
      if (this->log().error) {
        this->log().error("OpenEditor: invalid step " + std::to_string(static_cast<int>(this->step_)));
      }
      return Poll::FAILED;
  }
}

void AdjustField::reset(ValueParser parse, DirectionFn direction, int target, int guard_limit) {
  // The fixed-target form: a TargetFn that always returns the same value, so
  // CHECK below has exactly one implementation for both this and the
  // live-target overload -- see TargetFn's own comment.
  this->reset(
      parse, direction,
      [target](int &out) {
        out = target;
        return true;
      },
      guard_limit);
}

void AdjustField::reset(ValueParser parse, DirectionFn direction, TargetFn target, int guard_limit) {
  this->parse_ = parse;
  this->direction_ = direction;
  this->target_ = std::move(target);
  this->guard_limit_ = guard_limit;
}

void AdjustField::on_start() { this->guard_count_ = 0; }

Poll AdjustField::poll() {
  switch (this->step_) {
    case CHECK: {
      // Step 1: the guard, not a timeout, is what bounds this loop -- see
      // the class comment. Reaching it means either the field genuinely
      // cannot get to the target (PLAN.md risk 6: Outdoor Temp's guessed
      // range rejecting a value looks identical to a dropped press from
      // here) or something is very wrong; either way the caller still needs
      // to walk the editor out afterwards, same as every other failure in
      // this component.
      if (this->guard_count_ >= this->guard_limit_) {
        return Poll::FAILED;
      }
      // Step 2: re-read the target fresh -- see TargetFn's own comment for
      // why this matters (stage 7's clock fields) and why it can never fail
      // for the fixed-target reset() overload. FAILED, not a silent stall,
      // if it is genuinely unavailable: there is nothing to adjust towards.
      int want = 0;
      if (!this->target_ || !this->target_(want)) {
        if (this->log().error) {
          this->log().error("AdjustField: target unavailable -- aborting rather than adjusting towards an unknown "
                             "value");
        }
        return Poll::FAILED;
      }
      // Step 3: NOT an error if this fails to parse -- see class comment.
      int cur = 0;
      if (!this->parse_(this->runner_->display().raw_line2(), cur)) {
        return Poll::RUNNING;
      }
      // Step 4.
      if (cur == want) {
        return Poll::DONE;
      }
      // Step 5.
      this->guard_count_++;
      const bool up = this->direction_(cur, want);
      const protocol::KeyMask mask =
          up ? protocol::key_mask(protocol::Key::UP) : protocol::key_mask(protocol::Key::DOWN);
      // never Set -- always accepted, see Runner::tap(). Not tap_then_(): the
      // wait below is for line2 to actually change (or CHANGE_TIMEOUT_MS),
      // not merely for the keypad to go idle again.
      this->runner_->tap(mask, this->runner_->tap_duration_ms());
      return this->goto_step(WAIT_CHANGE);
    }

    // Step 6: never fires the next tap blind. Loops back to CHECK once line2
    // has actually changed since the tap above, or after CHANGE_TIMEOUT_MS
    // regardless -- the guard in CHECK is what stops a genuinely stuck field
    // from looping forever, not this per-tap timeout.
    case WAIT_CHANGE:
      if (this->runner_->display().line2_changed_at_ms() > this->entered_) {
        return this->goto_step(CHECK);
      }
      return this->elapsed() >= CHANGE_TIMEOUT_MS ? this->goto_step(CHECK) : Poll::RUNNING;

    // step_ somehow outside the Step enum -- a bug, not a legitimate landing
    // state (CHECK and WAIT_CHANGE above are every real step, and each has
    // its own explicit case). Previously returned DONE here, reporting a
    // field as adjusted to its target when it was never actually confirmed;
    // FAILED routes through Runner::recover() instead.
    default:
      if (this->log().error) {
        this->log().error("AdjustField: invalid step " + std::to_string(static_cast<int>(this->step_)));
      }
      return Poll::FAILED;
  }
}

void ExitEditChain::on_start() { this->commits_ = 0; }

Poll ExitEditChain::poll() {
  switch (this->step_) {
    case CHECK:
      if (!this->runner_->display().editor_open(this->runner_->now_ms())) {
        return Poll::DONE;  // the common case: the chain has already been walked off its end
      }
      if (this->commits_ >= MAX_COMMITS) {
        if (this->log().warn) {
          this->log().warn("sequence: editor still open after " + std::to_string(static_cast<int>(MAX_COMMITS)) +
                           " commits -- waiting out the unit's own ~2-minute timeout");
        }
        return this->goto_step(WAIT_TIMEOUT);
      }
      this->commits_++;
      // Set only -- see class comment. This is the one place in the whole
      // component that commits an edit-in-progress on purpose.
      return this->tap_then_(protocol::key_mask(protocol::Key::SET), SETTLE);

    case SETTLE:
      // Longer than editor_open()'s own settle_ms_ (1200ms default) so the
      // next CHECK's answer actually means something -- see class comment.
      return this->elapsed() >= COMMIT_SETTLE_MS ? this->goto_step(CHECK) : Poll::RUNNING;

    case WAIT_TIMEOUT:
      if (!this->runner_->display().editor_open(this->runner_->now_ms())) {
        return Poll::DONE;  // the unit's own timeout closed it, as documented
      }
      // See class comment for why this is FAILED, not a silent DONE:
      // cascading through Runner::recover()'s single verified Up tap is
      // safer than letting whatever runs next press Up unconditionally.
      return this->elapsed() >= FALLBACK_TIMEOUT_MS ? Poll::FAILED : Poll::RUNNING;

    // step_ somehow outside the Step enum -- a bug, not a legitimate landing
    // state (CHECK, SETTLE and WAIT_TIMEOUT above are every real step, and
    // each has its own explicit case). Previously returned DONE here,
    // reporting the edit chain as walked closed when it was never actually
    // confirmed; FAILED routes through Runner::recover() instead, same
    // reasoning as WAIT_TIMEOUT's own FAILED above.
    default:
      if (this->log().error) {
        this->log().error("ExitEditChain: invalid step " + std::to_string(static_cast<int>(this->step_)));
      }
      return Poll::FAILED;
  }
}

// ------------------------------------------------------- settings fields --

bool parse_summer_mode_field(const std::string &line2, int &out) {
  bool on = false;
  if (!parser::parse_on_off(line2, on)) {
    return false;
  }
  out = on ? 1 : 0;
  return true;
}

bool parse_temp_field(const std::string &line2, int &out) { return parser::parse_field(line2, 0, 2, out); }

bool direction_no_wrap(int cur, int want) { return cur < want; }

// ------------------------------------------------------------ clock fields --

bool parse_clock_day_field(const std::string &line2, int &out) {
  // clock_rendered() first -- clock_day()'s own comment assumes it already
  // passed, so calling it on a mid-blink frame (the day blanked, e.g.
  // "    23:49") would read garbage rather than correctly failing.
  if (!parser::clock_rendered(line2)) {
    return false;
  }
  const int day = parser::clock_day(line2);
  if (day < 0) {
    return false;  // clock_rendered() passed but the three letters weren't a day name -- defensive, not expected
  }
  out = day;
  return true;
}

bool parse_clock_hour_field(const std::string &line2, int &out) {
  if (!parser::clock_rendered(line2)) {
    return false;
  }
  out = parser::clock_hour(line2);
  return true;
}

bool parse_clock_minute_field(const std::string &line2, int &out) {
  if (!parser::clock_rendered(line2)) {
    return false;
  }
  out = parser::clock_minute(line2);
  return true;
}

bool direction_wrap_24(int cur, int want) { return parser::wrapped_delta(cur, want, 24) > 0; }

bool direction_wrap_60(int cur, int want) { return parser::wrapped_delta(cur, want, 60) > 0; }

std::optional<int> read_fresh_value(const Display &display, uint32_t since_ms, AdjustField::ValueParser parse) {
  if (display.line2_changed_at_ms() <= since_ms) {
    return std::nullopt;
  }
  int v = 0;
  if (!parse(display.raw_line2(), v)) {
    return std::nullopt;
  }
  return v;
}

}  // namespace vent_axia
}  // namespace esphome
