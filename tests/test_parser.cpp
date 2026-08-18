#include "test_framework.h"

#include "parser.h"

using namespace esphome::vent_axia::parser;

// --------------------------------------------------------- parse_field --

TEST_CASE(parse_field_rejects_blank_field_and_leaves_out_untouched) {
  int out = 42;
  CHECK(!parse_field("   C", 0, 3, out));
  CHECK_EQ(out, 42);  // blank is not zero -- out must be untouched, not set to 0
}

TEST_CASE(parse_field_parses_a_captured_page0_line) {
  // "018 029 % 0994  " -- supply airflow %, motor PWM %, RPM. Straight from
  // the unit's own page 0 capture.
  const std::string line = "018 029 % 0994  ";
  int airflow = 0;
  int pwm = 0;
  int rpm = 0;
  CHECK(parse_field(line, 0, 3, airflow));
  CHECK_EQ(airflow, 18);
  CHECK(parse_field(line, 4, 3, pwm));
  CHECK_EQ(pwm, 29);
  CHECK(parse_field(line, 10, 4, rpm));
  CHECK_EQ(rpm, 994);
}

TEST_CASE(parse_field_accepts_leading_space_and_sign_for_temperature_fields) {
  int out = 0;
  CHECK(parse_field(" -5", 0, 3, out));
  CHECK_EQ(out, -5);
  CHECK(parse_field("+05", 0, 3, out));
  CHECK_EQ(out, 5);
  CHECK(parse_field(" 20", 0, 3, out));
  CHECK_EQ(out, 20);
}

TEST_CASE(parse_field_rejects_a_string_shorter_than_pos_plus_len) {
  int out = 7;
  CHECK(!parse_field("12", 0, 3, out));
  CHECK_EQ(out, 7);
  CHECK(!parse_field("0994", 14, 4, out));  // too short at this offset
  CHECK_EQ(out, 7);
}

TEST_CASE(parse_field_rejects_non_numeric_junk) {
  int out = 7;
  CHECK(!parse_field("1X3", 0, 3, out));
  CHECK_EQ(out, 7);
  CHECK(!parse_field("Set", 0, 3, out));
  CHECK_EQ(out, 7);
}

TEST_CASE(parse_field_rejects_a_lone_sign_with_no_digit) {
  int out = 7;
  CHECK(!parse_field("  -", 0, 3, out));
  CHECK_EQ(out, 7);
  CHECK(!parse_field("  +", 0, 3, out));
  CHECK_EQ(out, 7);
}

// -------------------------------------------------------------- trim --

TEST_CASE(trim_strips_trailing_spaces_only) {
  CHECK_EQ(trim("Boost  Active   "), std::string("Boost  Active"));
  CHECK_EQ(trim("NoTrailingSpace"), std::string("NoTrailingSpace"));
  CHECK_EQ(trim("   "), std::string(""));
  CHECK_EQ(trim(""), std::string(""));
}

// --------------------------------------------------------- parse_on_off --

TEST_CASE(parse_on_off_accepts_all_documented_case_variants) {
  bool out = false;
  CHECK(parse_on_off("On              ", out));
  CHECK(out);
  CHECK(parse_on_off("on              ", out));
  CHECK(out);
  CHECK(parse_on_off("OFF             ", out));
  CHECK(!out);
  CHECK(parse_on_off("off             ", out));
  CHECK(!out);
  CHECK(parse_on_off("Off             ", out));
  CHECK(!out);
}

TEST_CASE(parse_on_off_rejects_the_blank_frame_a_blinking_editor_produces) {
  bool out = true;
  CHECK(!parse_on_off("                ", out));
  CHECK(out);  // untouched
}

TEST_CASE(parse_on_off_rejects_unrelated_text) {
  bool out = true;
  CHECK(!parse_on_off("Maybe           ", out));
  CHECK(out);  // untouched
}

// ------------------------------------------------------- clock_rendered --

TEST_CASE(clock_rendered_accepts_a_fully_rendered_line) {
  CHECK(clock_rendered("Sun 23:49"));
  CHECK(clock_rendered("Mon 00:00       "));  // padded to 16, still fine
}

TEST_CASE(clock_rendered_rejects_mid_blink_frames) {
  CHECK(!clock_rendered("    23:49"));  // day field blanked
  CHECK(!clock_rendered("Sun   :49"));  // hour field blanked
  CHECK(!clock_rendered("Sun 23:  "));  // minute field blanked
  CHECK(!clock_rendered("Sun 23"));     // too short to contain minute
  CHECK(!clock_rendered(""));
}

// --------------------------------------------- clock_day / hour / minute --

TEST_CASE(clock_day_matches_the_seven_display_names) {
  CHECK_EQ(clock_day("Mon 00:00"), 0);
  CHECK_EQ(clock_day("Tue 00:00"), 1);
  CHECK_EQ(clock_day("Wed 00:00"), 2);
  CHECK_EQ(clock_day("Thu 00:00"), 3);
  CHECK_EQ(clock_day("Fri 00:00"), 4);
  CHECK_EQ(clock_day("Sat 00:00"), 5);
  CHECK_EQ(clock_day("Sun 23:49"), 6);
  CHECK_EQ(clock_day("Xyz 00:00"), -1);
}

TEST_CASE(clock_hour_and_minute_decode_a_rendered_line) {
  CHECK_EQ(clock_hour("Sun 23:49"), 23);
  CHECK_EQ(clock_minute("Sun 23:49"), 49);
  CHECK_EQ(clock_hour("Mon 00:05"), 0);
  CHECK_EQ(clock_minute("Mon 00:05"), 5);
}

// ------------------------------------------------------ dow_to_display --

TEST_CASE(dow_to_display_maps_esphome_sunday_first_to_display_monday_first) {
  CHECK_EQ(dow_to_display(1), 6);  // ESPHome Sunday -> display index 6
  CHECK_EQ(dow_to_display(2), 0);  // ESPHome Monday -> display index 0
  CHECK_EQ(dow_to_display(3), 1);
  CHECK_EQ(dow_to_display(7), 5);  // ESPHome Saturday -> display index 5
}

// -------------------------------------------------------- wrapped_delta --

TEST_CASE(wrapped_delta_takes_the_shortest_path_for_hour) {
  CHECK_EQ(wrapped_delta(23, 1, 24), 2);    // forward over midnight is shorter
  CHECK_EQ(wrapped_delta(1, 23, 24), -2);   // backward is shorter
  CHECK_EQ(wrapped_delta(10, 10, 24), 0);   // already there
}

TEST_CASE(wrapped_delta_takes_the_shortest_path_for_minute) {
  CHECK_EQ(wrapped_delta(58, 2, 60), 4);
  CHECK_EQ(wrapped_delta(2, 58, 60), -4);
}

TEST_CASE(wrapped_delta_rounds_the_exact_half_tie_toward_up) {
  // 30 is exactly half of mod 60 either direction; vask_decode.h's `<=`
  // resolves the tie toward "up" (positive), not "down".
  CHECK_EQ(wrapped_delta(0, 30, 60), 30);
  CHECK_EQ(wrapped_delta(30, 0, 60), 30);
  // Same tie at mod 24 (noon apart).
  CHECK_EQ(wrapped_delta(0, 12, 24), 12);
}
