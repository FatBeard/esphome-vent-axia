#include "test_framework.h"

#include <functional>
#include <string>
#include <vector>

#include "sequence.h"
#include "sequence_test_helpers.h"

using namespace esphome::vent_axia;
using namespace vatest;

// Stage 7: SyncClock, plus the two new AdjustField pieces it introduces --
// TargetFn (a live, re-read-every-iteration target) and the wrapping
// direction functions (direction_wrap_24/60). Split into its own file rather
// than growing test_sequence.cpp, per tests/CMakeLists.txt's glob -- reuses
// that file's fake keypad/display harness via sequence_test_helpers.h.
//
// Several tests below drive AdjustField directly (with the clock's parsers/
// directions) rather than the whole SyncClock sequence -- same reasoning as
// test_sequence.cpp's own AdjustField tests: it is the more direct way to
// pin down exactly which of the two clock-specific pieces (the live target,
// the wrap direction) is under test, without also depending on GotoMenu/
// OpenEditor/LeaveMenu timing. The happy-path and abort-path tests exercise
// the whole SyncClock orchestration, where that timing does matter.

namespace {

/// Set Clock's line1, matching screens::classify's "Set Clock" prefix.
const std::string kSetClockLine1 = "Set Clock";

}  // namespace

// ===================================================== AdjustField (clock) --

TEST_CASE(day_field_does_not_wrap_from_sunday_to_monday) {
  // PLAN.md/sequence.h are explicit that the day field is the one place
  // direction_no_wrap (not direction_wrap_24/60) applies among the clock
  // fields: Up on Sun does nothing on the real unit, so getting from Sun to
  // Mon can only mean six DOWN taps, never a single wrapping Up.
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  disp.update(vatest::pad16(kSetClockLine1), vatest::pad16("Sun 12:00"), 0);

  AdjustField adjust;
  adjust.reset(
      parse_clock_day_field, direction_no_wrap, [](int &out) {
        out = 0;  // Mon
        return true;
      },
      8);
  CHECK(runner.request(adjust));

  // Sun(6) -> Sat(5) -> Fri(4) -> Thu(3) -> Wed(2) -> Tue(1) -> Mon(0): six
  // DOWN taps.
  const char *const days[] = {"Sat", "Fri", "Thu", "Wed", "Tue", "Mon"};
  for (const char *day : days) {
    clock.advance(300);  // let the queued tap fire
    disp.update(vatest::pad16(kSetClockLine1), vatest::pad16(std::string(day) + " 12:00"), clock.now);
    clock.advance(300);  // let AdjustField notice and loop back
  }
  clock.advance(200);

  CHECK(!runner.busy());
  CHECK(!kp.busy());
  const auto episodes = episodes_from(sink);
  CHECK_EQ(episodes.size(), static_cast<size_t>(6));
  for (const auto mask : episodes) {
    CHECK_EQ(mask, DOWN);  // never UP -- the day field does not wrap
  }
}

TEST_CASE(hour_field_takes_the_shortest_path_across_midnight) {
  // 23 -> 01 is two UP taps through midnight (23 -> 00 -> 01), never 22 DOWN
  // taps the long way round -- proves direction_wrap_24 actually consults
  // parser::wrapped_delta rather than falling back to a plain comparison.
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  disp.update(vatest::pad16(kSetClockLine1), vatest::pad16("Mon 23:00"), 0);

  AdjustField adjust;
  adjust.reset(
      parse_clock_hour_field, direction_wrap_24, [](int &out) {
        out = 1;
        return true;
      },
      14);
  CHECK(runner.request(adjust));

  clock.advance(300);
  disp.update(vatest::pad16(kSetClockLine1), vatest::pad16("Mon 00:00"), clock.now);
  clock.advance(300);
  disp.update(vatest::pad16(kSetClockLine1), vatest::pad16("Mon 01:00"), clock.now);
  // Extra margin (vs. the 300ms elsewhere): only two taps here, so unlike
  // the six-tap day-field test above there is little slack in the keypad's
  // own queue for the second tap's press+key_gap (~450ms) to fully clear by
  // a bare 300ms.
  clock.advance(700);

  CHECK(!runner.busy());
  CHECK(!kp.busy());
  const auto episodes = episodes_from(sink);
  CHECK_EQ(episodes.size(), static_cast<size_t>(2));
  CHECK_EQ(episodes[0], UP);
  CHECK_EQ(episodes[1], UP);
}

TEST_CASE(minute_field_follows_a_rollover_of_the_live_target_instead_of_chasing_a_stale_one) {
  // The whole reason AdjustField gained a TargetFn: the minute field can
  // take up to ~34 taps (~46s at ~1.35s each), long enough for a real
  // minute to roll over mid-adjustment. This changes the target PARTWAY
  // THROUGH the adjustment -- simulating that rollover -- and asserts the
  // sequence converges on the NEW value rather than the one it started
  // chasing, exactly as the old adjust_minute script's own comment promised
  // ("The target is re-read from Home Assistant on every iteration").
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  disp.update(vatest::pad16(kSetClockLine1), vatest::pad16("Mon 10:15"), 0);

  int target_minute = 16;
  AdjustField adjust;
  adjust.reset(
      parse_clock_minute_field, direction_wrap_60, [&target_minute](int &out) {
        out = target_minute;
        return true;
      },
      34);
  CHECK(runner.request(adjust));

  // First iteration: cur=15, want=16 -> one UP tap fires.
  clock.advance(300);

  // The rollover: before the unit has even responded to that first tap, the
  // live clock ticks forward, so the NEXT CHECK will see a different target
  // than the one this tap was aimed at.
  target_minute = 17;

  // The unit responds to the first (already-queued) tap: 15 -> 16.
  disp.update(vatest::pad16(kSetClockLine1), vatest::pad16("Mon 10:16"), clock.now);
  clock.advance(300);  // AdjustField re-CHECKs: cur=16, want=17 (the NEW target) -> taps UP again

  disp.update(vatest::pad16(kSetClockLine1), vatest::pad16("Mon 10:17"), clock.now);
  clock.advance(700);  // cur == want(17) -> DONE, plus margin for the 2nd tap's key_gap to clear -- see the hour test above

  CHECK(!runner.busy());
  CHECK(!kp.busy());
  const auto episodes = episodes_from(sink);
  // Two taps total: it followed the rollover to 17, it did not stop at the
  // stale target of 16, and it did not need a third tap to "correct" an
  // overshoot -- the live re-read means there never was one.
  CHECK_EQ(episodes.size(), static_cast<size_t>(2));
  CHECK_EQ(episodes[0], UP);
  CHECK_EQ(episodes[1], UP);
}

TEST_CASE(day_field_guard_limit_stops_a_stuck_field_at_8_taps) {
  // A unit that ignores every press looks, from here, identical to a
  // dropped-press storm -- the guard is what stops either from mashing keys
  // forever. Same shape as test_sequence.cpp's own
  // adjust_field_respects_its_guard_limit_rather_than_looping_forever: the
  // guard trips at exactly DAY_GUARD (8) taps, then the sequence itself
  // FAILs (as a directly-requested ROOT here) and Runner::recover() issues
  // its own single Up on top, since "Set Clock" is a menu screen with no
  // editor open by the time the guard trips.
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  disp.update(vatest::pad16(kSetClockLine1), vatest::pad16("Sun 12:00"), 0);  // never changes again

  AdjustField adjust;
  adjust.reset(
      parse_clock_day_field, direction_no_wrap, [](int &out) {
        out = 0;  // Mon -- never reached, line2 is frozen
        return true;
      },
      8);
  CHECK(runner.request(adjust));

  clock.advance(15000);  // 8 guard taps, each up to ~1.35s, plus recover()'s own tap, plus margin

  CHECK(!runner.busy());
  CHECK(!kp.busy());
  const auto episodes = episodes_from(sink);
  CHECK_EQ(episodes.size(), static_cast<size_t>(9));  // 8 guard taps + recover()'s single Up
  for (size_t i = 0; i < 8; i++) {
    CHECK_EQ(episodes[i], DOWN);  // Sun(6) > Mon(0): direction_no_wrap says DOWN throughout
  }
  CHECK_EQ(episodes[8], UP);
}

TEST_CASE(hour_field_guard_limit_stops_a_stuck_field_at_14_taps) {
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  disp.update(vatest::pad16(kSetClockLine1), vatest::pad16("Mon 23:00"), 0);  // never changes again

  AdjustField adjust;
  adjust.reset(
      parse_clock_hour_field, direction_wrap_24, [](int &out) {
        out = 0;
        return true;
      },
      14);
  CHECK(runner.request(adjust));

  clock.advance(24000);  // 14 guard taps at up to ~1.35s each, plus recover(), plus margin

  CHECK(!runner.busy());
  CHECK(!kp.busy());
  const auto episodes = episodes_from(sink);
  CHECK_EQ(episodes.size(), static_cast<size_t>(15));  // 14 guard taps + recover()'s single Up
}

TEST_CASE(minute_field_guard_limit_stops_a_stuck_field_at_34_taps) {
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  disp.update(vatest::pad16(kSetClockLine1), vatest::pad16("Mon 10:30"), 0);  // never changes again

  AdjustField adjust;
  adjust.reset(
      parse_clock_minute_field, direction_wrap_60, [](int &out) {
        out = 0;
        return true;
      },
      34);
  CHECK(runner.request(adjust));

  clock.advance(55000);  // 34 guard taps at up to ~1.35s each, plus recover(), plus margin

  CHECK(!runner.busy());
  CHECK(!kp.busy());
  const auto episodes = episodes_from(sink);
  CHECK_EQ(episodes.size(), static_cast<size_t>(35));  // 34 guard taps + recover()'s single Up
}

TEST_CASE(mid_blink_clock_frame_is_not_partially_decoded_or_counted_against_the_guard) {
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  // Day field blanked mid-blink -- "    12:00", not "Sun 12:00" -- must be
  // rejected outright (parser::clock_rendered fails on the non-alpha day),
  // not partially decoded into a bogus day index.
  disp.update(vatest::pad16(kSetClockLine1), vatest::pad16("    12:00"), 0);

  AdjustField adjust;
  adjust.reset(
      parse_clock_day_field, direction_no_wrap, [](int &out) {
        out = 0;
        return true;
      },
      8);
  CHECK(runner.request(adjust));

  // Several ticks with only the never-valid frame present: must stay
  // RUNNING, tap nothing, and not fail -- a parse failure is "wait for the
  // next frame", never an error and never a guard-counted attempt.
  clock.advance(2000);
  CHECK(runner.busy());
  CHECK(episodes_from(sink).empty());

  // Now the field settles on its real (frozen -- never reaching Mon) value.
  // The guard must still take exactly 8 real taps from here -- proving the
  // blank frames above contributed nothing to guard_count_, same as the
  // dedicated day-guard test above.
  disp.update(vatest::pad16(kSetClockLine1), vatest::pad16("Sun 12:00"), clock.now);
  clock.advance(15000);

  CHECK(!runner.busy());
  CHECK(!kp.busy());
  const auto episodes = episodes_from(sink);
  CHECK_EQ(episodes.size(), static_cast<size_t>(9));  // 8 guard taps + recover()'s single Up
  for (size_t i = 0; i < 8; i++) {
    CHECK_EQ(episodes[i], DOWN);
  }
  CHECK_EQ(episodes[8], UP);
}

// =========================================================== SyncClock --

TEST_CASE(sync_clock_corrects_all_three_fields_with_exactly_four_sets_and_leaves_the_display_alone) {
  // End to end: lands on Set Clock, opens the editor, corrects day (0 taps
  // needed -- already right), hour (23 -> 00, one UP) and minute (58 -> 59,
  // one UP), four Sets total (open + advance + advance + commit), then
  // leaves via LeaveMenu -- one more Up -- and ends with the display alone
  // and no key held. Every tap-producing transition is synchronised by
  // polling for the next transmitted episode rather than a fixed delay, so
  // this is not sensitive to exactly how many ticks Keypad's own queue (tap
  // + mandatory key_gap) takes to drain -- same reasoning as
  // write_setting_runs_navigate_open_adjust_commit_exit_readback_in_order's
  // own "before/while" idiom in test_sequence.cpp.
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  RecordingLog log;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  SyncClock sync_clock;
  runner.set_log_sink(log.as_log_sink());
  sync_clock.set_time_source([](int &dow_display, int &hour, int &minute) {
    dow_display = 0;  // Mon
    hour = 0;
    minute = 59;
    return true;
  });

  CHECK(runner.request(sync_clock));

  // Waits until at least one more episode has been transmitted than there
  // was at entry, in small (20ms) steps -- robust to exactly how long
  // Keypad's own queue takes to actually fire a just-tapped key. Used
  // throughout instead of a fixed delay: NAVIGATE/VERIFY/CHECK_TIME above
  // all finish well inside whatever generous fixed budget a test gives
  // them, so measuring OPEN's own Set tap from a fixed point risks missing
  // it -- OpenEditor's internal TAP -> WAIT_TAP -> SETTLE -> CHECK path
  // (~1150ms) can run to completion, see its own CHECK fail on a display
  // this test hasn't updated yet, and silently retry before this test's
  // code ever gets a chance to react.
  const auto wait_for_next_episode = [&]() {
    const size_t before = episodes_from(sink).size();
    while (episodes_from(sink).size() == before) {
      clock.advance(20);
    }
  };

  // NAVIGATE (GotoMenu(1)) never looks at the display; provide the arrival
  // screen well after navigation starts so VERIFY's freshness check is
  // satisfied for the rest of the run.
  clock.advance(100);
  disp.update(vatest::pad16(kSetClockLine1), vatest::pad16("Mon 23:58"), clock.now);

  // Wait for GotoMenu(1)'s own 6 episodes (5 Up, 1 Down) precisely, rather
  // than a fixed budget that could run long enough to let OPEN's own Set
  // tap -- and even its one retry -- fire before this test is watching.
  while (episodes_from(sink).size() < 6) {
    clock.advance(20);
  }
  clock.advance(300);  // let VERIFY, then CHECK_TIME, notice -- neither touches the keypad

  // OPEN (OpenEditor): catch the Set tap's own first transmitted frame the
  // moment it happens, then wait ~500ms -- comfortably inside OpenEditor's
  // own ~450ms (tap+key_gap) + 700ms (settle) = ~1150ms path to its CHECK --
  // before blinking the day field open, so that update is neither so early
  // it has gone stale past Display::editor_open()'s 1200ms settle window by
  // the time CHECK runs, nor so late it arrives after CHECK already looked.
  wait_for_next_episode();
  clock.advance(500);
  disp.update(vatest::pad16(kSetClockLine1), vatest::pad16("    23:58"), clock.now);  // day blanked -- editing it
  clock.advance(900);  // past OpenEditor's 700ms settle -- editor_open() now reads true

  // ADJUST_DAY: the day field resolves back to its real (already-correct)
  // value -- zero taps needed, straight through to SET_DAY's own Set.
  disp.update(vatest::pad16(kSetClockLine1), vatest::pad16("Mon 23:58"), clock.now);
  wait_for_next_episode();  // SET_DAY

  // ADJUST_HOUR: hour field blanks, then resolves to its real (23, wrong)
  // value -- one UP tap (23 -> 00, the shortest path across midnight).
  disp.update(vatest::pad16(kSetClockLine1), vatest::pad16("Mon   :58"), clock.now);
  clock.advance(50);
  disp.update(vatest::pad16(kSetClockLine1), vatest::pad16("Mon 23:58"), clock.now);
  wait_for_next_episode();  // the UP tap
  disp.update(vatest::pad16(kSetClockLine1), vatest::pad16("Mon 00:58"), clock.now);
  wait_for_next_episode();  // AdjustField notices cur == want, DONE -> SET_HOUR

  // ADJUST_MINUTE: minute field blanks, then resolves to its real (58,
  // wrong) value -- one UP tap (58 -> 59).
  disp.update(vatest::pad16(kSetClockLine1), vatest::pad16("Mon 00:  "), clock.now);
  clock.advance(50);
  disp.update(vatest::pad16(kSetClockLine1), vatest::pad16("Mon 00:58"), clock.now);
  wait_for_next_episode();  // the UP tap
  disp.update(vatest::pad16(kSetClockLine1), vatest::pad16("Mon 00:59"), clock.now);
  wait_for_next_episode();  // AdjustField notices cur == want, DONE -> COMMIT, the fourth and final Set

  // SETTLE (700ms): the unit is back on the (non-editing) Set Clock screen.
  disp.update(vatest::pad16(kSetClockLine1), vatest::pad16("Mon 00:59"), clock.now);
  clock.advance(800);

  // LEAVE (LeaveMenu): one Up tap, then -- once the keypad is idle again
  // (its own tap plus mandatory key_gap) -- it notices the display has left
  // the menu once the unit's own navigation takes effect.
  wait_for_next_episode();
  disp.update(vatest::pad16("18%"), vatest::pad16(""), clock.now);
  clock.advance(700);

  CHECK(!runner.busy());
  CHECK(!kp.busy());  // no key left asserted
  const auto episodes = episodes_from(sink);
  for (const auto mask : episodes) {
    CHECK(mask != MAIN);  // Main is never pressed anywhere in this sequence -- on this unit it is Boost
  }

  // Every value chosen above (day already correct; hour and minute each one
  // tap away) makes the whole run's episode sequence fully deterministic --
  // OPEN's Set and SET_DAY's very next Set are otherwise indistinguishable
  // in the transmitted stream (both plain SET episodes, nothing between
  // them, since ADJUST_DAY needs zero taps), so this asserts exact indices
  // rather than a tolerant "run of one or more" the way
  // write_setting_runs_navigate_open_adjust_commit_exit_readback_in_order
  // does for a field that genuinely might need a variable number of taps.
  CHECK_EQ(episodes.size(), static_cast<size_t>(13));
  // NAVIGATE: GotoMenu(1) -- 5 Up, then 1 Down.
  for (size_t i = 0; i < 5; i++) {
    CHECK_EQ(episodes[i], UP);
  }
  CHECK_EQ(episodes[5], DOWN);
  CHECK_EQ(episodes[6], SET);   // OPEN
  CHECK_EQ(episodes[7], SET);   // SET_DAY (ADJUST_DAY needed zero taps)
  CHECK_EQ(episodes[8], UP);    // ADJUST_HOUR: 23 -> 00
  CHECK_EQ(episodes[9], SET);   // SET_HOUR
  CHECK_EQ(episodes[10], UP);   // ADJUST_MINUTE: 58 -> 59
  CHECK_EQ(episodes[11], SET);  // COMMIT -- the fourth and last Set
  CHECK_EQ(episodes[12], UP);   // LEAVE
}

TEST_CASE(sync_clock_presses_set_not_up_when_the_commit_set_is_dropped_and_the_editor_stays_open) {
  // Finding 1: SyncClock's LEAVE step runs right after the fourth (commit)
  // Set. If THAT Set was dropped, the editor is still open, and the old
  // code's unconditional LeaveMenu Up would adjust the minute field instead
  // of navigating out -- PLAN.md §3's 14C->19C failure, and worse here
  // because the run would then report DONE having silently written a wrong
  // time. This test keeps line2 blinking (~350ms period, the same rate
  // Display::editor_open()'s own class comment documents for a real open
  // editor) straight through SETTLE's now-1800ms window, simulating exactly
  // that dropped commit, and asserts every key transmitted from the commit
  // onward is a Set (EXIT_CHAIN walking the still-open editor closed with
  // more commits) and NEVER an Up, until the editor is actually observed to
  // close -- at which point LeaveMenu's own single Up follows. Revert
  // either the LeaveMenu::TAP guard or SyncClock's EXIT_CHAIN step and this
  // fails: the pre-fix code taps Up right after SETTLE regardless of
  // editor_open().
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  RecordingLog log;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  SyncClock sync_clock;
  runner.set_log_sink(log.as_log_sink());
  sync_clock.set_time_source([](int &dow_display, int &hour, int &minute) {
    // Matches the display below exactly, so day/hour/minute each need ZERO
    // adjust taps -- gets to COMMIT (the fourth Set) via the shortest
    // possible path, same trick the day field uses in the happy-path test
    // above, applied to all three fields here to keep this test focused on
    // what happens AFTER the commit rather than on AdjustField's own taps.
    dow_display = 0;  // Mon
    hour = 10;
    minute = 30;
    return true;
  });

  CHECK(runner.request(sync_clock));

  const auto wait_for_next_episode = [&]() {
    const size_t before = episodes_from(sink).size();
    while (episodes_from(sink).size() == before) {
      clock.advance(20);
    }
  };

  // NAVIGATE, VERIFY, CHECK_TIME.
  clock.advance(100);
  disp.update(vatest::pad16(kSetClockLine1), vatest::pad16("Mon 10:30"), clock.now);
  while (episodes_from(sink).size() < 6) {
    clock.advance(20);
  }
  clock.advance(300);

  // OPEN: catch the Set tap, then blink the day field open.
  wait_for_next_episode();
  clock.advance(500);
  disp.update(vatest::pad16(kSetClockLine1), vatest::pad16("    10:30"), clock.now);
  clock.advance(900);

  // ADJUST_DAY: already correct -- zero taps, straight to SET_DAY's Set.
  disp.update(vatest::pad16(kSetClockLine1), vatest::pad16("Mon 10:30"), clock.now);
  wait_for_next_episode();  // SET_DAY

  // ADJUST_HOUR: already correct -- zero taps, straight to SET_HOUR's Set.
  disp.update(vatest::pad16(kSetClockLine1), vatest::pad16("Mon   :30"), clock.now);
  clock.advance(50);
  disp.update(vatest::pad16(kSetClockLine1), vatest::pad16("Mon 10:30"), clock.now);
  wait_for_next_episode();  // SET_HOUR

  // ADJUST_MINUTE: already correct -- zero taps, straight to COMMIT's Set,
  // the fourth and final one -- except, in spirit, it's about to be dropped:
  // the unit below never actually settles, simulating exactly that.
  disp.update(vatest::pad16(kSetClockLine1), vatest::pad16("Mon 10:  "), clock.now);
  clock.advance(50);
  disp.update(vatest::pad16(kSetClockLine1), vatest::pad16("Mon 10:30"), clock.now);
  wait_for_next_episode();  // COMMIT

  const size_t episodes_after_commit = episodes_from(sink).size();

  // SETTLE, then EXIT_CHAIN: keep line2 blinking (still "editing") straight
  // through both SETTLE's 1800ms wait and EXIT_CHAIN's own first check, so
  // editor_open() reads true the whole time. Loop until the NEXT episode
  // (EXIT_CHAIN's own commit Set) is actually transmitted, rather than a
  // fixed budget -- robust to exactly how many ticks COMMIT/SETTLE take.
  bool blink_on = true;
  while (episodes_from(sink).size() == episodes_after_commit) {
    clock.advance(350);
    disp.update(vatest::pad16(kSetClockLine1), vatest::pad16(blink_on ? "Mon 10:30" : "    10:30"), clock.now);
    blink_on = !blink_on;
  }

  // This is the finding: the key issued while the editor was (simulated)
  // still open must be a Set, never an Up.
  const auto mid_episodes = episodes_from(sink);
  CHECK(mid_episodes.size() > episodes_after_commit);
  for (size_t i = episodes_after_commit; i < mid_episodes.size(); i++) {
    CHECK(mid_episodes[i] != UP);
  }
  CHECK_EQ(mid_episodes.back(), SET);  // ExitEditChain's own commit, not LeaveMenu's Up

  // Now let the editor actually close (the retry commit landed for real):
  // line2 goes quiet. ExitEditChain's own COMMIT_SETTLE_MS (1800ms) plus its
  // own key_gap elapse, its next CHECK finds editor_open() false and
  // finishes; LEAVE follows with its single, now-safe Up.
  disp.update(vatest::pad16(kSetClockLine1), vatest::pad16("Mon 10:30"), clock.now);
  // NOT a fixed clock.advance() here -- wait_for_next_episode() itself
  // advances in small steps until the next episode (LEAVE's Up, once
  // ExitEditChain's own CHECK finds editor_open() false) actually fires.
  // A big fixed advance first would let that Up happen UNWATCHED, so the
  // very next wait_for_next_episode() call would then wait for a SECOND
  // episode that will never come on its own -- exactly the kind of
  // off-by-one that would misreport LeaveMenu's own WAIT_EXIT timeout (and
  // Runner::recover()'s consequent Up) as this test passing for the wrong
  // reason.
  wait_for_next_episode();  // LEAVE's Up
  disp.update(vatest::pad16("18%"), vatest::pad16(""), clock.now);
  clock.advance(700);

  CHECK(!runner.busy());
  CHECK(!kp.busy());
  const auto episodes = episodes_from(sink);
  // From the commit to the very last episode: only Sets, until exactly one
  // final Up -- never an Up while the editor could still have been open.
  for (size_t i = episodes_after_commit; i + 1 < episodes.size(); i++) {
    CHECK_EQ(episodes[i], SET);
  }
  CHECK_EQ(episodes.back(), UP);
}

TEST_CASE(sync_clock_settled_commit_does_not_trigger_a_spurious_exit_chain) {
  // Finding 1a's false-positive guard: a SUCCESSFUL commit also changes
  // line2 (the editor closing repaints the settled screen), so gating
  // EXIT_CHAIN on too-short a settle window would misread that as "still
  // open" and run ExitEditChain needlessly -- which, per SETTLE_MS's own
  // comment, risks landing a Set on an already-settled Set Clock screen and
  // RE-OPENING the day editor, leaving things worse than doing nothing.
  // This lets line2 go quiet right after the commit (the real, common
  // case), advances past the 1800ms settle window, and asserts the ONLY key
  // issued afterwards is LeaveMenu's single Up -- no Set. Complements the
  // dropped-commit test above, which proves the opposite branch.
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  RecordingLog log;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  SyncClock sync_clock;
  runner.set_log_sink(log.as_log_sink());
  sync_clock.set_time_source([](int &dow_display, int &hour, int &minute) {
    dow_display = 0;  // Mon
    hour = 10;
    minute = 30;
    return true;
  });

  CHECK(runner.request(sync_clock));

  const auto wait_for_next_episode = [&]() {
    const size_t before = episodes_from(sink).size();
    while (episodes_from(sink).size() == before) {
      clock.advance(20);
    }
  };

  clock.advance(100);
  disp.update(vatest::pad16(kSetClockLine1), vatest::pad16("Mon 10:30"), clock.now);
  while (episodes_from(sink).size() < 6) {
    clock.advance(20);
  }
  clock.advance(300);

  wait_for_next_episode();  // OPEN
  clock.advance(500);
  disp.update(vatest::pad16(kSetClockLine1), vatest::pad16("    10:30"), clock.now);
  clock.advance(900);

  disp.update(vatest::pad16(kSetClockLine1), vatest::pad16("Mon 10:30"), clock.now);
  wait_for_next_episode();  // SET_DAY

  disp.update(vatest::pad16(kSetClockLine1), vatest::pad16("Mon   :30"), clock.now);
  clock.advance(50);
  disp.update(vatest::pad16(kSetClockLine1), vatest::pad16("Mon 10:30"), clock.now);
  wait_for_next_episode();  // SET_HOUR

  disp.update(vatest::pad16(kSetClockLine1), vatest::pad16("Mon 10:  "), clock.now);
  clock.advance(50);
  disp.update(vatest::pad16(kSetClockLine1), vatest::pad16("Mon 10:30"), clock.now);
  wait_for_next_episode();  // COMMIT

  const size_t episodes_after_commit = episodes_from(sink).size();

  // The unit actually closed the editor: line2 settles on the final value
  // and goes quiet, exactly as SETTLE's own case comment describes ("a
  // successful commit also changes line2"). No further disp.update() calls
  // from here, so by the time SETTLE's 1800ms elapse and EXIT_CHAIN's CHECK
  // runs, editor_open() reads false and it falls straight through to LEAVE
  // without ever transmitting anything -- the only new episode is
  // LeaveMenu's own Up. wait_for_next_episode() itself advances in small
  // steps until that happens, rather than a fixed advance first (which
  // would let it happen unwatched -- see the equivalent comment in the
  // dropped-commit test above for why that matters).
  wait_for_next_episode();
  disp.update(vatest::pad16("18%"), vatest::pad16(""), clock.now);
  clock.advance(700);

  CHECK(!runner.busy());
  CHECK(!kp.busy());
  const auto episodes = episodes_from(sink);
  CHECK_EQ(episodes.size(), episodes_after_commit + 1);
  CHECK_EQ(episodes.back(), UP);  // LeaveMenu's single Up -- no ExitEditChain Set in between
}

TEST_CASE(sync_clock_aborts_without_pressing_set_when_set_clock_screen_never_appears) {
  // Abort path (a): the display never shows Set Clock at all (simulating
  // the unit not responding to navigation). VERIFY's own 3000ms budget
  // fails it; recover() finds a non-menu screen (the default, unset
  // Display) and so does nothing further -- GotoMenu's own navigation taps
  // are the only thing transmitted.
  Keypad kp;
  Display disp;  // default line1 == "" -- classifies as STATUS, never Set Clock
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  RecordingLog log;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  SyncClock sync_clock;
  runner.set_log_sink(log.as_log_sink());
  sync_clock.set_time_source([](int &dow_display, int &hour, int &minute) {
    dow_display = 0;
    hour = 0;
    minute = 0;
    return true;
  });

  CHECK(runner.request(sync_clock));
  clock.advance(8000);  // GotoMenu(1) (~3.7s) + VERIFY's 3000ms budget, comfortably

  CHECK(!runner.busy());
  CHECK(!kp.busy());
  CHECK(log.warn_count >= 1);
  const auto episodes = episodes_from(sink);
  for (const auto mask : episodes) {
    CHECK(mask != SET);  // never opened an editor -- nothing was ever adjusted or committed
  }
}

TEST_CASE(sync_clock_aborts_without_pressing_set_when_line2_never_renders_a_valid_clock) {
  // Abort path (b): line1 correctly reaches Set Clock, but line2 never
  // produces a frame parse_clock_day_field (or any of the three) accepts --
  // simulating a display stuck mid-blink or otherwise unreadable.
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  RecordingLog log;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  SyncClock sync_clock;
  runner.set_log_sink(log.as_log_sink());
  sync_clock.set_time_source([](int &dow_display, int &hour, int &minute) {
    dow_display = 0;
    hour = 0;
    minute = 0;
    return true;
  });

  CHECK(runner.request(sync_clock));
  clock.advance(100);
  // Set Clock screen shown, but line2 is garbage -- never a valid clock
  // rendering -- for the rest of the run.
  disp.update(vatest::pad16(kSetClockLine1), vatest::pad16(""), clock.now);
  clock.advance(9000);  // GotoMenu(1) + VERIFY's own 3000ms budget + recover()'s single Up clearing, comfortably

  CHECK(!runner.busy());
  CHECK(!kp.busy());
  CHECK(log.warn_count >= 1);
  const auto episodes = episodes_from(sink);
  for (const auto mask : episodes) {
    CHECK(mask != SET);
  }
}

TEST_CASE(sync_clock_aborts_and_transmits_no_set_when_the_time_source_is_unavailable) {
  // Abort path (c): the screen and clock reading are both fine, but there is
  // no usable time source (no `time_id` configured, USE_TIME undefined, or
  // the clock has not synced yet -- all collapse to the same "unavailable"
  // false return). CHECK_TIME must fail BEFORE OPEN ever runs, so -- unlike
  // (a) and (b), where GotoMenu's own navigation is unavoidable -- this is
  // the one path where "no Set was ever transmitted" is the whole point:
  // OPEN is the first step capable of pressing Set at all, and CHECK_TIME is
  // strictly upstream of it.
  Keypad kp;
  Display disp;
  Runner runner(kp, disp);
  runner.set_link_up(true);
  RecordingSink sink;
  RecordingLog log;
  Clock clock{kp, runner};
  sink.current_now = &clock.now;
  kp.set_frame_sink(sink.as_frame_sink());

  SyncClock sync_clock;
  runner.set_log_sink(log.as_log_sink());
  sync_clock.set_time_source([](int &, int &, int &) { return false; });  // no time source at all

  CHECK(runner.request(sync_clock));
  clock.advance(100);
  disp.update(vatest::pad16(kSetClockLine1), vatest::pad16("Mon 12:00"), clock.now);
  clock.advance(4000);  // GotoMenu(1)
  clock.advance(300);   // VERIFY passes (the clock reading is fine) -- CHECK_TIME fails right after

  CHECK(!runner.busy());
  CHECK(!kp.busy());
  CHECK(log.error_count >= 1);  // CHECK_TIME's own fail-fast log
  size_t set_count = 0;
  for (const auto mask : episodes_from(sink)) {
    if (mask == SET) {
      set_count++;
    }
  }
  CHECK_EQ(set_count, static_cast<size_t>(0));  // no Set was ever transmitted
}
