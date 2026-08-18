#include "test_framework.h"

#include "status.h"

using namespace esphome::vent_axia::status;

// -------------------------------------------------------- classify_line --

TEST_CASE(classify_line_matches_all_seven_messages_case_insensitively) {
  CHECK(classify_line("Normal Airflow  ") == LineMessage::NORMAL_AIRFLOW);
  CHECK(classify_line("low airflow     ") == LineMessage::LOW_AIRFLOW);
  CHECK(classify_line("Boost Airflow   ") == LineMessage::BOOST_AIRFLOW);
  CHECK(classify_line("CHECK FILTER    ") == LineMessage::CHECK_FILTER);
  // Exactly 16 chars, fills the line -- see status.h.
  CHECK(classify_line("Summer Bypass On") == LineMessage::SUMMER_BYPASS_ON);
  CHECK(classify_line("Dryout Mode     ") == LineMessage::DRYOUT_MODE);
  CHECK(classify_line("defrost active  ") == LineMessage::DEFROST_ACTIVE);
}

TEST_CASE(classify_line_returns_none_for_unrecognised_or_blank_text) {
  CHECK(classify_line("18%             ") == LineMessage::NONE);
  CHECK(classify_line("Set Clock       ") == LineMessage::NONE);
  CHECK(classify_line("                ") == LineMessage::NONE);
  CHECK(classify_line("") == LineMessage::NONE);
}

// ---------------------------------------------------- parse_line_values --
// The status-line grammar, covering every documented line2 form.

TEST_CASE(parse_line_values_plain_airflow_percentage) {
  const auto v = parse_line_values("Normal Airflow  ", "18%             ");
  CHECK(v.airflow_percent.has_value());
  CHECK_EQ(*v.airflow_percent, 18);
  CHECK(!v.countdown_minutes.has_value());
  CHECK(!v.purge);
}

TEST_CASE(parse_line_values_boost_30_minutes) {
  const auto v = parse_line_values("Boost Airflow   ", "48%       30m   ");
  CHECK(v.airflow_percent.has_value());
  CHECK_EQ(*v.airflow_percent, 48);
  CHECK(v.countdown_minutes.has_value());
  CHECK_EQ(*v.countdown_minutes, 30);
  CHECK(!v.purge);
}

TEST_CASE(parse_line_values_boost_60_minutes) {
  const auto v = parse_line_values("Boost Airflow   ", "48%       60m   ");
  CHECK_EQ(*v.airflow_percent, 48);
  CHECK_EQ(*v.countdown_minutes, 60);
  CHECK(!v.purge);
}

TEST_CASE(parse_line_values_purge_with_100_percent_line2_and_purge_line1) {
  // Layout is genuinely unresolved (status.h) -- this is one guess at it.
  const auto v = parse_line_values("Purge      120 m", "100%            ");
  CHECK(v.airflow_percent.has_value());
  CHECK_EQ(*v.airflow_percent, 100);
  CHECK(v.countdown_minutes.has_value());
  CHECK_EQ(*v.countdown_minutes, 120);
  CHECK(v.purge);
}

TEST_CASE(parse_line_values_purge_with_lines_swapped) {
  // The other guess at the layout -- must parse identically either way,
  // which is the entire point of scanning both lines instead of assuming.
  const auto v = parse_line_values("100%            ", "Purge      120 m");
  CHECK(v.airflow_percent.has_value());
  CHECK_EQ(*v.airflow_percent, 100);
  CHECK(v.countdown_minutes.has_value());
  CHECK_EQ(*v.countdown_minutes, 120);
  CHECK(v.purge);
}

TEST_CASE(parse_line_values_finds_nothing_on_a_menu_screen) {
  const auto v = parse_line_values("Set Clock       ", "Sun 23:49       ");
  CHECK(!v.airflow_percent.has_value());
  CHECK(!v.countdown_minutes.has_value());
  CHECK(!v.purge);
}

// ------------------------------------------------------- StatusTracker --

TEST_CASE(status_tracker_has_no_state_before_any_status_screen_frame) {
  StatusTracker t;
  CHECK(!t.has_state());
  CHECK(!t.boosting().has_value());
  CHECK(!t.summer_bypass().has_value());
  CHECK(!t.airflow_percent().has_value());
}

TEST_CASE(status_tracker_reports_false_for_a_flag_never_seen_once_status_is_known) {
  StatusTracker t;
  t.update("Normal Airflow  ", "18%             ", true, 0);
  CHECK(t.has_state());
  CHECK(t.boosting().has_value());
  CHECK(!*t.boosting());  // known, and definitely not boosting
}

TEST_CASE(status_tracker_flag_stays_true_across_an_alternation_where_it_is_invisible) {
  StatusTracker t;
  t.update("Boost Airflow   ", "48%       30m   ", true, 0);
  CHECK(*t.boosting());

  // The loop alternates away to the other half of the cycle -- boosting must
  // still read true even though this frame doesn't mention it.
  t.update("18%             ", "48%       30m   ", true, 1700);
  CHECK(*t.boosting());

  t.update("Boost Airflow   ", "48%       30m   ", true, 3400);
  CHECK(*t.boosting());
}

TEST_CASE(status_tracker_flag_ages_out_after_its_timeout_while_continuously_on_status) {
  StatusTracker t;
  t.update("Boost Airflow   ", "48%       30m   ", true, 0);
  CHECK(*t.boosting());

  // Comfortably under ALTERNATION_TIMEOUT_MS: still true.
  t.update("18%             ", "18%             ", true, 11000);
  CHECK(*t.boosting());

  // Now over it, still never having seen "Boost Airflow" again.
  t.update("18%             ", "18%             ", true, 12100);
  CHECK(!*t.boosting());
}

TEST_CASE(status_tracker_bypass_uses_its_own_longer_timeout) {
  StatusTracker t;
  t.update("Summer Bypass On", "18%             ", true, 0);
  CHECK(*t.summer_bypass());

  // Past the general alternation timeout but well inside bypass's 45s one.
  t.update("Normal Airflow  ", "18%             ", true, 20000);
  CHECK(*t.summer_bypass());

  t.update("Normal Airflow  ", "18%             ", true, 46000);
  CHECK(!*t.summer_bypass());
}

TEST_CASE(status_tracker_does_not_age_a_flag_out_while_parked_on_a_menu_screen) {
  StatusTracker t;
  t.update("Boost Airflow   ", "48%       30m   ", true, 0);
  CHECK(*t.boosting());

  // A diagnostics fetch or clock sync parks the display in a menu for well
  // over ALTERNATION_TIMEOUT_MS. is_status_screen is false throughout.
  t.update("Diagnostic  05  ", "0000            ", false, 5000);
  t.update("Diagnostic  06  ", "0000            ", false, 15000);
  t.update("Set Clock       ", "Sun 23:49       ", false, 30000);
  CHECK(*t.boosting());  // must not have aged out purely from wall-clock time

  // Back on the status screen immediately afterwards, still not showing
  // "Boost Airflow" this particular frame (it may be mid-alternation) -- the
  // flag must still read true, because only ~3.4s of *visible* status-screen
  // time (the pre-park stretch) has actually elapsed against its budget.
  t.update("18%             ", "48%       30m   ", true, 30100);
  CHECK(*t.boosting());
}

TEST_CASE(status_tracker_still_ages_out_after_returning_from_a_park_if_enough_visible_time_passes) {
  StatusTracker t;
  t.update("Boost Airflow   ", "48%       30m   ", true, 0);
  CHECK(*t.boosting());

  t.update("Diagnostic  05  ", "0000            ", false, 5000);  // parked
  t.update("18%             ", "18%             ", true, 5100);  // resumes, not boosting
  CHECK(*t.boosting());  // only ~100ms of visible time since the sighting

  // Accrue well past ALTERNATION_TIMEOUT_MS of genuine on-status time.
  t.update("18%             ", "18%             ", true, 20000);
  CHECK(!*t.boosting());
}

TEST_CASE(status_tracker_purging_flag_from_purge_keyword_on_either_line) {
  StatusTracker t;
  t.update("Purge      120 m", "100%            ", true, 0);
  CHECK(t.purging().has_value());
  CHECK(*t.purging());
  CHECK(*t.airflow_percent() == 100);
  CHECK(*t.boost_time_remaining() == 120);
}

TEST_CASE(status_tracker_boost_time_remaining_unpublished_without_a_countdown) {
  StatusTracker t;
  // A percentage with no countdown anywhere -- boost_time_remaining must
  // stay unpublished regardless of whether this turns out to be a genuine
  // continuous episode or just the first frame of one: that is still the
  // documented, deliberate behaviour (there really is no countdown to
  // report), not a bug. What CHANGED 13 Aug 2026 (see the plan this shipped
  // under) is that continuous boost is no longer left undecoded elsewhere --
  // continuous_boost() answers that question separately, and on a single
  // frame like this one it correctly answers "not yet confirmed" rather than
  // guessing ahead of CONTINUOUS_CONFIRM_MS -- see the dedicated
  // continuous_boost() tests below for the confirmed case.
  t.update("Boost Airflow   ", "48%             ", true, 0);
  CHECK(*t.boosting());
  CHECK(!t.boost_time_remaining().has_value());
  CHECK(t.continuous_boost().has_value());
  CHECK(!*t.continuous_boost());
}

// ------------------------------------------------------- humidity_boost --
// Decodes line2 column 15's glyphs::ALPHA (0xE0, measured live PLAN.md §8
// stage 15) -- the manual's alpha annunciator for a proportional 0-10V
// sensor or the internal humidity sensor boosting airflow (PLAN.md §4/§8
// stage 14). The regression that matters most here is telling this apart
// from stage 10's `ls`, which sits in the same right-hand zone of line2 but
// is a structurally different signal.

TEST_CASE(has_sensor_boost_annunciator_true_for_the_measured_byte_at_column_15) {
  // The captured frame this decode was built from: line1 "Summer Bypass On",
  // line2 "31%            *" as published under the pre-stage-16
  // sanitize()'d pipeline -- the raw byte underneath, confirmed live 18 Aug
  // 2026 (PLAN.md §8 stage 15), is 0xE0. The percentage is incidental to
  // what is being tested here -- see has_sensor_boost_annunciator()'s
  // comment for why the "31% is a proportional rate between Normal and
  // Boost" reading was withdrawn on 17 Aug 2026 (18% is the LOW rate;
  // Normal measured 30%). Only column 15 matters, whatever precedes it.
  CHECK(has_sensor_boost_annunciator("31%            \xE0"));
}

TEST_CASE(has_sensor_boost_annunciator_false_on_a_plain_status_frame) {
  CHECK(!has_sensor_boost_annunciator("18%             "));
}

TEST_CASE(has_sensor_boost_annunciator_false_for_stage_10s_ls_regression) {
  // THE KEY REGRESSION. Stage 10's switched-live annunciator occupies
  // columns 14-15 ('l' then 's'), one column to the left of and including
  // column 15 -- but column 15 itself holds 's' (0x73), not glyphs::ALPHA
  // (0xE0), so a check of line2[15] alone cannot confuse the two even
  // though both live in the same right-hand zone of the line.
  CHECK(!has_sensor_boost_annunciator("48%           ls"));
}

TEST_CASE(has_sensor_boost_annunciator_false_for_a_literal_asterisk) {
  // THE ACTUAL BEHAVIOUR CHANGE stage 16 makes here: pre-stage-16, a literal
  // ASCII '*' (0x2A) at column 15 -- indistinguishable from glyphs::ALPHA
  // once sanitize() had collapsed the real byte to the same character --
  // would have fired this predicate. Now that it reads the raw lane and
  // compares against the exact measured byte, an actual asterisk on the
  // display (nothing has ever produced one, but nothing ruled it out
  // either under the old collapse) must NOT be confused with the
  // annunciator.
  CHECK(!has_sensor_boost_annunciator("31%            *"));
}

TEST_CASE(has_sensor_boost_annunciator_false_for_a_short_line) {
  // protocol::LINE_LEN is a fixed 16 on the wire, but the host tests (and a
  // not-yet-fully-arrived frame) can pass shorter strings -- must not read
  // past the end of a short line looking for column 15.
  CHECK(!has_sensor_boost_annunciator("31%"));
  CHECK(!has_sensor_boost_annunciator(""));
}

TEST_CASE(status_tracker_humidity_boost_nullopt_before_any_status_frame) {
  StatusTracker t;
  CHECK(!t.humidity_boost().has_value());
}

TEST_CASE(status_tracker_humidity_boost_set_by_the_annunciator_on_a_status_screen) {
  StatusTracker t;
  t.update("Summer Bypass On", "31%            \xE0", true, 0);
  CHECK(t.humidity_boost().has_value());
  CHECK(*t.humidity_boost());
}

TEST_CASE(status_tracker_humidity_boost_false_when_the_annunciator_is_absent) {
  StatusTracker t;
  t.update("Normal Airflow  ", "18%             ", true, 0);
  CHECK(t.humidity_boost().has_value());
  CHECK(!*t.humidity_boost());
}

TEST_CASE(status_tracker_humidity_boost_not_set_by_stage_10s_ls_on_status_screen) {
  StatusTracker t;
  t.update("Boost Airflow   ", "48%           ls", true, 0);
  CHECK(t.humidity_boost().has_value());
  CHECK(!*t.humidity_boost());
}

TEST_CASE(status_tracker_humidity_boost_ignores_the_annunciator_on_a_menu_or_diagnostic_screen) {
  StatusTracker t;
  // State established FIRST, deliberately. Asserting on a menu frame alone
  // would prove nothing: before any status frame has_state() is false, so
  // humidity_boost() returns nullopt whether or not the annunciator was
  // read -- the assertion would still pass if the touch_() call were moved
  // above update()'s is_status_screen early return, which is precisely the
  // regression this test exists to catch.
  t.update("Normal Airflow  ", "18%             ", true, 0);
  CHECK(t.has_state());
  CHECK(!*t.humidity_boost());

  // Now a menu/diagnostic frame carrying glyphs::ALPHA at column 15. Those
  // screens legitimately show their own custom glyphs, and nothing rules
  // out one of them coincidentally landing on this exact byte at this exact
  // column -- only the status loop's line2 column 15 means the annunciator,
  // so this must not set the flag.
  t.update("Diagnostic  05  ", "0000           \xE0", false, 1000);
  CHECK(!*t.humidity_boost());

  // And still false back on the status screen: the menu frame left nothing
  // behind for a later frame to inherit.
  t.update("Normal Airflow  ", "18%             ", true, 2000);
  CHECK(!*t.humidity_boost());
}

TEST_CASE(status_tracker_humidity_boost_stays_true_across_a_frame_where_the_annunciator_is_absent) {
  StatusTracker t;
  t.update("Summer Bypass On", "31%            \xE0", true, 0);
  CHECK(*t.humidity_boost());

  // The annunciator is genuinely GONE from this frame -- not merely
  // accompanied by the status loop's other line1 message. That is the
  // property the sticky Flag exists for and the only thing that separates
  // it from a direct per-frame read: stage 9 measured line2 as
  // non-alternating, but stage 10's `ls` proved this right-hand zone can
  // blink, and a blink must not flap the entity. A test that kept the
  // annunciator present in every frame would pass against a direct read
  // too, proving nothing.
  t.update("Low Airflow     ", "18%             ", true, 1700);
  CHECK(*t.humidity_boost());

  t.update("Summer Bypass On", "31%            \xE0", true, 3400);
  CHECK(*t.humidity_boost());
}

TEST_CASE(status_tracker_humidity_boost_ages_out_after_its_timeout) {
  StatusTracker t;
  t.update("Summer Bypass On", "31%            \xE0", true, 0);
  CHECK(*t.humidity_boost());

  // Comfortably under ALTERNATION_TIMEOUT_MS: still true, annunciator gone.
  t.update("Normal Airflow  ", "18%             ", true, 11000);
  CHECK(*t.humidity_boost());

  // Now over it, still never having seen the annunciator again.
  t.update("Normal Airflow  ", "18%             ", true, 12100);
  CHECK(!*t.humidity_boost());
}

TEST_CASE(status_tracker_airflow_percent_frozen_while_parked) {
  StatusTracker t;
  t.update("Normal Airflow  ", "18%             ", true, 0);
  CHECK_EQ(*t.airflow_percent(), 18);

  t.update("Diagnostic  05  ", "0000            ", false, 1000);
  CHECK_EQ(*t.airflow_percent(), 18);  // unchanged while off the status screen
}

// ------------------------------------------------------ continuous_boost --
// Reopened 13 Aug 2026 against live evidence from 192.168.1.200 (see the
// plan this shipped under). The central hazard: a timed boost's own expiry
// looks, for up to ALTERNATION_TIMEOUT_MS (plus a few seconds of observed
// real-world jitter), identical to the trailing edge of a genuine
// continuous episode -- countdown gone, boosting_ still sticky-true.
// CONTINUOUS_CONFIRM_MS (20s), required to exceed ALTERNATION_TIMEOUT_MS
// (12s) by a healthy margin, is what tells them apart.

TEST_CASE(continuous_boost_false_before_the_confirm_window_elapses) {
  StatusTracker t;
  t.update("Boost Airflow   ", "48%             ", true, 0);
  CHECK(t.continuous_boost().has_value());
  CHECK(!*t.continuous_boost());

  // "Boost Airflow" keeps reappearing (keeping boosting_ genuinely sticky,
  // the way real hardware alternates) but well under CONTINUOUS_CONFIRM_MS
  // of total elapsed time with no countdown ever parsed.
  t.update("Summer Bypass On", "48%             ", true, 5000);
  t.update("Boost Airflow   ", "48%             ", true, 9000);
  CHECK(!*t.continuous_boost());
}

TEST_CASE(continuous_boost_true_once_past_the_confirm_window) {
  StatusTracker t;
  t.update("Boost Airflow   ", "48%             ", true, 0);       // ms_without_countdown_ = 0
  t.update("Summer Bypass On", "48%             ", true, 6000);    // += 6000 = 6000
  t.update("Boost Airflow   ", "48%             ", true, 13000);   // += 7000 = 13000 (re-matches, stays sticky)
  CHECK(!*t.continuous_boost());

  t.update("Summer Bypass On", "48%             ", true, 19000);   // += 6000 = 19000, still under 20000
  CHECK(!*t.continuous_boost());

  // Past CONTINUOUS_CONFIRM_MS of total elapsed time with no countdown ever
  // parsed, and boosting_ has never gone inactive (last real "Boost Airflow"
  // sighting only 7900ms ago at this point, well inside ALTERNATION_TIMEOUT_MS)
  // -- now genuinely confirmed continuous.
  t.update("Summer Bypass On", "48%             ", true, 20900);   // += 1900 = 20900
  CHECK(*t.boosting());
  CHECK(t.continuous_boost().has_value());
  CHECK(*t.continuous_boost());
}

TEST_CASE(continuous_boost_never_true_across_a_timed_boosts_own_expiry) {
  // THE KEY REGRESSION. A 30-minute boost (compressed to 3m -> 2m -> 1m
  // ticks so the test doesn't need to simulate 30 real minutes) that
  // genuinely expires must never report continuous. Live evidence (13 Aug
  // 2026, 192.168.1.200) shows a real expiry moves line1 AND line2 together
  // in the SAME frame -- "Boost Airflow"/"NN% NNm" straight to (e.g.)
  // "Low Airflow"/"18%" -- so unlike a genuine continuous episode (where
  // "Boost Airflow" keeps reappearing every 6-8s indefinitely with line2
  // never showing a countdown at all), there is no drawn-out stretch of
  // "Boost Airflow" persisting with no countdown here. The only residual
  // hazard is boosting_'s own stickiness: it stays active for up to
  // ALTERNATION_TIMEOUT_MS (measured live at up to 14.0s, see
  // CONTINUOUS_CONFIRM_MS's own comment) after that last real "Boost
  // Airflow" sighting, during which this accumulator keeps advancing from
  // wherever the last countdown reset it to.
  StatusTracker t;
  t.update("Boost Airflow   ", "48%       3m    ", true, 0);
  CHECK(!*t.continuous_boost());
  t.update("Boost Airflow   ", "48%       2m    ", true, 60000);
  CHECK(!*t.continuous_boost());
  t.update("Boost Airflow   ", "48%       1m    ", true, 120000);
  CHECK(!*t.continuous_boost());  // still ticking -- each tick resets the accumulator

  // Genuine expiry, same-frame transition, a few seconds after the last
  // tick (the boost had under a minute left showing "1m"): line1 stops
  // saying "Boost Airflow" and line2 loses its countdown in the same
  // update(). boosting_ stays sticky-true (last real match only 4000ms ago,
  // comfortably under ALTERNATION_TIMEOUT_MS), so the accumulator starts
  // climbing from the 0 the last tick reset it to.
  t.update("Low Airflow     ", "18%             ", true, 124000);  // +4000 without a countdown
  CHECK(*t.boosting());  // still sticky-true -- the real cessation hasn't aged out yet
  CHECK(!*t.continuous_boost());

  // Right at the edge of ALTERNATION_TIMEOUT_MS since the last real "Boost
  // Airflow" match (120000): 11900ms elapsed, boosting_ still (barely)
  // active, accumulator at its peak for this episode -- nowhere near
  // CONTINUOUS_CONFIRM_MS's 20000ms threshold, which is exactly the margin
  // the constant exists to guarantee.
  t.update("Low Airflow     ", "18%             ", true, 131900);
  CHECK(*t.boosting());
  CHECK(!*t.continuous_boost());

  // 200ms later boosting_ finally ages out (12100ms since the last real
  // match) -- the accumulator resets to 0 on the very same frame, so
  // continuous_boost() never had a chance to cross the threshold at any
  // point in this whole episode.
  t.update("Low Airflow     ", "18%             ", true, 132100);
  CHECK(!*t.boosting());
  CHECK(!*t.continuous_boost());
}

TEST_CASE(continuous_boost_never_flashes_when_a_timed_boosts_countdown_lands_a_frame_late) {
  // Closes the second race the plan documents: resetting the accumulator on
  // "!boosting_.active" (not merely on seeing a countdown) means the clock
  // starts at the EPISODE, not at the first countdown sighting -- so a
  // countdown landing a frame or two after line1's "Boost Airflow" first
  // appears can never flash Boost Continuous first, however that ordering
  // happens to land.
  StatusTracker t;
  t.update("Normal Airflow  ", "18%             ", true, 0);  // not boosting -- accumulator pinned at 0
  CHECK(!*t.continuous_boost());

  // Boost starts. First frame or two show "Boost Airflow" with no countdown
  // parsed yet (simulating the countdown landing on line2 a beat later).
  t.update("Boost Airflow   ", "48%             ", true, 3400);
  CHECK(!*t.continuous_boost());
  t.update("Boost Airflow   ", "48%             ", true, 6800);
  CHECK(!*t.continuous_boost());

  // The countdown lands -- resets the accumulator again, same as any other
  // countdown sighting.
  t.update("Boost Airflow   ", "48%       30m   ", true, 10200);
  CHECK(!*t.continuous_boost());
}

TEST_CASE(continuous_boost_accumulator_freezes_while_parked_on_a_menu_screen) {
  // Mirrors status_tracker_does_not_age_a_flag_out_while_parked_on_a_menu_screen
  // above -- a diagnostics fetch or clock sync parking the display in a menu
  // must not spend that wall-clock time against the accumulator's budget,
  // or a routine 15-minute scrape could itself manufacture a false Boost
  // Continuous confirmation.
  StatusTracker t;
  t.update("Boost Airflow   ", "48%             ", true, 0);
  t.update("Boost Airflow   ", "48%             ", true, 5000);
  CHECK(!*t.continuous_boost());  // 5000ms without a countdown so far

  // Parked in a menu for well over CONTINUOUS_CONFIRM_MS. is_status_screen
  // is false throughout, so update() returns before the accumulator logic
  // ever runs.
  t.update("Diagnostic  05  ", "0000            ", false, 10000);
  t.update("Diagnostic  06  ", "0000            ", false, 15000);
  t.update("Set Clock       ", "Sun 23:49       ", false, 30000);
  CHECK(*t.boosting());           // still sticky -- the park didn't age this out either
  CHECK(!*t.continuous_boost());  // must not have accrued the ~25s spent parked

  // Back on the status screen immediately afterwards -- only ~100ms of
  // *visible* time has actually elapsed against the accumulator's budget
  // (5000ms from before the park, plus ~100ms now), nowhere near 20000ms.
  t.update("Summer Bypass On", "48%             ", true, 30100);
  CHECK(!*t.continuous_boost());
}

// -------------------------------------------------- AirflowModeTracker --
// The derivation that used to live only in VentAxiaHub::publish_airflow_mode_()
// (vent_axia.cpp), untestable on the host because that file is the one place
// (besides the platform *.py files) allowed to include esphome/... headers --
// see README "Portable core" and tests/CMakeLists.txt's exclusion of
// vent_axia.cpp from the glob. Moved into status::AirflowModeTracker so the
// state machine itself -- purge-wins, the boost-60 latch, the
// continuous-confirm window -- is covered here instead.

TEST_CASE(airflow_mode_tracker_unknown_before_any_status_screen_frame) {
  StatusTracker status;
  AirflowModeTracker mode;
  CHECK(!mode.update(status).has_value());

  status.update("Normal Airflow  ", "18%             ", true, 0);
  CHECK(mode.update(status).has_value());
}

TEST_CASE(airflow_mode_tracker_purge_wins_over_boost) {
  StatusTracker status;
  AirflowModeTracker mode;
  // Both purging_ and boosting_ can be simultaneously active in the
  // tracker's own sticky state (each rides line1's alternation and ages out
  // independently), even though the unit presumably only ever shows one at
  // a time -- purge must still win outright regardless.
  status.update("Boost Airflow   ", "48%       30m   ", true, 0);
  status.update("Purge      120 m", "100%            ", true, 3000);
  CHECK(*status.purging());
  CHECK(*status.boosting());
  const auto m = mode.update(status);
  CHECK(m.has_value());
  CHECK(*m == AirflowMode::PURGE);
}

TEST_CASE(airflow_mode_tracker_latches_boost_60_through_the_countdown_second_half) {
  StatusTracker status;
  AirflowModeTracker mode;
  status.update("Boost Airflow   ", "48%       45m   ", true, 0);
  CHECK(*mode.update(status) == AirflowMode::BOOST_60);

  // Countdown drops into the second half (<=30), on its own indistinguishable
  // from an actual 30-minute boost -- the latch set by the 45m frame above
  // is what keeps this reporting BOOST_60 instead of silently flipping to
  // BOOST_30 with nobody having touched anything. This is the latch's whole
  // purpose.
  status.update("Boost Airflow   ", "48%       20m   ", true, 5000);
  const auto m = mode.update(status);
  CHECK(m.has_value());
  CHECK(*m == AirflowMode::BOOST_60);
}

TEST_CASE(airflow_mode_tracker_reports_boost_30_when_never_seen_above_30) {
  StatusTracker status;
  AirflowModeTracker mode;
  status.update("Boost Airflow   ", "48%       25m   ", true, 0);
  const auto m = mode.update(status);
  CHECK(m.has_value());
  CHECK(*m == AirflowMode::BOOST_30);
}

TEST_CASE(airflow_mode_tracker_latch_clears_when_boosting_ends) {
  StatusTracker status;
  AirflowModeTracker mode;
  status.update("Boost Airflow   ", "48%       45m   ", true, 0);
  CHECK(*mode.update(status) == AirflowMode::BOOST_60);

  // Boosting ends -- ALTERNATION_TIMEOUT_MS (12000ms) of nothing but
  // "Normal Airflow" ages boosting() out to false, same threshold every
  // other line1 flag in this class uses.
  status.update("Normal Airflow  ", "18%             ", true, 12100);
  CHECK(!*status.boosting());
  CHECK(*mode.update(status) == AirflowMode::NORMAL);

  // A fresh, genuinely 30-minute boost starts. If the latch had not
  // cleared, this would wrongly inherit BOOST_60 from the episode that just
  // ended.
  status.update("Boost Airflow   ", "48%       25m   ", true, 15000);
  CHECK(*mode.update(status) == AirflowMode::BOOST_30);
}

TEST_CASE(airflow_mode_tracker_reads_normal_before_confirm_and_continuous_after) {
  // Same timing as continuous_boost_true_once_past_the_confirm_window above
  // -- "Boost Airflow" re-matches periodically to keep boosting_ sticky
  // (each gap well under ALTERNATION_TIMEOUT_MS) while no countdown is ever
  // parsed, so ms_without_countdown_ accumulates toward CONTINUOUS_CONFIRM_MS.
  StatusTracker status;
  AirflowModeTracker mode;
  status.update("Boost Airflow   ", "48%             ", true, 0);
  CHECK(*mode.update(status) == AirflowMode::NORMAL);  // never guess ahead of the confirm window

  status.update("Summer Bypass On", "48%             ", true, 6000);
  status.update("Boost Airflow   ", "48%             ", true, 13000);
  status.update("Summer Bypass On", "48%             ", true, 19000);
  CHECK(*mode.update(status) == AirflowMode::NORMAL);  // 19000ms without a countdown, still under 20000

  status.update("Summer Bypass On", "48%             ", true, 20900);
  CHECK(*status.boosting());
  const auto m = mode.update(status);
  CHECK(m.has_value());
  CHECK(*m == AirflowMode::BOOST_CONTINUOUS);
}

TEST_CASE(airflow_mode_tracker_exactly_30_remaining_does_not_latch) {
  // The boundary: the rule is strictly ABOVE 30, so a countdown that reads
  // exactly 30 must not latch BOOST_60.
  StatusTracker status;
  AirflowModeTracker mode;
  status.update("Boost Airflow   ", "48%       30m   ", true, 0);
  CHECK(*mode.update(status) == AirflowMode::BOOST_30);

  // If 30 had wrongly latched, this later frame (still in the ambiguous
  // 1-30 range) would misreport BOOST_60.
  status.update("Boost Airflow   ", "48%       15m   ", true, 3000);
  const auto m = mode.update(status);
  CHECK(m.has_value());
  CHECK(*m == AirflowMode::BOOST_30);
}
