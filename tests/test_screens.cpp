#include "test_framework.h"

#include "screens.h"

using namespace esphome::vent_axia::screens;

TEST_CASE(starts_with_ci_ignores_case) {
  CHECK(starts_with_ci("Diagnostic  27", "diagnostic"));
  CHECK(starts_with_ci("SUMMER MODE     ", "Summer Mode"));
  CHECK(starts_with_ci("summer mode     ", "SUMMER MODE"));
  CHECK(!starts_with_ci("Sum", "Summer"));   // shorter than prefix
  CHECK(!starts_with_ci("Boost Airflow   ", "Summer"));
}

TEST_CASE(menu_screen_whitelist) {
  CHECK(is_menu_screen("Set Clock       "));
  CHECK(is_menu_screen("summer mode     "));  // manual's casing, still matches
  CHECK(is_menu_screen("Indoor Temp     "));
  CHECK(is_menu_screen("Outdoor Temp    "));
  CHECK(is_menu_screen("Diagnostic  01  "));
  // Status-loop screens are deliberately NOT in the whitelist -- see the
  // comment on is_menu_screen for why "unrecognised" must mean status.
  CHECK(!is_menu_screen("18%             "));
  CHECK(!is_menu_screen("Boost Airflow   "));
  CHECK(!is_menu_screen("                "));
}

TEST_CASE(diagnostic_page_parses_two_digits) {
  const auto page = diagnostic_page("Diagnostic  27");
  CHECK(page.has_value());
  CHECK_EQ(*page, 27);
}

TEST_CASE(diagnostic_page_rejects_missing_digits_without_crashing) {
  CHECK(!diagnostic_page("Diagnostic  ").has_value());
}

TEST_CASE(diagnostic_page_rejects_non_numeric_without_crashing) {
  CHECK(!diagnostic_page("Diagnostic  2X").has_value());
}

TEST_CASE(diagnostic_page_rejects_non_diagnostic_screens) {
  CHECK(!diagnostic_page("Indoor Temp  20").has_value());
  CHECK(!diagnostic_page("").has_value());
}

TEST_CASE(trim_trailing_preserves_interior_spaces) {
  CHECK_EQ(trim_trailing("Boost  Active   "), std::string("Boost  Active"));
  CHECK_EQ(trim_trailing("NoTrailingSpace"), std::string("NoTrailingSpace"));
  CHECK_EQ(trim_trailing("   "), std::string(""));
  CHECK_EQ(trim_trailing(""), std::string(""));
}

TEST_CASE(is_menu_screen_agrees_with_classify) {
  // The two must never diverge: is_menu_screen is defined in terms of
  // classify precisely so that a later stage adding a screen cannot teach one
  // about it and not the other.
  for (const char *line : {"Set Clock       ", "Summer Mode     ", "Indoor Temp     ",
                           "Outdoor Temp    ", "Diagnostic  05  ", "18%             ",
                           "Boost Airflow   ", "Check Filter    ", "                "}) {
    CHECK_EQ(is_menu_screen(line), classify(line) != ScreenKind::STATUS);
  }
}

TEST_CASE(classify_maps_each_screen_kind) {
  CHECK(classify("18%             ") == ScreenKind::STATUS);
  CHECK(classify("Boost Airflow   ") == ScreenKind::STATUS);
  CHECK(classify("Set Clock       ") == ScreenKind::SET_CLOCK);
  CHECK(classify("Summer Mode     ") == ScreenKind::SUMMER_MODE);
  CHECK(classify("Indoor Temp     ") == ScreenKind::INDOOR_TEMP);
  CHECK(classify("Outdoor Temp    ") == ScreenKind::OUTDOOR_TEMP);
  CHECK(classify("Diagnostic  05  ") == ScreenKind::DIAGNOSTIC);
}
