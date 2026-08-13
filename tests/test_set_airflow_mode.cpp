#include "test_framework.h"

#include <string>
#include <vector>

#include "screens.h"
#include "sequence.h"
#include "sequence_test_helpers.h"
#include "status.h"

using namespace esphome::vent_axia;
using namespace vatest;

// Stage 7's other deliverable: SetAirflowMode (PLAN.md §3's row, §6's
// airflow_mode select). Split into its own file per tests/CMakeLists.txt's
// glob -- reuses test_sequence.cpp/test_sync_clock.cpp's fake keypad/display
// harness via sequence_test_helpers.h, same as test_sync_clock.cpp does.
//
// Opus's review of this stage's first draft (Finding 1) found that
// CHECK_CURRENT never verified the display was actually on the status loop
// before touching Main -- so, unlike the claim this comment used to make,
// several tests below DO use screens::classify()'s menu-screen strings now,
// specifically to prove that refusal. Finding 2 additionally made
// CHECK_CURRENT depend on the hub's alternation-aware status::StatusTracker
// rather than a raw single-frame parse, so every test constructs one and
// wires it via seq.set_status() -- see feed() below, which keeps a Display
// and a StatusTracker in sync exactly the way VentAxiaHub::loop() does for
// every incoming frame (vent_axia.cpp).

namespace {

/// Like episodes_from() (sequence_test_helpers.h) but keeps each episode's
/// first/last transmitted timestamp instead of collapsing it to just a
/// mask. Needed here specifically because CANCEL_PURGE/OPEN_PURGE's fixed
/// 5500ms Main HOLD and every other Main press in this sequence (a ~50ms
/// TAP) assert the exact same mask -- the mask alone cannot tell a hold
/// apart from a tap, but the span between an episode's first and last
/// frame can: a hold retransmits every tx_interval_ms_ (20ms default) for
/// seconds, a tap emits only the handful of frames its ~50ms duration
/// allows.
struct EpisodeSpan {
  KeyMask mask;
  uint32_t first_ms;
  uint32_t last_ms;
};

std::vector<EpisodeSpan> episode_spans_from(const RecordingSink &sink) {
  std::vector<EpisodeSpan> out;
  uint32_t last_ts = 0;
  bool have_last = false;
  for (const auto &f : sink.frames) {
    if (!have_last || f.first - last_ts > 100) {
      out.push_back(EpisodeSpan{f.second, f.first, f.first});
    } else {
      out.back().last_ms = f.first;
    }
    last_ts = f.first;
    have_last = true;
  }
  return out;
}

/// Feeds an identical line1/line2 update into both `disp` and `status`,
/// mirroring exactly what VentAxiaHub::loop() does for every incoming frame
/// (vent_axia.cpp: display_.update() immediately followed by status_.update()
/// with is_status_screen = display_.screen_kind() == STATUS). Finding 2
/// (Opus review) made SetAirflowMode depend on the sticky, alternation-aware
/// StatusTracker rather than a raw display read, so every test needs both
/// kept in sync the same way production code does, or CHECK_CURRENT's own
/// "purge state not yet known" refusal fires on every single test that
/// forgets it.
void feed(Display &disp, status::StatusTracker &status, const std::string &line1, const std::string &line2,
          uint32_t now_ms) {
  disp.update(vatest::pad16(line1), vatest::pad16(line2), now_ms);
  status.update(disp.line1(), disp.line2(), disp.screen_kind() == screens::ScreenKind::STATUS, now_ms);
}

}  // namespace

// ======================================================= On the status loop --
// Finding 1 (Opus review): CHECK_CURRENT must prove the display is on the
// status loop before touching Main at all -- a menu or diagnostic screen is
// a reachable start state (Runner::recover()'s own exit tap does not wait
// for the unit to actually leave the menu, and the unit's own timeout is
// minutes), not a hypothetical one.

TEST_CASE(a_menu_screen_at_start_refuses_without_pressing_anything) {
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  RecordingLog log;
  status::StatusTracker status;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  feed(disp, status, "Summer Mode", "On", clock.now);

  SetAirflowMode seq;
  seq.set_log_sink(log.as_log_sink());
  seq.set_status(&status);
  seq.configure(AirflowTarget::NORMAL);
  CHECK(runner.request(seq));

  clock.advance(200);

  CHECK(!runner.busy());
  CHECK(!kp.busy());
  CHECK(log.error_count >= 1);
  CHECK(episodes_from(sink).empty());
}

TEST_CASE(a_diagnostic_screen_at_start_refuses_without_pressing_anything) {
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  RecordingLog log;
  status::StatusTracker status;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  feed(disp, status, "Diagnostic  05", "018 029 %      ", clock.now);

  SetAirflowMode seq;
  seq.set_log_sink(log.as_log_sink());
  seq.set_status(&status);
  seq.configure(AirflowTarget::BOOST_30);
  CHECK(runner.request(seq));

  clock.advance(200);

  CHECK(!runner.busy());
  CHECK(!kp.busy());
  CHECK(log.error_count >= 1);
  CHECK(episodes_from(sink).empty());
}

// ============================================ Purge state must be known --
// Finding 2 (Opus review): CHECK_CURRENT's purge answer must come from
// status_'s STICKY purging(), never a raw single-frame parse -- and a
// tracker that does not yet have an answer (or was never wired up at all)
// must FAIL rather than being read as "not purging" (CLAUDE.md's "Blank !=
// zero" applied to a derived boolean).

TEST_CASE(set_airflow_mode_fails_when_no_status_frame_has_ever_been_decoded) {
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  RecordingLog log;
  status::StatusTracker status;  // constructed, but update() is never called -- has_state() stays false
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  // The display itself is fine (on the status loop) -- this isolates the
  // "purge state not yet known" refusal from the "not on the status loop"
  // one CHECK_CURRENT also carries (the menu-screen tests above).
  disp.update(vatest::pad16("Normal Airflow"), vatest::pad16("18%"), 0);

  SetAirflowMode seq;
  seq.set_log_sink(log.as_log_sink());
  seq.set_status(&status);
  seq.configure(AirflowTarget::NORMAL);
  CHECK(runner.request(seq));

  clock.advance(200);

  CHECK(!runner.busy());
  CHECK(!kp.busy());
  CHECK(log.error_count >= 1);
  CHECK(episodes_from(sink).empty());
}

TEST_CASE(set_airflow_mode_fails_when_no_status_tracker_was_ever_configured) {
  // set_status() deliberately never called -- the default-constructed
  // nullptr. Same "must fail, not guess" reasoning as the no-frame-yet case
  // above, but for the "nobody wired this up" mistake specifically.
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  RecordingLog log;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  disp.update(vatest::pad16("Normal Airflow"), vatest::pad16("18%"), 0);

  SetAirflowMode seq;
  seq.set_log_sink(log.as_log_sink());
  seq.configure(AirflowTarget::NORMAL);
  CHECK(runner.request(seq));

  clock.advance(200);

  CHECK(!runner.busy());
  CHECK(!kp.busy());
  CHECK(log.error_count >= 1);
  CHECK(episodes_from(sink).empty());
}

// =============================================================== Purge --

TEST_CASE(purge_target_when_already_purging_needs_no_keys_at_all) {
  // Purge is idempotent (CHECK_CURRENT, seq_set_airflow_mode.cpp): if the
  // unit is already showing it, there is nothing to press. Matches the
  // manual's "same gesture starts and cancels it" -- pressing the cancel
  // gesture again would TURN PURGE OFF, the opposite of what a repeated
  // Purge selection should do.
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  RecordingLog log;
  status::StatusTracker status;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  feed(disp, status, "Purge", "120 m", clock.now);

  SetAirflowMode seq;
  seq.set_log_sink(log.as_log_sink());
  seq.set_status(&status);
  seq.configure(AirflowTarget::PURGE);
  CHECK(runner.request(seq));

  clock.advance(200);

  CHECK(!runner.busy());
  CHECK(!kp.busy());
  CHECK(episodes_from(sink).empty());
}

TEST_CASE(purge_target_from_normal_start_is_a_5500ms_hold_not_a_tap) {
  // From a non-purging state, Purge is reached DIRECTLY -- no boost probe
  // and no normalising taps at all (the manual describes Purge as a hold
  // from any state, not something requiring Normal first; see
  // SetAirflowMode's own class comment, step 1).
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  RecordingLog log;
  status::StatusTracker status;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  feed(disp, status, "Normal Airflow", "18%", clock.now);

  SetAirflowMode seq;
  seq.set_log_sink(log.as_log_sink());
  seq.set_status(&status);
  seq.configure(AirflowTarget::PURGE);
  CHECK(runner.request(seq));

  clock.advance(5700);  // PURGE_HOLD_MS (5500ms) plus margin for the release + FINISHED transition

  CHECK(!runner.busy());
  CHECK(!kp.busy());
  const auto spans = episode_spans_from(sink);
  CHECK_EQ(spans.size(), static_cast<size_t>(1));
  CHECK_EQ(spans[0].mask, MAIN);
  // A single ~50ms tap could not span this long -- this is what proves it
  // was a HOLD, not a tap (see episode_spans_from()'s own comment).
  CHECK(spans[0].last_ms - spans[0].first_ms > 5000);
}

TEST_CASE(purge_target_with_an_active_but_not_in_this_frame_purge_does_nothing) {
  // Finding 2 (Opus review): the STICKY purging() must be trusted over a
  // single frame -- the status loop alternates, and PLAN.md risk 4 (the
  // purge layout is unresolved) makes a single-frame miss of an active
  // purge plausible, not just theoretical. This seeds a genuine Purge
  // sighting, then lets the CURRENT frame move on to something that
  // mentions Purge nowhere at all (simulating the alternation), well
  // within status.h's ~12s sticky window -- purging() must still read
  // true, so a PURGE target must land on CHECK_CURRENT's no-op branch and
  // touch nothing, NOT re-issue the cancel-and-start-again gesture.
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  RecordingLog log;
  status::StatusTracker status;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  feed(disp, status, "Purge", "120 m", clock.now);
  clock.advance(3000);
  feed(disp, status, "Normal Airflow", "18%", clock.now);

  SetAirflowMode seq;
  seq.set_log_sink(log.as_log_sink());
  seq.set_status(&status);
  seq.configure(AirflowTarget::PURGE);
  CHECK(runner.request(seq));

  clock.advance(200);

  CHECK(!runner.busy());
  CHECK(!kp.busy());
  CHECK(episodes_from(sink).empty());  // no cancel hold -- the sticky flag still says purging
}

TEST_CASE(boost_target_with_that_same_still_purging_state_issues_the_cancel_hold_first) {
  // Same seeded state as the PURGE no-op test above, but targeting BOOST_30
  // instead -- the sticky purging() must still route through CANCEL_PURGE
  // even though the CURRENT frame alone shows no evidence of Purge at all.
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  RecordingLog log;
  status::StatusTracker status;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  feed(disp, status, "Purge", "120 m", clock.now);
  clock.advance(3000);
  feed(disp, status, "Normal Airflow", "18%", clock.now);

  SetAirflowMode seq;
  seq.set_log_sink(log.as_log_sink());
  seq.set_status(&status);
  seq.configure(AirflowTarget::BOOST_30);
  CHECK(runner.request(seq));

  clock.advance(5600);  // CANCEL_PURGE's fixed 5500ms hold clears
  clock.advance(500);   // CANCEL_SETTLE_MS (400ms) -- the explicit hold-to-tap gap
  // PROBE_CHECK's own defensive re-check uses the INSTANTANEOUS parse of the
  // CURRENT display, deliberately not the sticky flag (see
  // seq_set_airflow_mode.cpp's PROBE_CHECK comment) -- "Normal Airflow" has
  // no "Purge" substring, so this passes and falls through to the ordinary
  // boost probe.
  clock.advance(8200);  // the negative boost probe
  clock.advance(500);   // the single apply tap plus its key_gap

  CHECK(!runner.busy());
  CHECK(!kp.busy());
  const auto spans = episode_spans_from(sink);
  CHECK_EQ(spans.size(), static_cast<size_t>(2));
  CHECK_EQ(spans[0].mask, MAIN);
  CHECK(spans[0].last_ms - spans[0].first_ms > 5000);  // the cancel hold fired FIRST
  CHECK_EQ(spans[1].mask, MAIN);
  CHECK(spans[1].last_ms - spans[1].first_ms < 100);   // then the single apply tap
}

// ============================================================== Normal --

TEST_CASE(normal_target_from_normal_start_needs_no_taps_but_still_pays_for_one_probe) {
  // Even "already there" pays for exactly one negative boost probe (~8s) --
  // this sequence has no shortcut for "nothing to do", the same way the old
  // boost_normalise always probed first regardless of what it expected to
  // find. presses_for_(NORMAL) == 0, so APPLY_TAP is skipped entirely once
  // the probe concludes.
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  RecordingLog log;
  status::StatusTracker status;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  feed(disp, status, "Normal Airflow", "18%", clock.now);

  SetAirflowMode seq;
  seq.set_log_sink(log.as_log_sink());
  seq.set_status(&status);
  seq.configure(AirflowTarget::NORMAL);
  CHECK(runner.request(seq));

  clock.advance(40);
  CHECK(runner.busy());  // still mid-probe well before the 8s timeout
  CHECK(episodes_from(sink).empty());

  clock.advance(8200);  // PROBE_TIMEOUT_MS (8000ms) plus margin

  CHECK(!runner.busy());
  CHECK(!kp.busy());
  CHECK(episodes_from(sink).empty());  // never touched the keypad at all
}

// =============================================================== Boost --

TEST_CASE(boost30_target_from_normal_start_needs_exactly_one_tap) {
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  RecordingLog log;
  status::StatusTracker status;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  feed(disp, status, "Normal Airflow", "18%", clock.now);

  SetAirflowMode seq;
  seq.set_log_sink(log.as_log_sink());
  seq.set_status(&status);
  seq.configure(AirflowTarget::BOOST_30);
  CHECK(runner.request(seq));

  clock.advance(8200);  // the negative probe (8s) before APPLY_TAP ever queues anything
  clock.advance(500);   // the single apply tap plus its key_gap

  CHECK(!runner.busy());
  CHECK(!kp.busy());
  const auto episodes = episodes_from(sink);
  CHECK_EQ(episodes.size(), static_cast<size_t>(1));
  CHECK_EQ(episodes[0], MAIN);
}

TEST_CASE(boost60_target_from_normal_start_needs_exactly_two_taps) {
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  RecordingLog log;
  status::StatusTracker status;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  feed(disp, status, "Normal Airflow", "18%", clock.now);

  SetAirflowMode seq;
  seq.set_log_sink(log.as_log_sink());
  seq.set_status(&status);
  seq.configure(AirflowTarget::BOOST_60);
  CHECK(runner.request(seq));

  clock.advance(8200);
  // Both apply taps are queued as a batch (APPLY_TAP, seq_set_airflow_mode.cpp)
  // -- Keypad's own queue enforces key_gap between them, so this is "wait
  // for the queue to drain", not two separate steps.
  clock.advance(1000);

  CHECK(!runner.busy());
  CHECK(!kp.busy());
  const auto episodes = episodes_from(sink);
  CHECK_EQ(episodes.size(), static_cast<size_t>(2));
  CHECK_EQ(episodes[0], MAIN);
  CHECK_EQ(episodes[1], MAIN);
}

// ============================================== BOOST_CONTINUOUS ordinals --
// AirflowTarget's ordinal order is LOAD-BEARING (sequence.h's own comment):
// vent_axia.cpp's write_select() casts the select's raw option index
// straight to this enum, and no test can span the Python/C++ boundary to
// catch select.py's AIRFLOW_MODE_OPTIONS drifting out of sync with it. This
// pins the C++ half against the documented list: ["Normal", "Boost 30 min",
// "Boost 60 min", "Boost Continuous", "Purge"].
TEST_CASE(airflow_target_ordinals_match_select_pys_documented_option_list) {
  CHECK(static_cast<AirflowTarget>(0) == AirflowTarget::NORMAL);
  CHECK(static_cast<AirflowTarget>(1) == AirflowTarget::BOOST_30);
  CHECK(static_cast<AirflowTarget>(2) == AirflowTarget::BOOST_60);
  CHECK(static_cast<AirflowTarget>(3) == AirflowTarget::BOOST_CONTINUOUS);
  CHECK(static_cast<AirflowTarget>(4) == AirflowTarget::PURGE);
}

TEST_CASE(boost_continuous_target_from_normal_start_needs_exactly_three_taps) {
  // Behavioural equivalent of "presses_for_(BOOST_CONTINUOUS) == 3" --
  // presses_for_ is private static with no test precedent for calling it
  // directly (see boost30/boost60's own tests just above), so this observes
  // the same fact through the public Runner/Keypad surface instead, exactly
  // mirroring boost60_target_from_normal_start_needs_exactly_two_taps.
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  RecordingLog log;
  status::StatusTracker status;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  feed(disp, status, "Normal Airflow", "18%", clock.now);

  SetAirflowMode seq;
  seq.set_log_sink(log.as_log_sink());
  seq.set_status(&status);
  seq.configure(AirflowTarget::BOOST_CONTINUOUS);
  CHECK(runner.request(seq));

  clock.advance(8200);  // the negative probe (8s) before APPLY_TAP ever queues anything
  // Three taps queued as a batch (APPLY_TAP) -- Keypad's queue enforces
  // key_gap between each, so this waits for the queue to drain, not three
  // separate steps.
  clock.advance(1500);

  CHECK(!runner.busy());
  CHECK(!kp.busy());
  const auto episodes = episodes_from(sink);
  CHECK_EQ(episodes.size(), static_cast<size_t>(3));
  for (const auto mask : episodes) {
    CHECK_EQ(mask, MAIN);
  }
}

TEST_CASE(probe_catches_a_boost_frame_that_arrives_mid_wait_rather_than_only_sampling_once) {
  // Line1 alternates roughly every 3.2-3.5s while boosting (status.h) -- a
  // probe that only sampled once could catch the "wrong" half of the cycle
  // (e.g. "Check Filter") and wrongly conclude "not boosting". This starts
  // on the wrong half and flips to "Boost Airflow" partway through
  // PROBE_WAIT's 8s window, proving the probe wakes up and acts on it
  // rather than waiting out the full timeout and concluding false.
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  RecordingLog log;
  status::StatusTracker status;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  // Genuinely boosting, but the very first frame this sequence sees happens
  // to be the OTHER half of the status loop's alternation.
  feed(disp, status, "Check Filter", "48%", clock.now);

  SetAirflowMode seq;
  seq.set_log_sink(log.as_log_sink());
  seq.set_status(&status);
  seq.configure(AirflowTarget::NORMAL);
  CHECK(runner.request(seq));

  clock.advance(40);  // CHECK_CURRENT -> PROBE_CHECK's own immediate read: "Check Filter" != Boost -> PROBE_WAIT
  clock.advance(2000);  // comfortably inside the 8s window, nowhere near the timeout

  const auto before_flip = episodes_from(sink).size();
  feed(disp, status, "Boost Airflow", "48%       30m", clock.now);

  const auto wait_for_next_episode = [&]() {
    while (episodes_from(sink).size() == before_flip) {
      clock.advance(20);
    }
  };
  // A normalising tap fires well before the 8s timeout would otherwise
  // elapse (which would instead have concluded "not boosting" and skipped
  // straight to FINISHED for this NORMAL target, transmitting nothing at
  // all -- so a tap here can only mean the probe correctly detected
  // boosting and started normalising). Bounded wait: fails the test with a
  // clear infinite-loop timeout rather than hanging if this regresses.
  wait_for_next_episode();
  CHECK(clock.now < 5000);  // well before CHECK_CURRENT's ~40ms + PROBE_WAIT's 8000ms timeout would have elapsed

  // Let the rest of the run play out: after this one normalising tap, flip
  // back to Normal (simulating the tap having worked) so the sequence
  // actually finishes, rather than leaving it running when the test ends.
  // Bounded rather than a fixed advance: NORMALISE_SETTLE's own 1000ms is
  // measured from whenever NORMALISE_WAIT happens to hand off to it (not
  // from a round clock boundary this test controls), so a tight fixed
  // budget here is fragile -- a generous bound that still fails loudly
  // (via the CHECK below) rather than hanging if this regresses is safer.
  clock.advance(500);
  feed(disp, status, "Normal Airflow", "18%", clock.now);
  for (int guard = 0; guard < 500 && runner.busy(); guard++) {
    clock.advance(20);
  }

  CHECK(!runner.busy());
  CHECK(!kp.busy());
  const auto episodes = episodes_from(sink);
  CHECK_EQ(episodes.size(), static_cast<size_t>(1));
  CHECK_EQ(episodes[0], MAIN);
}

// ============================================================ Normalise --

TEST_CASE(normal_target_from_boosting_start_normalises_through_continuous_with_probed_taps) {
  // The full mhrv_orig/controls.yaml boost_normalise loop: probe, tap,
  // settle, re-probe -- repeated until the unit confirms Normal. This
  // starts at Boost 30 (one press already made, unknown to this sequence --
  // it only ever knows "boosting or not") and simulates the unit's counter
  // advancing one notch per tap: Boost 30 -> Boost 60 -> Continuous ->
  // Normal, three taps in total. Line1 reads "Boost Airflow" throughout the
  // first three probes (all three of those states share the same line1
  // text -- mhrv_orig's own boost_probe treats them identically) and only
  // flips to "Normal Airflow" for the fourth, proving normalising THROUGH
  // continuous boost is harmless and requires no special handling
  // (SetAirflowMode's own class comment, step 4).
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  RecordingLog log;
  status::StatusTracker status;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  feed(disp, status, "Boost Airflow", "48%       30m", clock.now);

  SetAirflowMode seq;
  seq.set_log_sink(log.as_log_sink());
  seq.set_status(&status);
  seq.configure(AirflowTarget::NORMAL);
  CHECK(runner.request(seq));

  // Line1 stays "Boost Airflow" (all three of Boost30/Boost60/Continuous
  // share it, per the comment above) until the 3rd normalising tap has
  // actually fired, at which point this flips it to Normal -- REACTIVELY,
  // not after a fixed delay. PROBE_CHECK re-checks on its own
  // NORMALISE_SETTLE_MS timer regardless of what the display shows, so a
  // fixed advance generous enough to cover one whole tap-settle-probe cycle
  // would risk letting a 4th tap fire before this test ever got a chance to
  // intervene -- checking every 20ms tick instead catches the 3rd tap's
  // first transmitted frame within one tick, comfortably before the
  // ~1.4s a full settle-then-reprobe cycle needs.
  bool flipped = false;
  while (runner.busy()) {
    clock.advance(20);
    if (!flipped && episodes_from(sink).size() >= 3) {
      feed(disp, status, "Normal Airflow", "18%", clock.now);
      flipped = true;
    }
  }

  CHECK(flipped);
  CHECK(!kp.busy());
  const auto episodes = episodes_from(sink);
  CHECK_EQ(episodes.size(), static_cast<size_t>(3));  // exactly the 3 normalising taps, no apply taps
  for (const auto mask : episodes) {
    CHECK_EQ(mask, MAIN);
  }
}

TEST_CASE(normalise_guard_caps_at_4_taps_and_fails_cleanly_without_ever_applying_the_target) {
  // A unit that never leaves "Boost Airflow" no matter how many Main taps
  // land -- the same class of failure AdjustField's own guard exists to
  // bound elsewhere in this component (PLAN.md risk 6: "bound the loop,
  // don't trust luck"). Proves the guard trips at exactly NORMALISE_GUARD
  // (4) taps and the sequence FAILS outright rather than proceeding to tap
  // the target's own press count on top of an unknown boost state -- see
  // after_probe_()'s own comment for why that would be worse than refusing.
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  RecordingLog log;
  status::StatusTracker status;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  feed(disp, status, "Boost Airflow", "48%       30m", clock.now);  // never changes again

  SetAirflowMode seq;
  seq.set_log_sink(log.as_log_sink());
  seq.set_status(&status);
  seq.configure(AirflowTarget::BOOST_30);  // the target is irrelevant here -- normalising never gets there
  CHECK(runner.request(seq));

  // 4 guard iterations, each up to a tap+key_gap (~450ms) plus the 1s
  // settle -- comfortably inside 12s even though every probe here resolves
  // immediately (line1 never changes, so PROBE_CHECK's own instant read
  // always answers "still boosting" without ever needing PROBE_WAIT's 8s).
  clock.advance(12000);

  CHECK(!runner.busy());
  CHECK(!kp.busy());
  CHECK(log.error_count >= 1);
  const auto episodes = episodes_from(sink);
  CHECK_EQ(episodes.size(), static_cast<size_t>(4));  // exactly the guard limit, then FAILED -- no 5th tap, no apply
  for (const auto mask : episodes) {
    CHECK_EQ(mask, MAIN);
  }
}

// ================================================== Purge -> Boost (risk 3) --

TEST_CASE(boost30_target_from_purging_start_cancels_first_then_probes_then_applies) {
  // PLAN.md risk 3's own worked example: "Purge -> Boost 30 is a 5.5s
  // cancel hold, an 8s probe, up to four normalising taps with probes, then
  // one tap." This simulates the cheapest case of that path -- the cancel
  // hold works first time and the unit is confirmed Normal on the very
  // first probe, so no normalising taps are needed at all, leaving exactly
  // two episodes: the cancel HOLD and the single apply TAP.
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  RecordingLog log;
  status::StatusTracker status;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  feed(disp, status, "Purge", "120 m", clock.now);

  SetAirflowMode seq;
  seq.set_log_sink(log.as_log_sink());
  seq.set_status(&status);
  seq.configure(AirflowTarget::BOOST_30);
  CHECK(runner.request(seq));

  clock.advance(5600);  // CHECK_CURRENT -> CANCEL_PURGE's fixed 5500ms hold clears
  // Simulate the unit having actually left Purge in response to the cancel
  // hold -- the real display would show this by now.
  feed(disp, status, "Normal Airflow", "18%", clock.now);
  clock.advance(500);   // CANCEL_SETTLE_MS (400ms) -- the explicit hold-to-tap gap
  clock.advance(8200);  // PROBE_TIMEOUT_MS (8000ms): the negative boost probe
  clock.advance(500);   // the single apply tap plus its key_gap

  CHECK(!runner.busy());
  CHECK(!kp.busy());
  const auto spans = episode_spans_from(sink);
  CHECK_EQ(spans.size(), static_cast<size_t>(2));
  CHECK_EQ(spans[0].mask, MAIN);
  CHECK(spans[0].last_ms - spans[0].first_ms > 5000);   // the cancel hold
  CHECK_EQ(spans[1].mask, MAIN);
  CHECK(spans[1].last_ms - spans[1].first_ms < 100);    // the apply tap -- see episode_spans_from()'s own comment
}

TEST_CASE(set_airflow_mode_refuses_to_guess_when_purge_is_still_showing_after_the_cancel_hold) {
  // Defensive branch (seq_set_airflow_mode.cpp's PROBE_CHECK): whether the
  // 5.5s cancel hold reliably clears Purge is untested on real hardware
  // (README "Portable core" / PLAN.md §8's "unvalidated against hardware").
  // If it somehow does not, tapping Main believing it is adjusting the
  // Normal/Boost counter would be guessing at an interaction nobody has
  // observed -- this proves that path fails loudly instead of guessing.
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  RecordingLog log;
  status::StatusTracker status;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  feed(disp, status, "Purge", "120 m", clock.now);  // never changes -- the cancel does not "take"

  SetAirflowMode seq;
  seq.set_log_sink(log.as_log_sink());
  seq.set_status(&status);
  seq.configure(AirflowTarget::BOOST_30);
  CHECK(runner.request(seq));

  clock.advance(6300);  // CANCEL_PURGE (5500ms) + CANCEL_SETTLE (400ms) + margin for PROBE_CHECK to run

  CHECK(!runner.busy());
  CHECK(!kp.busy());
  CHECK(log.error_count >= 1);
  // Exactly one episode: the cancel hold itself. Nothing was ever tapped
  // toward the target -- PROBE_CHECK's defensive re-check fails the run
  // before that.
  const auto episodes = episodes_from(sink);
  CHECK_EQ(episodes.size(), static_cast<size_t>(1));
  CHECK_EQ(episodes[0], MAIN);
}

// ============================================================ No hangs --
//
// "however it ends" (success, guard failure, timeout) -- success is covered
// by every CHECK(!kp.busy()) above, and guard/refusal failures by every
// CHECK(log.error_count >= 1) case (the menu/diagnostic-screen refusals, the
// two missing-status-tracker refusals, the still-purging-after-cancel
// refusal, and the 4-tap normalise guard). There is deliberately no
// dedicated ROOT-TIMEOUT test here: every RUNNING state in this sequence
// already carries its own bound (CANCEL_PURGE/OPEN_PURGE's fixed 5500ms,
// PROBE_WAIT's 8000ms, NORMALISE_SETTLE's 1000ms, and NORMALISE_WAIT/
// APPLY_WAIT resolving whenever Keypad's own queue drains, which it always
// eventually does), and the guard caps the one loop that could otherwise run
// long -- so timeout_ms()'s 90s root budget is unreachable by construction,
// not merely untested. See timeout_ms()'s own comment (sequence.h) for the
// arithmetic.
//
// Finding 3 (Opus review) -- the boost-60 latch and the during-run
// publish suppression -- both live in VentAxiaHub::publish_airflow_mode_()
// (vent_axia.cpp), which is compiled only for the firmware, not the host
// suite: per README "Portable core", vent_axia.cpp is the one file (besides
// the platform *.py files) allowed to include esphome/... headers, and
// tests/CMakeLists.txt's glob of component sources explicitly EXCLUDES it
// for exactly that reason. There is no way to reach either fix from this
// host test binary without linking ESPHome itself, so neither is covered
// here -- both were instead verified by compiling mhrv/mhrv.yaml and
// example/esp32-idf.yaml and by reading the diff.
