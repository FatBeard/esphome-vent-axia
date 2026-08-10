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
  // Continuous boost (or plain normal airflow): a percentage with no
  // countdown anywhere. boost_time_remaining must stay unpublished -- this
  // is the documented, deliberate behaviour, not a bug.
  t.update("Boost Airflow   ", "48%             ", true, 0);
  CHECK(*t.boosting());
  CHECK(!t.boost_time_remaining().has_value());
}

TEST_CASE(status_tracker_airflow_percent_frozen_while_parked) {
  StatusTracker t;
  t.update("Normal Airflow  ", "18%             ", true, 0);
  CHECK_EQ(*t.airflow_percent(), 18);

  t.update("Diagnostic  05  ", "0000            ", false, 1000);
  CHECK_EQ(*t.airflow_percent(), 18);  // unchanged while off the status screen
}
