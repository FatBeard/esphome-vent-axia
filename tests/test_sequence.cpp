#include "test_framework.h"

#include <functional>
#include <string>
#include <vector>

#include "frame_test_helper.h"
#include "sequence.h"
#include "sequence_test_helpers.h"

using namespace esphome::vent_axia;
using namespace vatest;

namespace {

/// A Sequence whose behaviour is supplied by the test as std::functions,
/// for exercising the ENGINE (Runner's stack/timeout/propagation) without
/// needing a real primitive. Exposes protected Sequence machinery (await(),
/// goto_step(), elapsed(), the current step, and runner_-mediated keypad
/// access) through public do_*() wrappers so a test's lambda -- which is
/// not a Sequence member function and so cannot reach protected members
/// itself -- can still drive them via the `self` reference poll() passes in.
class ScriptedSequence final : public Sequence {
 public:
  const char *name() const override { return this->name_; }

  Poll poll() override {
    this->poll_count++;
    return this->on_poll ? this->on_poll(*this) : Poll::DONE;
  }
  void on_start() override {
    this->start_count++;
    if (this->on_start_hook) {
      this->on_start_hook();
    }
  }
  void on_finish(Poll result) override {
    this->finish_count++;
    this->last_result = result;
    if (this->on_finish_hook) {
      this->on_finish_hook(result);
    }
  }
  uint32_t timeout_ms() const override { return this->timeout_ms_; }

  Poll do_await(Sequence &child, uint8_t on_ok) { return this->await(child, on_ok); }
  Poll do_goto(uint8_t s) { return this->goto_step(s); }
  uint32_t do_elapsed() const { return this->elapsed(); }
  uint8_t current_step() const { return this->step_; }
  bool do_press(protocol::KeyMask mask) { return this->runner_->press(mask); }

  const char *name_{"Scripted"};
  std::function<Poll(ScriptedSequence &)> on_poll;
  std::function<void()> on_start_hook;
  std::function<void(Poll)> on_finish_hook;
  int start_count{0};
  int poll_count{0};
  int finish_count{0};
  Poll last_result{Poll::RUNNING};
  uint32_t timeout_ms_{120000};
};

}  // namespace

// ================================================================ Runner --
// The engine itself: stack, timeout, propagation, the fixed depth, request()'s
// two refusals, and the Set interlock now enforced here (PLAN.md §7 -- moved
// from vent_axia.cpp so it is testable at all, see sequence.h's comment on
// Runner::tap()).

TEST_CASE(a_root_sequence_runs_to_completion_and_on_finish_sees_done) {
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);

  ScriptedSequence seq;
  seq.on_poll = [](ScriptedSequence &) { return Poll::DONE; };

  CHECK(runner.request(seq));
  CHECK_EQ(seq.start_count, 1);

  Clock clock{kp, runner};
  clock.tick();

  CHECK_EQ(seq.poll_count, 1);
  CHECK_EQ(seq.finish_count, 1);
  CHECK(seq.last_result == Poll::DONE);
  CHECK(!runner.busy());
}

TEST_CASE(a_failing_child_propagates_failed_to_its_parent_which_is_never_polled_again) {
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);

  ScriptedSequence child;
  child.name_ = "Child";
  child.on_poll = [](ScriptedSequence &) { return Poll::FAILED; };

  ScriptedSequence parent;
  parent.name_ = "Parent";
  parent.on_poll = [&](ScriptedSequence &self) {
    // Only ever reachable at step 0 -- if this fires a second time (i.e. the
    // parent got resumed at step 1 despite the child failing) the test below
    // catches it via poll_count.
    if (self.current_step() == 0) {
      return self.do_await(child, 1);
    }
    return Poll::DONE;
  };

  CHECK(runner.request(parent));

  Clock clock{kp, runner};
  clock.tick();  // parent's poll(): awaits child, pushes it
  clock.tick();  // child's poll(): fails

  CHECK_EQ(child.finish_count, 1);
  CHECK(child.last_result == Poll::FAILED);
  CHECK_EQ(parent.poll_count, 1);  // never polled again -- see the comment above
  CHECK_EQ(parent.finish_count, 1);
  CHECK(parent.last_result == Poll::FAILED);
  CHECK(!runner.busy());
}

TEST_CASE(on_finish_runs_on_a_timeout_too_and_recover_releases_the_key) {
  // Rounds out "on_finish runs on every exit path": success is the first
  // test above, a child's failure cascading is the second, this is the
  // third -- a root that never finishes on its own, caught by Runner's
  // per-root backstop (PLAN.md §2 "Sequence timeout"), not by anything the
  // sequence itself does.
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);

  ScriptedSequence root;
  root.timeout_ms_ = 1000;
  root.on_poll = [](ScriptedSequence &self) {
    self.do_press(UP);  // holds forever, on purpose -- this sequence never finishes itself
    return Poll::RUNNING;
  };

  CHECK(runner.request(root));

  Clock clock{kp, runner};
  clock.advance(980);  // still under the 1000ms budget
  CHECK_EQ(root.finish_count, 0);

  clock.tick();  // crosses 1000ms -- the backstop fires
  CHECK_EQ(root.finish_count, 1);
  CHECK(root.last_result == Poll::FAILED);
  CHECK(!runner.busy());

  clock.tick();  // recover()'s release() takes effect on the following loop() tick
  CHECK(!kp.busy());
}

TEST_CASE(request_refuses_a_second_root_while_one_is_running) {
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);

  ScriptedSequence first;
  first.name_ = "First";
  first.on_poll = [](ScriptedSequence &) { return Poll::RUNNING; };  // never finishes in this test
  ScriptedSequence second;
  second.name_ = "Second";

  CHECK(runner.request(first));
  CHECK(runner.busy());
  CHECK(std::string(runner.running_name()) == "First");

  CHECK(!runner.request(second));
  CHECK_EQ(second.start_count, 0);  // never touched at all, not queued either
  CHECK(std::string(runner.running_name()) == "First");  // unchanged
}

TEST_CASE(request_refuses_when_the_link_is_down) {
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  // link_up defaults to false -- deliberately never set here.

  ScriptedSequence seq;
  CHECK(!runner.request(seq));
  CHECK(!runner.busy());
  CHECK_EQ(seq.start_count, 0);
}

TEST_CASE(the_stack_does_not_overflow_a_5th_nesting_level_fails_cleanly) {
  CHECK_EQ(static_cast<int>(Runner::MAX_DEPTH), 4);  // this test assumes the documented depth

  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);

  ScriptedSequence root, a, b, c, d;
  root.name_ = "Root";
  a.name_ = "A";
  b.name_ = "B";
  c.name_ = "C";
  d.name_ = "D";

  root.on_poll = [&](ScriptedSequence &self) { return self.do_await(a, 1); };
  a.on_poll = [&](ScriptedSequence &self) { return self.do_await(b, 1); };
  b.on_poll = [&](ScriptedSequence &self) { return self.do_await(c, 1); };
  // root(1) -> a(2) -> b(3) -> c(4) is already MAX_DEPTH; awaiting d would be
  // a 5th frame and must be refused rather than corrupting the stack.
  c.on_poll = [&](ScriptedSequence &self) { return self.do_await(d, 1); };
  d.on_poll = [](ScriptedSequence &) { return Poll::DONE; };  // must never actually run

  CHECK(runner.request(root));

  Clock clock{kp, runner};
  clock.advance(200);

  CHECK_EQ(d.start_count, 0);
  CHECK_EQ(d.poll_count, 0);
  CHECK(c.last_result == Poll::FAILED);
  CHECK(b.last_result == Poll::FAILED);
  CHECK(a.last_result == Poll::FAILED);
  CHECK(root.last_result == Poll::FAILED);
  CHECK(!runner.busy());
}

TEST_CASE(runner_refuses_set_while_a_diagnostic_page_is_showing) {
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  disp.update(vatest::pad16("Diagnostic  05"), vatest::pad16(""), 0);

  RecordingLog log;
  runner.set_log_sink(log.as_log_sink());

  CHECK(!runner.tap(SET, 50));
  CHECK(!runner.press(SET));
  CHECK(!kp.busy());  // never reached the keypad at all
  CHECK(log.error_count >= 2);

  // Never interlocked: Up/Down/Main are always accepted, regardless of screen.
  CHECK(runner.tap(UP, 50));
}

TEST_CASE(runner_does_not_refuse_set_on_a_non_diagnostic_screen) {
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  disp.update(vatest::pad16("Indoor Temp"), vatest::pad16(" 20C"), 0);

  CHECK(runner.tap(SET, 50));
}

// ============================================================ primitives --

TEST_CASE(tap_issues_one_tap_and_completes_once_idle) {
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  Tap tap(UP, 50);
  CHECK(runner.request(tap));
  clock.advance(600);  // comfortably past the 50ms tap plus the 400ms gap

  CHECK(!runner.busy());
  CHECK(!kp.busy());
  const auto episodes = episodes_from(sink);
  CHECK_EQ(episodes.size(), static_cast<size_t>(1));
  CHECK_EQ(episodes[0], UP);
}

TEST_CASE(tap_fails_fast_when_the_set_interlock_refuses_it) {
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  disp.update(vatest::pad16("Diagnostic  05"), vatest::pad16(""), 0);
  RecordingSink sink;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  Tap tap(SET, 50);
  CHECK(runner.request(tap));
  clock.advance(100);

  CHECK(!runner.busy());
  CHECK(sink.frames.empty());  // the refused tap never reached the keypad
  CHECK(!kp.busy());
}

TEST_CASE(hold_until_completes_when_the_predicate_becomes_true_and_releases_the_key) {
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  bool ready = false;

  HoldUntil hold(UP, [&ready] { return ready; }, 5000);
  CHECK(runner.request(hold));

  Clock clock{kp, runner};
  clock.advance(200);
  CHECK(runner.busy());
  CHECK(kp.busy());  // asserted, predicate not true yet

  ready = true;
  clock.advance(40);

  CHECK(!runner.busy());
  CHECK(!kp.busy());  // released in on_finish -- see HoldUntil's class comment
}

TEST_CASE(hold_until_fails_and_still_releases_the_key_on_timeout) {
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);

  HoldUntil hold(DOWN, [] { return false; }, 500);
  CHECK(runner.request(hold));

  Clock clock{kp, runner};
  clock.advance(800);  // past the 500ms timeout

  CHECK(!runner.busy());
  CHECK(!kp.busy());
}

TEST_CASE(hold_until_re_presses_every_tick_which_is_safe_and_does_not_restart_the_watchdog) {
  // The one invariant the spec calls out by name: press() is a documented
  // no-op when re-asserting an already-held mask (keypad.h), so HoldUntil
  // calling it every poll() must not reset Keypad's 30s watchdog clock.
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);

  HoldUntil hold(UP, [] { return false; }, 60000);  // outlives the watchdog on purpose
  CHECK(runner.request(hold));

  Clock clock{kp, runner};
  // +100ms margin: the very first press() only takes effect on the tick
  // AFTER poll() calls it (Keypad applies a pending hold in loop(), not
  // synchronously -- see keypad.cpp), so the watchdog's own 30s clock
  // actually starts one tick later than t=0.
  clock.advance(30100);

  CHECK(kp.watchdog_releases() >= 1u);
  // HoldUntil's own 60s timeout is still nowhere close, so it is still
  // running and will simply re-press on the next tick (a fresh assertion,
  // not the no-op above, since the watchdog already dropped it back to
  // IDLE) -- proving the watchdog is a real, working backstop independent
  // of whatever the sequence above it believes.
  CHECK(runner.busy());
}

TEST_CASE(goto_menu_issues_five_up_taps_then_n_down_taps) {
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  GotoMenu goto_menu(2);  // Summer Mode
  CHECK(runner.request(goto_menu));
  clock.advance(5000);  // 5 taps + settle + 2 taps + settle, comfortably

  CHECK(!runner.busy());
  const auto episodes = episodes_from(sink);
  CHECK_EQ(episodes.size(), static_cast<size_t>(7));
  for (size_t i = 0; i < 5; i++) {
    CHECK_EQ(episodes[i], UP);
  }
  for (size_t i = 5; i < 7; i++) {
    CHECK_EQ(episodes[i], DOWN);
  }
}

TEST_CASE(goto_menu_index_0_is_five_up_taps_and_no_down_taps) {
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  GotoMenu goto_menu(0);  // status -- the hard stop alone
  CHECK(runner.request(goto_menu));
  clock.advance(4000);

  CHECK(!runner.busy());
  const auto episodes = episodes_from(sink);
  CHECK_EQ(episodes.size(), static_cast<size_t>(5));
  for (const auto mask : episodes) {
    CHECK_EQ(mask, UP);
  }
}

TEST_CASE(leave_menu_issues_exactly_one_up_tap) {
  Keypad kp;
  Display disp;  // default: line1() == "", classify()s as STATUS -- not a menu screen
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  LeaveMenu leave;
  CHECK(runner.request(leave));
  clock.advance(600);

  CHECK(!runner.busy());
  const auto episodes = episodes_from(sink);
  CHECK_EQ(episodes.size(), static_cast<size_t>(1));
  CHECK_EQ(episodes[0], UP);
}

TEST_CASE(leave_menu_issues_no_up_tap_while_an_editor_is_open) {
  // Finding 1's structural backstop, on the primitive directly: with
  // Display::editor_open() true at TAP time, LeaveMenu must press NOTHING
  // -- Up here would adjust the field under the cursor instead of
  // navigating out (the class comment's 14C->19C observation), so the
  // right move is to fall straight through to WAIT_EXIT and let the unit's
  // own ~2-minute timeout close the editor untouched. Complements
  // leave_menu_issues_exactly_one_up_tap (editor_open() false: a fresh
  // Display, no frame ever received) and
  // leave_menu_waits_out_the_unit_timeout_rather_than_pressing_again (also
  // false, once aged past the settle window) -- this is the one case where
  // it reads true.
  Keypad kp;
  Display disp;
  disp.update(vatest::pad16("Set Clock"), vatest::pad16("Mon 12:00"), 0);  // a fresh frame -- editor_open() reads true
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  LeaveMenu leave;
  CHECK(runner.request(leave));

  // The first poll (now=20ms) is well inside editor_open()'s ~1200ms settle
  // window from the update() above -- TAP must decline to tap at all.
  clock.advance(20);
  CHECK(episodes_from(sink).empty());
  CHECK(runner.busy());  // parked in WAIT_EXIT already, not TAP/WAIT_TAP

  // Stays open for a while longer -- still nothing transmitted.
  clock.advance(500);
  CHECK(episodes_from(sink).empty());
  CHECK(runner.busy());

  // The unit's own timeout eventually closes the menu on its own; confirm
  // LeaveMenu notices and finishes -- still without ever pressing Up.
  disp.update(vatest::pad16("18%"), vatest::pad16(""), clock.now);
  clock.advance(100);

  CHECK(!runner.busy());
  CHECK(episodes_from(sink).empty());
}

TEST_CASE(leave_menu_waits_out_the_unit_timeout_rather_than_pressing_again) {
  Keypad kp;
  Display disp;
  disp.update(vatest::pad16("Set Clock"), vatest::pad16(""), 0);  // parked on a menu screen
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  // Age the display past Display::editor_open()'s own ~1200ms settle window
  // before starting LeaveMenu -- stage 7a made LeaveMenu's own TAP step
  // check editor_open() before pressing anything (Finding 1's structural
  // backstop), and this fixture's single update() at t=0 would otherwise
  // read as a freshly-opened editor for its own first ~1200ms purely because
  // the frame is new, not because anything is actually being edited -- the
  // exact false positive Display::editor_open()'s own class comment warns
  // about ("line2 stopped changing cannot be observed as an edge"). This
  // test means to simulate a settled, non-editing menu screen, so it must
  // start past that window for editor_open() to read false, matching what
  // it is actually testing.
  clock.advance(1300);

  LeaveMenu leave;
  CHECK(runner.request(leave));

  // Still parked on the menu screen the whole time -- must NOT press Up
  // again while waiting. 60s is comfortably under both LeaveMenu's own
  // 130s wait and the default 120s root backstop (see this file's note on
  // that overlap in the final report), enough to prove "patient", not "mash".
  clock.advance(60000);
  CHECK(runner.busy());
  CHECK_EQ(episodes_from(sink).size(), static_cast<size_t>(1));

  // The unit's own timeout would eventually close the menu; simulate that
  // and confirm LeaveMenu notices and finishes, still without a second tap.
  disp.update(vatest::pad16("18%"), vatest::pad16(""), clock.now);
  clock.advance(100);

  CHECK(!runner.busy());
  CHECK_EQ(episodes_from(sink).size(), static_cast<size_t>(1));
}

// ======================================================= FetchDiagnostics --

TEST_CASE(fetch_diagnostics_completes_and_tracks_the_highest_page_actually_seen) {
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingLog log;
  bool success = false;

  FetchDiagnostics fd;
  fd.set_log_sink(log.as_log_sink());
  fd.set_on_success([&success] { success = true; });

  CHECK(runner.request(fd));
  Clock clock{kp, runner};

  // 1: enter the diagnostic menu.
  clock.advance(40);
  disp.update(vatest::pad16("Diagnostic  00"), vatest::pad16(""), clock.now);
  clock.advance(40);

  // 2: 300ms settle, then into the fixed 8s Down hold.
  clock.advance(400);

  // 3: the unit's own auto-repeat would walk every page; simulate it
  // passing through a few, ending on the highest one this firmware has
  // (27, never a hardcoded 28 -- PLAN.md §3/§7).
  disp.update(vatest::pad16("Diagnostic  05"), vatest::pad16(""), clock.now);
  clock.advance(1000);
  disp.update(vatest::pad16("Diagnostic  12"), vatest::pad16(""), clock.now);
  clock.advance(1000);
  disp.update(vatest::pad16("Diagnostic  27"), vatest::pad16(""), clock.now);
  clock.advance(7000);  // finishes out the 8s hold (well past it in total)

  // 5: hold Up back to page 00.
  disp.update(vatest::pad16("Diagnostic  00"), vatest::pad16(""), clock.now);
  clock.advance(200);

  // 6: release, 250ms settle -- see the dedicated test below for this step
  // in isolation.
  clock.advance(300);

  // 7: the fresh hold, until the display leaves the diagnostic menu.
  disp.update(vatest::pad16("18%"), vatest::pad16(""), clock.now);
  clock.advance(200);

  CHECK(!runner.busy());
  CHECK(success);
  CHECK(!kp.busy());
  // Four distinct pages were shown (0, 5, 12, 27); highest was 27, so "of"
  // is 28 -- never the old component's hardcoded, wrong-for-this-firmware 28.
  CHECK(log.last_info.find("captured 4 of 28") != std::string::npos);
  CHECK(log.last_info.find("highest 27") != std::string::npos);
}

TEST_CASE(fetch_diagnostics_releases_and_settles_before_the_fresh_exit_hold) {
  // The essential, non-obvious behaviour PLAN.md calls out: holding Up
  // straight through from page 00 never exits, so step 6 must be a genuine
  // release-and-settle, not a no-op on the way to step 7's hold.
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  FetchDiagnostics fd;

  CHECK(runner.request(fd));
  Clock clock{kp, runner};

  disp.update(vatest::pad16("Diagnostic  00"), vatest::pad16(""), clock.now);
  clock.advance(40);
  clock.advance(400);
  clock.advance(8000);
  clock.advance(440);  // the release settle between the Down and Up holds
  disp.update(vatest::pad16("Diagnostic  00"), vatest::pad16(""), clock.now);
  clock.advance(40);  // the page-00 hold observes it and completes

  // Now in the 250ms settle: the key must already be released -- not still
  // asserted from the hold that just finished.
  CHECK(!kp.busy());
  clock.advance(100);
  CHECK(!kp.busy());  // still released partway through the settle

  // Past the settle: EXIT's FRESH hold begins, asserting Up again.
  clock.advance(200);
  CHECK(kp.busy());
}

TEST_CASE(fetch_diagnostics_fails_and_recovers_if_the_unit_never_enters_the_menu) {
  Keypad kp;
  Display disp;  // never updated to a diagnostic screen
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingLog log;
  bool success = false;

  FetchDiagnostics fd;
  fd.set_log_sink(log.as_log_sink());
  fd.set_on_success([&success] { success = true; });

  CHECK(runner.request(fd));
  Clock clock{kp, runner};
  clock.advance(15200);  // past ENTER's 15s timeout

  CHECK(!runner.busy());
  CHECK(!success);
  CHECK(!kp.busy());  // released via HoldUntil::on_finish and/or Runner::recover()
}

TEST_CASE(fetch_diagnostics_leaves_real_silence_between_the_down_and_up_holds) {
  // A release is only silence on this protocol -- there is no key-up frame --
  // so moving straight from the Down hold into the Up hold would leave a
  // single loop tick of silence, far below the 400ms the unit needs to see a
  // release at all. It would read Down-then-Up as one unbroken press, never
  // register the Up, and the page-00 hold would time out with the display
  // abandoned in the diagnostic menu: the old component's exact failure.
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  kp.set_frame_sink(sink.as_frame_sink());

  FetchDiagnostics fd;
  CHECK(runner.request(fd));
  Clock clock{kp, runner};
  sink.current_now = &clock.now;

  disp.update(vatest::pad16("Diagnostic  00"), vatest::pad16(""), clock.now);
  clock.advance(40);    // entered the diagnostic menu
  clock.advance(400);   // 300ms entry settle, then into the Down hold
  clock.advance(8100);  // run the fixed 8s Down hold out

  // The Down hold has now been released. Step forward and confirm nothing is
  // transmitted at all until the settle has elapsed.
  const size_t frames_at_release = sink.frames.size();
  clock.advance(200);
  CHECK_EQ(sink.frames.size(), frames_at_release);  // silent 200ms in
  clock.advance(140);
  CHECK_EQ(sink.frames.size(), frames_at_release);  // still silent at ~340ms

  // Past 400ms of silence the Up hold may start.
  clock.advance(200);
  CHECK(sink.frames.size() > frames_at_release);
  CHECK_EQ(sink.frames.back().second, UP);
}

// ===================================================== editing model (6) --
// OpenEditor, AdjustField, ExitEditChain -- PLAN.md's "The unit's editing
// model". Set is the only key ever safe once an editor is open, so these
// three are the only things in the component allowed to open one, adjust a
// value inside one, or walk one closed.

TEST_CASE(adjust_field_steps_the_right_direction_and_stops_at_target) {
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  disp.update(vatest::pad16("Indoor Temp"), vatest::pad16("25 C"), 0);

  AdjustField adjust;
  adjust.reset(parse_temp_field, direction_no_wrap, 20, 40);
  CHECK(runner.request(adjust));

  // Simulates the unit ticking the value down one degree per Down tap --
  // exactly what the closed loop expects: a fresh, still-unequal value keeps
  // it going, and only a value that actually equals the target stops it.
  int value = 25;
  for (int i = 0; i < 5; i++) {
    clock.advance(300);  // let the queued tap actually fire
    value--;
    disp.update(vatest::pad16("Indoor Temp"), vatest::pad16(std::to_string(value) + " C"), clock.now);
    clock.advance(300);  // let AdjustField notice the change and loop back
  }
  clock.advance(200);

  CHECK(!runner.busy());
  CHECK(!kp.busy());  // no key left asserted
  const auto episodes = episodes_from(sink);
  CHECK_EQ(episodes.size(), static_cast<size_t>(5));
  for (const auto mask : episodes) {
    CHECK_EQ(mask, DOWN);
  }
}

TEST_CASE(adjust_field_respects_its_guard_limit_rather_than_looping_forever) {
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  // Line2 never moves -- simulates a value the unit refuses to accept
  // (PLAN.md risk 6) or a run of dropped presses. Summer Mode's own guard
  // (3, mhrv_orig/summer_bypass.yaml) so the test does not need to simulate
  // 40 stuck taps.
  disp.update(vatest::pad16("Summer Mode"), vatest::pad16("Off"), 0);

  AdjustField adjust;
  adjust.reset(parse_summer_mode_field, direction_no_wrap, 1 /* want On */, 3);
  CHECK(runner.request(adjust));

  clock.advance(6000);  // 3 guard taps, each waiting out the ~900ms change timeout, plus margin

  CHECK(!runner.busy());
  CHECK(!kp.busy());
  const auto episodes = episodes_from(sink);
  // 3 guard taps -- never a 4th, the guard rather than luck stopped it --
  // plus one more from Runner::recover(): the sequence failed on a menu
  // screen with no editor open (line2 has been frozen well past the settle
  // window), so recovery correctly walks out with a single Up.
  CHECK_EQ(episodes.size(), static_cast<size_t>(4));
  for (const auto mask : episodes) {
    CHECK_EQ(mask, UP);
  }
}

TEST_CASE(adjust_field_waits_for_line2_to_change_before_tapping_again) {
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  disp.update(vatest::pad16("Indoor Temp"), vatest::pad16("25 C"), 0);

  AdjustField adjust;
  adjust.reset(parse_temp_field, direction_no_wrap, 20, 40);
  CHECK(runner.request(adjust));

  clock.advance(500);  // the first tap fires and its gap clears
  CHECK_EQ(episodes_from(sink).size(), static_cast<size_t>(1));

  // Line2 has NOT changed -- still "25 C". Even though the keypad itself is
  // idle again, a second tap must not fire yet: never blind.
  clock.advance(300);  // ~800ms since the tap -- inside the ~900ms change window
  CHECK_EQ(episodes_from(sink).size(), static_cast<size_t>(1));

  clock.advance(400);  // now well past 900ms -- the timeout itself allows the retry
  CHECK_EQ(episodes_from(sink).size(), static_cast<size_t>(2));
}

TEST_CASE(adjust_field_tolerates_a_blank_or_blinking_frame_without_erroring_or_reading_it_as_zero) {
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  disp.update(vatest::pad16("Indoor Temp"), vatest::pad16("25 C"), 0);

  AdjustField adjust;
  adjust.reset(parse_temp_field, direction_no_wrap, 20, 40);
  CHECK(runner.request(adjust));

  clock.advance(500);  // the first tap (25 -> Down) fires
  CHECK_EQ(episodes_from(sink).size(), static_cast<size_t>(1));

  // The editor blinks: line2 goes blank for a frame -- "   C", not "" -- the
  // real rendering of a mid-blink temperature field (parser.h). This must
  // NOT be read as 0, must NOT fail the sequence, and must NOT itself
  // provoke a tap.
  disp.update(vatest::pad16("Indoor Temp"), vatest::pad16("   C"), clock.now);
  clock.advance(300);

  CHECK(runner.busy());  // still running -- not FAILED, not DONE
  CHECK_EQ(episodes_from(sink).size(), static_cast<size_t>(1));  // the blank frame alone did not tap

  // The blink resolves back to the real (unmoved) value.
  disp.update(vatest::pad16("Indoor Temp"), vatest::pad16("25 C"), clock.now);
  clock.advance(500);

  CHECK_EQ(episodes_from(sink).size(), static_cast<size_t>(2));  // taps again, correctly -- not stuck
  CHECK_EQ(episodes_from(sink)[1], DOWN);
  CHECK(runner.busy());  // 25 -> 20 needs more taps still; not finished by this one
}

TEST_CASE(adjust_field_reports_an_unavailable_target_through_an_installed_log_sink) {
  // Finding 2 (stage 7a): AdjustField grew a log_ member and this exact
  // error, but nothing ever called set_log_sink() on any of the
  // AdjustField members that reuse it (SyncClock::adjust_field_,
  // WriteSetting::adjust_field_) -- so the one genuinely new failure mode
  // that stage introduced logged nothing at all. This is a direct unit test
  // of AdjustField itself: install a log sink, give it a TargetFn that
  // always reports "unavailable" (false), and assert the error actually
  // arrives -- not just that the sequence fails.
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingLog log;
  Clock clock{kp, runner};

  disp.update(vatest::pad16("Indoor Temp"), vatest::pad16("25 C"), 0);

  AdjustField adjust;
  adjust.set_log_sink(log.as_log_sink());
  adjust.reset(
      parse_temp_field, direction_no_wrap, [](int &) { return false; },  // target never available
      40);
  CHECK(runner.request(adjust));

  clock.advance(100);

  CHECK(!runner.busy());
  CHECK_EQ(log.error_count, 1);
  CHECK(log.last_error.find("target unavailable") != std::string::npos);
}

TEST_CASE(open_editor_retries_exactly_once_then_fails) {
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  disp.update(vatest::pad16("Indoor Temp"), vatest::pad16("25 C"), 0);
  // Let the initial update go stale so editor_open() reads false from the
  // start -- otherwise its own default ~1200ms settle window would read as
  // "just opened" purely from this setup call, not from anything Set did.
  clock.advance(1300);

  OpenEditor open_editor;
  CHECK(runner.request(open_editor));

  // Nothing ever updates line2 again -- Set is tapped, but no editor ever
  // opens. Two full tap+700ms-settle cycles, comfortably.
  clock.advance(2500);
  // ...then long enough for recover()'s walk-out tap and its mandatory gap to
  // finish, so the "no key left asserted" check below is about the sequence
  // having cleaned up rather than about catching recovery mid-tap.
  clock.advance(600);

  CHECK(!runner.busy());
  CHECK(!kp.busy());  // no key left asserted
  const auto episodes = episodes_from(sink);
  // Two Sets -- exactly one retry -- then Runner::recover()'s single Up. No
  // editor ever opened here, so walking out with Up is the safe gesture; had
  // one been open, recover() would have left it to the unit's own timeout.
  CHECK_EQ(episodes.size(), static_cast<size_t>(3));
  CHECK_EQ(episodes[0], SET);
  CHECK_EQ(episodes[1], SET);
  CHECK_EQ(episodes[2], UP);
}

TEST_CASE(open_editor_succeeds_on_the_first_try_when_the_editor_actually_opens) {
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  disp.update(vatest::pad16("Indoor Temp"), vatest::pad16("25 C"), 0);
  clock.advance(1300);

  OpenEditor open_editor;
  CHECK(runner.request(open_editor));

  clock.advance(600);  // the Set tap (50ms) fires and its 400ms gap clears
  // The editor starts blinking -- any change counts as the "opened" signal.
  disp.update(vatest::pad16("Indoor Temp"), vatest::pad16(""), clock.now);
  clock.advance(900);  // past the 700ms settle -- editor_open() now reads true

  CHECK(!runner.busy());
  CHECK(!kp.busy());
  CHECK_EQ(episodes_from(sink).size(), static_cast<size_t>(1));  // no retry needed
}

TEST_CASE(exit_edit_chain_presses_only_set_up_to_4_times_then_falls_back_to_waiting_out_the_timeout) {
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  RecordingLog log;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  // A perpetually blinking editor -- the unit never actually closes it, the
  // exact scenario this primitive's fallback exists for.
  disp.update(vatest::pad16("Indoor Temp"), vatest::pad16("20 C"), 0);

  ExitEditChain exit_chain;
  exit_chain.set_log_sink(log.as_log_sink());
  CHECK(runner.request(exit_chain));

  bool blink = false;
  while (runner.busy()) {
    blink = !blink;
    disp.update(vatest::pad16("Indoor Temp"), vatest::pad16(blink ? "20 C" : "   C"), clock.now);
    clock.advance(300);  // well under the 1200ms settle window -- keeps editor_open() true throughout
  }

  CHECK(!kp.busy());  // no key left asserted, however this ended
  const auto episodes = episodes_from(sink);
  CHECK_EQ(episodes.size(), static_cast<size_t>(4));  // MAX_COMMITS -- never more
  for (const auto mask : episodes) {
    CHECK_EQ(mask, SET);  // never Up or Down -- see class comment
  }
  CHECK(log.warn_count >= 1);  // logged the fallback
}

TEST_CASE(exit_edit_chain_stops_as_soon_as_the_editor_is_seen_closed) {
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  disp.update(vatest::pad16("Indoor Temp"), vatest::pad16("20 C"), 0);
  clock.advance(1300);  // stale -- editor_open() reads false already, nothing to walk out of

  ExitEditChain exit_chain;
  CHECK(runner.request(exit_chain));
  clock.advance(100);

  CHECK(!runner.busy());
  CHECK(!kp.busy());
  CHECK(episodes_from(sink).empty());  // never pressed Set at all -- there was nothing open
}

// ============================================================ WriteSetting --

TEST_CASE(write_setting_runs_navigate_open_adjust_commit_exit_readback_in_order) {
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  WriteSetting write_setting;
  write_setting.configure(SettingId::SUMMER_MODE, 1);  // want On
  CHECK(runner.request(write_setting));

  // NAVIGATE (GotoMenu(2)) never looks at the display; provide the arrival
  // screen well after navigation starts so VERIFY's freshness check (newer
  // than nav_started_ms_) is satisfied for the rest of this run -- nothing
  // else updates line1/line2 to "Summer Mode" again until READ_BACK's own
  // pass gets there.
  clock.advance(100);
  disp.update(vatest::pad16("Summer Mode"), vatest::pad16("Off"), clock.now);
  clock.advance(4500);  // GotoMenu(2): 5 Up + settle + 2 Down + settle
  clock.advance(300);   // let VERIFY notice, well before OPEN's own Set tap can land

  // OPEN (OpenEditor): wait for its first Set tap to actually register --
  // robust to exactly how long GotoMenu/VERIFY took above -- then blink for
  // long enough to cover its one possible retry (~1.15s: tap+gap+700ms
  // settle) before settling steadily on "Off".
  const size_t before_open = episodes_from(sink).size();
  while (episodes_from(sink).size() == before_open) {
    clock.advance(20);
  }
  for (int i = 0; i < 5; i++) {
    disp.update(vatest::pad16("Summer Mode"), vatest::pad16(i % 2 == 0 ? "" : "Off"), clock.now);
    clock.advance(300);
  }
  disp.update(vatest::pad16("Summer Mode"), vatest::pad16("Off"), clock.now);
  clock.advance(200);

  // ADJUST (AdjustField): Off(0) -> On(1). Usually one Up tap, but the
  // blinking above and this step can legitimately overlap by a tick or two
  // (this is simulating two independent, un-synchronised timers, same as
  // the real unit and this component would be) -- see the episode-run
  // assertions below, which accept one or more Up taps here rather than
  // assuming a precise count.
  clock.advance(500);  // an Up tap fires
  disp.update(vatest::pad16("Summer Mode"), vatest::pad16("On"), clock.now);
  clock.advance(300);  // AdjustField notices, cur == target, DONE

  // COMMIT, SETTLE (1800ms), EXIT_CHAIN (the "On" update above is by now
  // well past its 1200ms settle window, so editor_open() already reads
  // false -- nothing to walk out of), HOME, and READ_BACK's own navigation:
  // none of it needs the display faked any further. ReadSettings never
  // hard-fails on a value it cannot confirm (see its class comment) -- this
  // test only cares that every step runs, in order, not that the read-back
  // finds a value.
  clock.advance(30000);

  CHECK(!runner.busy());
  CHECK(!kp.busy());  // no key left asserted, run to completion
  const auto episodes = episodes_from(sink);
  CHECK(episodes.size() > 10);

  // NAVIGATE: 5 Up, then 2 Down (menu index 2).
  for (size_t i = 0; i < 5; i++) {
    CHECK_EQ(episodes[i], UP);
  }
  CHECK_EQ(episodes[5], DOWN);
  CHECK_EQ(episodes[6], DOWN);

  // OPEN: a run of one or more Set taps (OpenEditor retries at most once;
  // exactly how many lands here depends on tick alignment between the
  // simulated blink and the sequence's own timers, not on anything this
  // test should be pinning down -- see the comment above).
  size_t idx = 7;
  CHECK(idx < episodes.size());
  CHECK_EQ(episodes[idx], SET);
  while (idx < episodes.size() && episodes[idx] == SET) {
    idx++;
  }

  // ADJUST: a run of one or more Up taps -- Off -> On, the value-changing
  // direction, never Down.
  CHECK(idx < episodes.size());
  CHECK_EQ(episodes[idx], UP);
  while (idx < episodes.size() && episodes[idx] == UP) {
    idx++;
  }

  // COMMIT: the next Set -- exactly one, and the LAST Set this run ever
  // presses.
  CHECK(idx < episodes.size());
  CHECK_EQ(episodes[idx], SET);
  idx++;

  // Everything after COMMIT is EXIT_CHAIN/HOME/READ_BACK -- navigation
  // only. ExitEditChain closed immediately (nothing was open by then), and
  // nothing past this point ever presses Set again.
  for (size_t i = idx; i < episodes.size(); i++) {
    CHECK(episodes[i] == UP || episodes[i] == DOWN);
  }
  // READ_BACK genuinely ran (its own GotoMenu(2)+GotoMenu(3)+GotoMenu(0)),
  // not just HOME on its own -- a fixed HOME-only tail would be 5 episodes.
  CHECK(episodes.size() > idx + 5);
}

TEST_CASE(write_setting_reaches_exit_edit_chain_on_the_outdoor_hop_failure_path) {
  // PLAN.md: Outdoor Temp's hop can leave an editor open whether or not it
  // landed on the right screen, so EXIT_CHAIN must be reached even when the
  // hop fails -- this is the failure-path half of the funnel design (the
  // success half is exercised, less directly, by the SUMMER_MODE test
  // above reaching EXIT_CHAIN too).
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  RecordingLog log;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  WriteSetting write_setting;
  write_setting.set_log_sink(log.as_log_sink());
  write_setting.configure(SettingId::OUTDOOR_TEMP, 14);
  CHECK(runner.request(write_setting));

  // NAVIGATE lands on Indoor Temp (Outdoor Temp's own row targets it) --
  // provide a fresh, parseable value so VERIFY passes.
  clock.advance(100);
  disp.update(vatest::pad16("Indoor Temp"), vatest::pad16("25 C"), clock.now);
  clock.advance(4700);  // GotoMenu(3): 5 Up + settle + 3 Down + settle
  clock.advance(300);

  // OPEN: blink between blank and "25 C" for a few seconds -- comfortably
  // covers OpenEditor's worst case (one retry, ~2.3s) whatever tick exactly
  // its Set tap and 700ms settle land on -- then let it succeed on Indoor
  // Temp.
  for (int i = 0; i < 12; i++) {
    disp.update(vatest::pad16("Indoor Temp"), vatest::pad16(i % 2 == 0 ? "" : "25 C"), clock.now);
    clock.advance(300);
  }

  // HOP_COMMIT taps Set to step past Indoor Temp -- but the display never
  // shows "Outdoor Temp" (simulating the chain closing outright instead of
  // advancing, which PLAN.md records as an observed real outcome). This is
  // a plain runner_->tap(), not OpenEditor, so it fires exactly once; give
  // it time to fire, then let WAIT_HOP_SCREEN's 3000ms timeout elapse.
  clock.advance(500);
  clock.advance(3300);  // WAIT_HOP_SCREEN gives up

  // EXIT_CHAIN (closes immediately -- the last real change is long stale by
  // now), HOME, and READ_BACK's own full navigation pass, none of which
  // needs the display faked any further -- same reasoning as the SUMMER_MODE
  // test above.
  clock.advance(30000);

  CHECK(!runner.busy());
  CHECK(!kp.busy());  // no key left asserted, however this ended

  // Reached EXIT_CHAIN despite the hop failing: after OPEN's Set tap(s) --
  // one or two, OpenEditor retries at most once -- and HOP_COMMIT's single
  // Set, nothing further presses Set (ExitEditChain closed immediately --
  // the last change was long enough ago to already read as closed by the
  // time it is checked), but the run keeps going (HOME + READ_BACK's own
  // navigation) rather than stopping dead.
  const auto episodes = episodes_from(sink);
  CHECK(episodes.size() > 2);
  size_t set_count = 0;
  for (const auto mask : episodes) {
    if (mask == SET) {
      set_count++;
    }
  }
  CHECK(set_count == 2 || set_count == 3);  // OPEN (1-2) + HOP_COMMIT (1) -- never a Set after that

  // And the overall write is reported as FAILED -- nothing was actually
  // written, since the hop never reached Outdoor Temp.
  CHECK(log.warn_count >= 1);
}

// ============================================================ ReadSettings --

TEST_CASE(read_settings_finishes_and_returns_home_even_when_nothing_parses) {
  // ReadSettings never hard-fails on an unreadable value (see its class
  // comment) -- this proves the whole pass still completes and settles the
  // keypad even when every screen it tries to read is unreadable/unreached.
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingLog log;

  ReadSettings read_settings;
  read_settings.set_log_sink(log.as_log_sink());
  CHECK(runner.request(read_settings));

  Clock clock{kp, runner};
  clock.advance(20000);  // two GotoMenus, two value-waits, no outdoor hop (indoor never read), one more GotoMenu

  CHECK(!runner.busy());
  CHECK(!kp.busy());
  CHECK(log.warn_count >= 2);  // Summer Mode and Indoor Temp both logged as unreadable
}

TEST_CASE(recover_never_presses_a_key_while_an_editor_is_still_open) {
  // Up inside an editor adjusts the value under the cursor instead of
  // navigating -- the 14 C walked to 19 C failure -- and Set would commit
  // whatever half-finished value a failed sequence abandoned. Neither is
  // acceptable, so recover() must leave an open editor entirely alone and
  // let the unit's own ~2min timeout close it, which commits nothing.
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  kp.set_frame_sink(sink.as_frame_sink());

  ScriptedSequence seq;
  seq.on_poll = [](ScriptedSequence &) { return Poll::FAILED; };
  CHECK(runner.request(seq));

  Clock clock{kp, runner};
  sink.current_now = &clock.now;

  // Park on a settings screen with a value blinking -- an open editor.
  disp.update(vatest::pad16("Indoor Temp"), vatest::pad16("     20 C"), clock.now);
  clock.tick();
  disp.update(vatest::pad16("Indoor Temp"), vatest::pad16("        C"), clock.now);  // blink
  clock.tick();
  CHECK(disp.editor_open(clock.now));

  clock.advance(1000);

  CHECK(!runner.busy());
  CHECK(sink.frames.empty());  // not one frame: no Up, no Set, nothing
  CHECK(!kp.busy());
}

TEST_CASE(recover_does_tap_up_once_on_a_menu_screen_with_no_editor_open) {
  // The other half of the above: with nothing being edited, walking out with
  // a single Up is safe and is what should happen.
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  kp.set_frame_sink(sink.as_frame_sink());

  Clock clock{kp, runner};
  sink.current_now = &clock.now;

  // Park on a menu screen and let it go quiet first: editor_open() is a
  // staleness test, so the screen has to have been settled for longer than
  // the settle window BEFORE the sequence fails, not merely at some point.
  disp.update(vatest::pad16("Indoor Temp"), vatest::pad16("     20 C"), clock.now);
  clock.advance(3000);
  CHECK(!disp.editor_open(clock.now));

  ScriptedSequence seq;
  seq.on_poll = [](ScriptedSequence &) { return Poll::FAILED; };
  CHECK(runner.request(seq));
  clock.advance(1000);  // fails, recovers, and the Up tap goes out

  const auto episodes = episodes_from(sink);
  CHECK_EQ(episodes.size(), static_cast<size_t>(1));
  if (!episodes.empty()) {
    CHECK_EQ(episodes[0], UP);
  }
}
