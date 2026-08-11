#include "test_framework.h"

#include <optional>
#include <string>

#include "screens.h"
#include "sequence.h"
#include "sequence_test_helpers.h"

using namespace esphome::vent_axia;
using namespace vatest;

// Stage 7's last deliverable, and the last sequence in this component by
// design (PLAN.md §8): ResetFilter, the one irreversible operation. Split
// into its own file per tests/CMakeLists.txt's glob -- reuses
// test_sequence.cpp/test_set_airflow_mode.cpp's fake keypad/display harness
// via sequence_test_helpers.h, same as every other seq_*.cpp's test file.
//
// Unlike SetAirflowMode, this sequence never reads status::StatusTracker (it
// only needs to know whether the display is on the status loop at all, not
// anything about boosting/purging), so these tests drive Display directly --
// no feed()/StatusTracker pair needed here.

namespace {

// Drives a full, successful FetchDiagnostics child run from wherever
// diagnostics_scan_'s ENTER step is waiting -- the EXACT same sequence of
// display updates and clock advances test_sequence.cpp's own
// fetch_diagnostics_completes_and_tracks_the_highest_page_actually_seen
// uses (down to the individual advance() calls -- FetchDiagnostics' own
// step timings are fiddly enough, between HoldUntil predicates and fixed
// settles, that a "simplified" rewrite of this sequence is exactly the kind
// of thing that quietly falls short of HOLD_DOWN_MS or DOWN_SETTLE_MS by a
// few hundred ms and stalls the whole scan -- only the page shown mid-scan
// and the final status-screen text are parameterised. Passing "23" through
// this scan is decorative only: the host build has no diagnostics decode
// wired to publish anything from it (that only exists on the ESPHome side
// of the portable-core boundary, vent_axia.cpp), so every test below
// supplies VERIFY's answer directly via set_filter_hours_source() instead.
void run_diagnostics_child_to_completion(Display &disp, Clock &clock, const std::string &final_line1) {
  clock.advance(40);
  disp.update(vatest::pad16("Diagnostic  00"), vatest::pad16(""), clock.now);
  clock.advance(40);

  clock.advance(400);  // ENTER_SETTLE_MS (300ms), into the fixed 8s Down hold

  disp.update(vatest::pad16("Diagnostic  05"), vatest::pad16(""), clock.now);
  clock.advance(1000);
  disp.update(vatest::pad16("Diagnostic  23"), vatest::pad16("00000"), clock.now);
  clock.advance(1000);
  disp.update(vatest::pad16("Diagnostic  27"), vatest::pad16(""), clock.now);
  clock.advance(7000);  // finishes out the 8s hold (well past it in total: 1000+1000+7000 =~ 9000 > HOLD_DOWN_MS)

  disp.update(vatest::pad16("Diagnostic  00"), vatest::pad16(""), clock.now);
  clock.advance(200);  // the page-00 hold observes it and completes

  clock.advance(300);  // release, 250ms settle before the fresh exit hold

  disp.update(vatest::pad16(final_line1), vatest::pad16(""), clock.now);
  clock.advance(200);  // the fresh exit hold sees the display has left the menu
}

}  // namespace

// ================================================ Requires the status screen --
// PLAN.md §7's own words for this sequence: "requires the status screen".
// No retry window, same reasoning as SetAirflowMode's own CHECK_CURRENT
// (seq_set_airflow_mode.cpp) -- this is the one guard standing between a
// stray press and an unrecoverable write.

TEST_CASE(a_menu_screen_at_start_refuses_without_pressing_anything) {
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  RecordingLog log;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  disp.update(vatest::pad16("Summer Mode"), vatest::pad16("On"), clock.now);

  ResetFilter seq;
  seq.set_log_sink(log.as_log_sink());
  CHECK(runner.request(seq));

  clock.advance(200);

  CHECK(!runner.busy());
  CHECK(!kp.busy());
  CHECK(log.error_count >= 1);
  CHECK(episodes_from(sink).empty());
  // Finding 2: actionable, not just diagnostic -- this is what a human sees
  // when the button appears to do nothing, unlike the reference script
  // (mhrv_orig/controls.yaml), which actively tried goto_menu 0 first
  // rather than refusing outright (seq_reset_filter.cpp's own CHECK_STATUS
  // comment records why this sequence diverges).
  CHECK(log.last_error.find("press this button again") != std::string::npos);
}

TEST_CASE(a_diagnostic_screen_at_start_refuses_without_pressing_anything) {
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  RecordingLog log;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  disp.update(vatest::pad16("Diagnostic  05"), vatest::pad16("018 029 %      "), clock.now);

  ResetFilter seq;
  seq.set_log_sink(log.as_log_sink());
  CHECK(runner.request(seq));

  clock.advance(200);

  CHECK(!runner.busy());
  CHECK(!kp.busy());
  CHECK(log.error_count >= 1);
  CHECK(episodes_from(sink).empty());
}

TEST_CASE(no_frame_at_all_refuses_without_pressing_anything) {
  // have_frame() false -- the display has never been updated. Same "must
  // fail, not guess" reasoning as SetAirflowMode's own no-status-frame test:
  // a default-constructed Display's line1() must never be read as "the
  // status screen" merely because it happens not to match a known menu
  // prefix (CHECK_STATUS's own comment, seq_reset_filter.cpp).
  Keypad kp;
  Display disp;  // never updated
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  RecordingLog log;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  ResetFilter seq;
  seq.set_log_sink(log.as_log_sink());
  CHECK(runner.request(seq));

  clock.advance(200);

  CHECK(!runner.busy());
  CHECK(!kp.busy());
  CHECK(log.error_count >= 1);
  CHECK(episodes_from(sink).empty());
}

// ===================================================== The hold itself --
// Up+Down simultaneously, for a fixed 5500ms -- a mask with BOTH bits, not
// two sequential taps (the same "holding two keys at once is fine, the
// protocol is a bitmask" fact FetchDiagnostics' own Up+Main entry combo
// relies on).

TEST_CASE(the_hold_is_a_simultaneous_up_plus_down_mask_for_5500ms_not_two_taps) {
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  RecordingLog log;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  disp.update(vatest::pad16("Normal Airflow"), vatest::pad16("18%"), clock.now);

  ResetFilter seq;
  seq.set_log_sink(log.as_log_sink());
  CHECK(runner.request(seq));

  // Partway through the hold: exactly one key mask on the wire, the OR of
  // both bits together, and the keypad must still be asserting it (proves a
  // HOLD, not two taps that would each release well inside 5500ms).
  clock.advance(2000);
  CHECK(kp.busy());
  CHECK_EQ(sink.frames.back().second, static_cast<KeyMask>(UP | DOWN));
  const auto mid_hold_episodes = episodes_from(sink);
  CHECK_EQ(mid_hold_episodes.size(), static_cast<size_t>(1));
  CHECK_EQ(mid_hold_episodes[0], static_cast<KeyMask>(UP | DOWN));

  // Past 5500ms the hold releases -- this is far enough past HOLD_MS that,
  // if this had instead been implemented as two sequential taps, both would
  // long since have completed and the keypad would have gone idle well
  // before now.
  clock.advance(3600);  // 5600ms total, past the fixed 5500ms hold
  CHECK(!kp.busy());
  const auto episodes = episodes_from(sink);
  CHECK_EQ(episodes.size(), static_cast<size_t>(1));  // one episode: the single combined hold
  CHECK_EQ(episodes[0], static_cast<KeyMask>(UP | DOWN));
}

// ============================================== Chains FetchDiagnostics --

TEST_CASE(release_settle_stays_silent_for_the_full_1000ms_before_fetch_begins) {
  // CLAUDE.md's "release is silence... a hold-to-hold transition needs an
  // explicit gap": RELEASE_SETTLE_MS (1000ms -- longer than the usual 400ms
  // elsewhere in this file, see its own comment, sequence.h) must fully
  // elapse before FETCH's own Up+Main entry hold begins, exactly the same
  // invariant FetchDiagnostics' own RELEASE_DOWN step protects between its
  // Down and Up holds.
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  disp.update(vatest::pad16("Normal Airflow"), vatest::pad16("18%"), clock.now);

  ResetFilter seq;
  CHECK(runner.request(seq));

  clock.advance(5600);  // HOLD_MS (5500ms) clears; the Up+Down hold has released
  CHECK(!kp.busy());
  const size_t frames_at_release = sink.frames.size();

  clock.advance(900);  // comfortably inside RELEASE_SETTLE_MS's 1000ms -- still silent
  CHECK(!kp.busy());
  CHECK_EQ(sink.frames.size(), frames_at_release);

  clock.advance(200);  // past 1000ms total -- FETCH may now begin asserting Up+Main
  CHECK(kp.busy());
  CHECK(sink.frames.size() > frames_at_release);
  CHECK_EQ(sink.frames.back().second, static_cast<KeyMask>(UP | MAIN));
}

TEST_CASE(after_the_hold_it_chains_a_diagnostics_scan_before_finishing) {
  // No filter_hours_source_ wired -- this test only cares that a diagnostics
  // scan actually runs (a fresh Up+Main hold shows up AFTER the Up+Down
  // hold's own release+settle), not what VERIFY concludes from it. See the
  // three-outcome tests below for that, and the dedicated settle test above
  // for the exact silent-gap timing.
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  RecordingLog log;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  disp.update(vatest::pad16("Normal Airflow"), vatest::pad16("18%"), clock.now);

  ResetFilter seq;
  seq.set_log_sink(log.as_log_sink());
  CHECK(runner.request(seq));

  clock.advance(5600);  // HOLD_MS (5500ms) clears
  CHECK(!kp.busy());
  const size_t episodes_after_hold = episodes_from(sink).size();
  CHECK_EQ(episodes_after_hold, static_cast<size_t>(1));

  clock.advance(1200);  // RELEASE_SETTLE_MS (1000ms) clears with margin

  run_diagnostics_child_to_completion(disp, clock, "18%");

  CHECK(!runner.busy());
  CHECK(!kp.busy());
  // A second episode appeared: FetchDiagnostics' own Up+Main entry combo,
  // proving the chain actually ran rather than VERIFY being reached with no
  // scan at all.
  const auto episodes = episodes_from(sink);
  CHECK(episodes.size() > episodes_after_hold);
  CHECK_EQ(episodes[1], static_cast<KeyMask>(UP | MAIN));
}

TEST_CASE(a_diagnostics_scan_that_cannot_even_enter_the_menu_fails_the_whole_sequence) {
  // The hold already happened by the time FETCH runs -- a failure here is
  // "could not CONFIRM", not "did not reset". Still reported as a FAILED
  // sequence (the cascade every other await() in this component uses,
  // Sequence::await()'s own comment) and recovered via Runner::recover(),
  // both proved by the two CHECKs below -- Runner::loop()'s own class
  // comment is explicit that a CHILD's failure cascading to its root is
  // silent by itself (unlike the root's own timeout_ms() firing, or
  // request() refusing to start at all, both of which do log): only
  // whoever wires on_sequence_failed (not exercised by this host-level test,
  // which drives Runner directly rather than through the hub) or the
  // resulting !runner.busy()/!kp.busy() state actually surfaces it here --
  // same reasoning test_sequence.cpp's own
  // fetch_diagnostics_fails_and_recovers_if_the_unit_never_enters_the_menu
  // test gives for not asserting a log line either.
  Keypad kp;
  Display disp;  // stays on "Normal Airflow" -- never shows a diagnostic screen
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingLog log;
  Clock clock{kp, runner};

  disp.update(vatest::pad16("Normal Airflow"), vatest::pad16("18%"), clock.now);

  ResetFilter seq;
  seq.set_log_sink(log.as_log_sink());
  CHECK(runner.request(seq));

  clock.advance(5600);   // HOLD_MS clears
  clock.advance(1200);   // RELEASE_SETTLE_MS clears with margin, FETCH awaits diagnostics_scan_
  clock.advance(15500);  // diagnostics_scan_'s own ENTER_TIMEOUT_MS (15000ms) elapses, never having entered

  CHECK(!runner.busy());
  CHECK(!kp.busy());  // released via HoldUntil::on_finish, ResetFilter::on_finish and/or Runner::recover()
}

// ===================================================== The four outcomes --
// mhrv_orig/controls.yaml's own three-way log (no reading at all, still
// zero, or confirmed nonzero), extended by Opus review (Finding 1) with a
// fourth: a nonzero reading that is UNCHANGED from before the hold, which
// must not be reported as a fresh confirmation -- see hours_before_'s own
// comment (sequence.h) for why that particular false positive is the one
// this sequence must never produce. All four are distinguishable, each
// logged at a different severity, and none fails the sequence itself (the
// hold already happened by VERIFY's turn).

TEST_CASE(no_filter_hours_source_configured_logs_cannot_confirm_and_still_succeeds) {
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingLog log;
  Clock clock{kp, runner};

  disp.update(vatest::pad16("Normal Airflow"), vatest::pad16("18%"), clock.now);

  ResetFilter seq;
  seq.set_log_sink(log.as_log_sink());
  // set_filter_hours_source() deliberately never called.
  CHECK(runner.request(seq));

  clock.advance(5600);
  clock.advance(1200);
  run_diagnostics_child_to_completion(disp, clock, "18%");

  CHECK(!runner.busy());  // VERIFY never fails the sequence -- see its own comment
  CHECK(!kp.busy());
  CHECK(log.warn_count >= 1);
  CHECK(log.last_warn.find("no reading from page 23") != std::string::npos);
}

TEST_CASE(a_filter_hours_source_answering_nullopt_logs_cannot_confirm) {
  // Same outcome as the unconfigured case above, reached the other way: a
  // source IS wired, but it has never published (mirrors
  // sensor::Sensor::has_state() == false) -- see FilterHoursSource's own
  // comment (sequence.h) for why nullopt is the "never published" case.
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingLog log;
  Clock clock{kp, runner};

  disp.update(vatest::pad16("Normal Airflow"), vatest::pad16("18%"), clock.now);

  ResetFilter seq;
  seq.set_log_sink(log.as_log_sink());
  seq.set_filter_hours_source([]() -> std::optional<int> { return std::nullopt; });
  CHECK(runner.request(seq));

  clock.advance(5600);
  clock.advance(1200);
  run_diagnostics_child_to_completion(disp, clock, "18%");

  CHECK(!runner.busy());
  CHECK(!kp.busy());
  CHECK(log.warn_count >= 1);
  CHECK(log.last_warn.find("no reading from page 23") != std::string::npos);
}

TEST_CASE(a_filter_hours_source_still_reading_zero_logs_the_reset_did_not_take) {
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingLog log;
  Clock clock{kp, runner};

  disp.update(vatest::pad16("Normal Airflow"), vatest::pad16("18%"), clock.now);

  ResetFilter seq;
  seq.set_log_sink(log.as_log_sink());
  seq.set_filter_hours_source([]() -> std::optional<int> { return 0; });
  CHECK(runner.request(seq));

  clock.advance(5600);
  clock.advance(1200);
  run_diagnostics_child_to_completion(disp, clock, "18%");

  CHECK(!runner.busy());  // still not a sequence failure -- see VERIFY's own comment
  CHECK(!kp.busy());
  CHECK(log.warn_count >= 1);
  CHECK(log.last_warn.find("did not take") != std::string::npos);
}

TEST_CASE(a_filter_hours_source_reading_nonzero_and_changed_logs_confirmed_with_the_hours) {
  // The realistic confirmed-reset shape: 0 hours before the hold (the usual
  // reason someone presses this button at all), a fresh nonzero reading
  // after diagnostics_scan_ re-reads page 23. A stateful lambda models the
  // two calls this sequence actually makes -- on_start() (before the hold,
  // sees the FIRST value) and VERIFY (after the chained scan, sees the
  // SECOND) -- rather than one that would answer the same thing both times,
  // which is exactly the case the next test covers.
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingLog log;
  Clock clock{kp, runner};

  disp.update(vatest::pad16("Normal Airflow"), vatest::pad16("18%"), clock.now);

  ResetFilter seq;
  seq.set_log_sink(log.as_log_sink());
  int calls = 0;
  seq.set_filter_hours_source([&calls]() -> std::optional<int> {
    calls++;
    return calls == 1 ? 0 : 4380;  // 6 months, hours -- reads 0 pre-hold, confirmed nonzero post-scan
  });
  CHECK(runner.request(seq));

  clock.advance(5600);
  clock.advance(1200);
  run_diagnostics_child_to_completion(disp, clock, "18%");

  CHECK(!runner.busy());
  CHECK(!kp.busy());
  CHECK(log.info_count >= 1);
  CHECK(log.last_info.find("confirmed") != std::string::npos);
  CHECK(log.last_info.find("4380") != std::string::npos);
  CHECK_EQ(log.warn_count, 0);  // a genuine confirmation must not ALSO warn
}

TEST_CASE(a_filter_hours_source_reading_nonzero_but_unchanged_from_before_the_hold_warns_not_confirms) {
  // Finding 1's own regression: someone cleans the filters EARLY and
  // presses reset_filter while the timer still shows a nonzero reading --
  // then diagnostics_scan_'s own scan happens to miss page 23 this run
  // (a dropped frame mid-scroll, say), so filter_hours_source_ keeps
  // answering the SAME pre-hold value it always has. Before the on_start()
  // snapshot existed, a bare read here would have read this as "confirmed,
  // N hours to go" for a reset that may never have taken -- exactly the one
  // outcome PLAN.md's "refuse rather than guess" spirit says must not be
  // allowed to lie, since it is what a human trusts before deciding whether
  // to press this irreversible button again. The constant lambda below
  // means on_start()'s pre-hold snapshot and VERIFY's post-scan read are
  // identical, 4380 both times -- proving VERIFY catches the staleness
  // rather than reporting a confirmation off a merely-nonzero number.
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingLog log;
  Clock clock{kp, runner};

  disp.update(vatest::pad16("Normal Airflow"), vatest::pad16("18%"), clock.now);

  ResetFilter seq;
  seq.set_log_sink(log.as_log_sink());
  seq.set_filter_hours_source([]() -> std::optional<int> { return 4380; });  // same value every call
  CHECK(runner.request(seq));

  clock.advance(5600);
  clock.advance(1200);
  run_diagnostics_child_to_completion(disp, clock, "18%");

  CHECK(!runner.busy());  // still not a sequence failure -- see VERIFY's own comment
  CHECK(!kp.busy());
  // last_warn, not just "a warn happened somewhere": VERIFY runs LAST (after
  // CHECK_STATUS's and diagnostics_scan_'s own earlier info-level logging),
  // so this is specifically VERIFY's own conclusion, and it must NOT read
  // "confirmed" -- the one outcome this sequence must never report on a
  // merely-nonzero-but-stale reading.
  CHECK(log.warn_count >= 1);
  CHECK(log.last_warn.find("unchanged") != std::string::npos);
  CHECK(log.last_warn.find("4380") != std::string::npos);
  CHECK(log.last_info.find("confirmed") == std::string::npos);
}

// ========================================================= No hangs --
// "however it ends" (success, refusal, child failure, timeout) leaves no key
// asserted -- on_finish()'s backstop release() (seq_reset_filter.cpp),
// exercised across every ending this file covers.

TEST_CASE(root_timeout_still_releases_the_key_and_recovers) {
  // A pathological stall mid-HOLD -- the root timeout (120s, sequence.h's
  // own arithmetic comment) is the last-resort backstop if something above
  // this sequence's own bounded steps ever goes wrong. Every step here
  // already carries its own timeout (HOLD's fixed 5500ms, RELEASE_SETTLE's
  // fixed 1000ms, diagnostics_scan_'s own internal timeouts), so this is
  // deliberately a synthetic case -- request() then never even ticking the
  // clock forward through a real HOLD, just proving the backstop itself
  // works, the same "no dedicated root-timeout test" reasoning
  // test_set_airflow_mode.cpp gives, checked here via recover() directly
  // instead of waiting out a redundant 120s.
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingLog log;

  disp.update(vatest::pad16("Normal Airflow"), vatest::pad16("18%"), 0);

  ResetFilter seq;
  seq.set_log_sink(log.as_log_sink());
  CHECK(runner.request(seq));

  Clock clock{kp, runner};
  clock.advance(2000);  // partway into the 5.5s hold -- key is asserted
  CHECK(kp.busy());

  clock.advance(120200);  // past timeout_ms()'s 120000ms root budget

  CHECK(!runner.busy());
  CHECK(!kp.busy());  // released via on_finish()'s backstop and/or Runner::recover()
}

TEST_CASE(a_check_status_refusal_leaves_the_keypad_untouched) {
  // The cheapest ending: refused before a single key was ever asserted.
  // on_finish()'s release() is still called (unconditionally, "however it
  // ends"), but Keypad::release() on an already-idle keypad is a documented
  // no-op, so this just confirms nothing was left half-pressed.
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingLog log;
  Clock clock{kp, runner};

  disp.update(vatest::pad16("Set Clock"), vatest::pad16("Mon 12:00"), clock.now);

  ResetFilter seq;
  seq.set_log_sink(log.as_log_sink());
  CHECK(runner.request(seq));

  clock.advance(200);

  CHECK(!runner.busy());
  CHECK(!kp.busy());
}
