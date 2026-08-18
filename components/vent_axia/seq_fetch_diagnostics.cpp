#include "sequence.h"

#include "screens.h"

namespace esphome {
namespace vent_axia {

namespace {
constexpr protocol::KeyMask UP = protocol::key_mask(protocol::Key::UP);
constexpr protocol::KeyMask DOWN = protocol::key_mask(protocol::Key::DOWN);
constexpr protocol::KeyMask MAIN = protocol::key_mask(protocol::Key::MAIN);
}  // namespace

void FetchDiagnostics::on_start() {
  // A long-lived hub member, run once a day off mhrv.yaml's 04:30 schedule
  // or on demand from the button -- both the page-tracking state below and
  // hold_'s configuration must start clean each run, not carry over
  // whatever the previous run left behind.
  this->highest_page_seen_ = -1;
  this->seen_pages_.fill(false);
}

bool FetchDiagnostics::at_page_00_() const {
  const auto page = screens::diagnostic_page(this->runner_->display().raw_line1());
  return page.has_value() && *page == 0;
}

void FetchDiagnostics::track_page_() {
  // Entirely passive, and it stays that way here too: decode_page()
  // (diagnostics.cpp), wired up by the hub's own on_change callback, is what
  // actually publishes each page's fields as it goes by. This only reads
  // line1 to know which page number is currently showing -- never line2,
  // and never any decoding -- purely to report what was captured when this
  // run finishes (on_finish() below).
  const auto page = screens::diagnostic_page(this->runner_->display().raw_line1());
  if (!page.has_value()) {
    return;  // mid-transition, or not on a diagnostic page yet -- not an error
  }
  if (*page >= 0 && static_cast<size_t>(*page) < this->seen_pages_.size()) {
    this->seen_pages_[static_cast<size_t>(*page)] = true;
  }
  if (*page > this->highest_page_seen_) {
    this->highest_page_seen_ = *page;
  }
}

int FetchDiagnostics::count_seen_pages_() const {
  int count = 0;
  for (const bool seen : this->seen_pages_) {
    if (seen) {
      count++;
    }
  }
  return count;
}

Poll FetchDiagnostics::poll() {
  this->track_page_();

  switch (this->step_) {
    // 1: hold Up+Main until the display actually shows a diagnostic page --
    // the unit's entry combo for the diagnostic menu. Timeout 15s.
    case ENTER:
      this->hold_.reset(
          UP | MAIN, [this] { return screens::is_diagnostic_screen(this->runner_->display().raw_line1()); },
          ENTER_TIMEOUT_MS);
      return this->await(this->hold_, RELEASE_ENTER);

    // 2: release (HoldUntil::on_finish just did it), settle 300ms before the
    // Down hold below -- the unit needs a moment after the entry combo.
    case RELEASE_ENTER:
      return this->elapsed() >= ENTER_SETTLE_MS ? this->goto_step(HOLD_DOWN) : Poll::RUNNING;

    // 3: hold Down for a fixed 8s. Deliberately NOT a HoldUntil: the unit's
    // own auto-repeat walks the whole menu in ~2s on its own, so 8s
    // comfortably covers every page, and each one publishes as it passes
    // through the passive decode already built in stage 3 (see
    // track_page_() -- this sequence does not decode anything itself). A
    // HoldUntil's timeout means FAILED; here running out the clock is the
    // normal, successful outcome, so it does not fit that primitive.
    case HOLD_DOWN:
      this->runner_->press(DOWN);  // never Set -- always accepted, see Runner::press()
      if (this->elapsed() < HOLD_DOWN_MS) {
        return Poll::RUNNING;
      }
      this->runner_->release();
      return this->goto_step(RELEASE_DOWN);

    // 4: release, and actually wait out a release before holding Up.
    //
    // This settle is load-bearing, and its absence is easy to miss because
    // the code reads as if release() takes effect at once. It does not: the
    // hub runs keypad_.loop() *before* runner_.loop(), so going straight from
    // this step into an Up hold would leave only a single ~16ms tick of
    // silence between the two masks. A release is *only* silence on this
    // protocol -- there is no key-up frame -- and 16ms is far below what the
    // unit needs to see one, which is why key_gap is 400ms. The unit would
    // read Down-then-Up as one unbroken press, never register the Up, and
    // the page-00 hold below would time out with the display abandoned in the
    // diagnostic menu: precisely the old component's failure.
    //
    // Any future sequence moving directly from one hold to another needs the
    // same treatment; taps get their gap for free from the keypad, holds do
    // not.
    case RELEASE_DOWN:
      if (this->elapsed() < DOWN_SETTLE_MS) {
        return Poll::RUNNING;
      }
      // 5: hold Up until page 00 comes round.
      this->hold_.reset(
          UP, [this] { return this->at_page_00_(); }, TO_PAGE_00_TIMEOUT_MS);
      return this->await(this->hold_, RELEASE_AT_00);

    // 6: release (HoldUntil::on_finish again), then settle 250ms before a
    // FRESH hold of Up.
    //
    // ESSENTIAL, and non-obvious enough to have cost a debugging round:
    // holding Up straight through from page 00 NEVER exits the diagnostic
    // menu -- 17s was measured parked there with no effect at all. Only a
    // fresh press, asserted after a release and this short settle, drops
    // out (in ~3s). Do not "simplify" steps 5-7 into one continuous hold;
    // that is the one thing that has already been tried and does not work.
    case RELEASE_AT_00:
      return this->elapsed() >= PAGE_00_SETTLE_MS ? this->goto_step(EXIT) : Poll::RUNNING;

    // 7: the fresh hold, per the comment above -- until the display leaves
    // the diagnostic menu entirely (line1 no longer starts "Diagnostic"),
    // never a hardcoded terminator page. The old component waited for the
    // literal string "Diagnostic  28", which does not exist on firmware
    // V32/05 (it stops at 27), so it timed out after 60s and abandoned the
    // display in the menu -- see on_finish() for how the real highest page
    // actually seen is reported instead. Timeout 15s.
    case EXIT:
      this->hold_.reset(
          UP, [this] { return !screens::is_diagnostic_screen(this->runner_->display().raw_line1()); }, EXIT_TIMEOUT_MS);
      return this->await(this->hold_, FINISHED);

    default:
      return Poll::DONE;
  }
}

void FetchDiagnostics::on_finish(Poll result) {
  // Backstop release -- every hold already released itself on its own exit
  // (HoldUntil::on_finish, and the explicit release() after the fixed Down
  // hold above), but this guarantees it regardless of which step this run
  // happened to fail or time out on.
  this->runner_->release();

  if (result != Poll::DONE) {
    return;  // Runner::recover() (the shared abort path) handles the rest
  }

  const int of = this->highest_page_seen_ + 1;
  const int captured = this->count_seen_pages_();
  if (this->log_.info) {
    // "of" is derived from the highest page number actually seen this run,
    // never a hardcoded constant that could go stale on a firmware update
    // (PLAN.md §3/§7 -- see the class comment). A mismatch between the two
    // numbers, e.g. "26 of 28", means a page was skipped mid-scroll, which
    // is worth knowing and would otherwise be completely invisible.
    this->log_.info("FetchDiagnostics: captured " + std::to_string(captured) + " of " + std::to_string(of) +
                     " pages (highest " + std::to_string(this->highest_page_seen_) + ")");
  }
  if (this->on_success_) {
    this->on_success_();
  }
}

}  // namespace vent_axia
}  // namespace esphome
