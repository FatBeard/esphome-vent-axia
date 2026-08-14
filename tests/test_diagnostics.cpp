#include "test_framework.h"

#include <optional>
#include <vector>

#include "diagnostics.h"

using namespace esphome::vent_axia;
using namespace esphome::vent_axia::diagnostics;

namespace {

// Records exactly what a page decode published, so a test can assert on
// precisely which keys fired and with what value -- including the negative
// "this key must NOT have been touched at all" case that matters for the
// blank-is-not-zero rule.
struct Recorder {
  std::vector<std::pair<SensorKey, int>> sensors;
  std::vector<std::pair<BinaryKey, bool>> binaries;
  std::vector<std::pair<TextKey, std::string>> texts;
  std::optional<bool> filter_change_due_report;

  Sink sink() {
    Sink s;
    s.publish_sensor = [this](SensorKey k, int v) { this->sensors.emplace_back(k, v); };
    s.publish_binary = [this](BinaryKey k, bool v) { this->binaries.emplace_back(k, v); };
    s.publish_text = [this](TextKey k, const std::string &v) { this->texts.emplace_back(k, v); };
    s.report_filter_change_due = [this](bool due) { this->filter_change_due_report = due; };
    return s;
  }

  bool has_sensor(SensorKey k) const {
    for (const auto &e : this->sensors) {
      if (e.first == k) {
        return true;
      }
    }
    return false;
  }

  int sensor_value(SensorKey k) const {
    for (const auto &e : this->sensors) {
      if (e.first == k) {
        return e.second;
      }
    }
    return -12345;  // sentinel: caller should have checked has_sensor() first
  }

  bool has_binary(BinaryKey k) const {
    for (const auto &e : this->binaries) {
      if (e.first == k) {
        return true;
      }
    }
    return false;
  }

  bool binary_value(BinaryKey k) const {
    for (const auto &e : this->binaries) {
      if (e.first == k) {
        return e.second;
      }
    }
    return false;
  }

  bool has_text(TextKey k) const {
    for (const auto &e : this->texts) {
      if (e.first == k) {
        return true;
      }
    }
    return false;
  }

  std::string text_value(TextKey k) const {
    for (const auto &e : this->texts) {
      if (e.first == k) {
        return e.second;
      }
    }
    return "<missing>";
  }

  size_t total_calls() const { return this->sensors.size() + this->binaries.size() + this->texts.size(); }
};

}  // namespace

// ------------------------------------------------------ pages 0/1: motors --

TEST_CASE(page0_decodes_supply_motor_from_a_captured_line) {
  Recorder r;
  decode_page(0, "018 029 % 0994  ", r.sink());

  CHECK_EQ(r.sensors.size(), static_cast<size_t>(3));
  CHECK(r.has_sensor(SensorKey::SUPPLY_AIRFLOW_SET));
  CHECK_EQ(r.sensor_value(SensorKey::SUPPLY_AIRFLOW_SET), 18);
  CHECK(r.has_sensor(SensorKey::SUPPLY_MOTOR_PWM));
  CHECK_EQ(r.sensor_value(SensorKey::SUPPLY_MOTOR_PWM), 29);
  CHECK(r.has_sensor(SensorKey::SUPPLY_FAN_RPM));
  CHECK_EQ(r.sensor_value(SensorKey::SUPPLY_FAN_RPM), 994);
  CHECK(r.binaries.empty());
  CHECK(r.texts.empty());
}

TEST_CASE(page1_decodes_extract_motor_from_a_captured_line) {
  Recorder r;
  decode_page(1, "018 029 % 0988  ", r.sink());

  CHECK_EQ(r.sensor_value(SensorKey::EXTRACT_AIRFLOW_SET), 18);
  CHECK_EQ(r.sensor_value(SensorKey::EXTRACT_MOTOR_PWM), 29);
  CHECK_EQ(r.sensor_value(SensorKey::EXTRACT_FAN_RPM), 988);
}

TEST_CASE(page0_and_page1_report_the_same_commanded_percentage_not_a_flow) {
  // The correction this table exists to record: field 1 is identical on both
  // motors and matches the status screen's 18% exactly -- a commanded
  // setpoint, not two independently measured flows that just happen to agree.
  Recorder r0;
  decode_page(0, "018 029 % 0994  ", r0.sink());
  Recorder r1;
  decode_page(1, "018 029 % 0988  ", r1.sink());
  CHECK_EQ(r0.sensor_value(SensorKey::SUPPLY_AIRFLOW_SET), r1.sensor_value(SensorKey::EXTRACT_AIRFLOW_SET));
}

TEST_CASE(page0_blank_middle_field_leaves_that_key_unpublished_not_zero) {
  // pos(4,3) blanked out -- the other two fields still parse fine.
  Recorder r;
  decode_page(0, "018     % 0994  ", r.sink());

  CHECK(r.has_sensor(SensorKey::SUPPLY_AIRFLOW_SET));
  CHECK(r.has_sensor(SensorKey::SUPPLY_FAN_RPM));
  CHECK(!r.has_sensor(SensorKey::SUPPLY_MOTOR_PWM));  // not published at all, not published as 0
  CHECK_EQ(r.sensors.size(), static_cast<size_t>(2));
}

// --------------------------------------------------- pages 2/3: T1/T2 --

TEST_CASE(page2_decodes_supply_temperature_and_no_fault) {
  Recorder r;
  decode_page(2, " 16 C 00        ", r.sink());

  CHECK_EQ(r.sensor_value(SensorKey::SUPPLY_AIR_TEMP), 16);
  CHECK(r.has_binary(BinaryKey::SUPPLY_TEMP_FAULT));
  CHECK(!r.binary_value(BinaryKey::SUPPLY_TEMP_FAULT));
}

TEST_CASE(page2_nonzero_fault_code_reports_a_fault) {
  Recorder r;
  decode_page(2, " 16 C 01        ", r.sink());
  CHECK(r.binary_value(BinaryKey::SUPPLY_TEMP_FAULT));  // 1 == short-circuit
}

TEST_CASE(page3_decodes_extract_temperature_and_no_fault) {
  Recorder r;
  decode_page(3, " 23 C 00        ", r.sink());

  CHECK_EQ(r.sensor_value(SensorKey::EXTRACT_AIR_TEMP), 23);
  CHECK(!r.binary_value(BinaryKey::EXTRACT_TEMP_FAULT));
}

// ------------------------------------------------- page 4: internal sensor --

TEST_CASE(page4_decodes_rh_temp_and_average_from_a_captured_line) {
  Recorder r;
  decode_page(4, "59 % 23 C 59 037", r.sink());

  CHECK_EQ(r.sensor_value(SensorKey::INDOOR_HUMIDITY), 59);
  CHECK_EQ(r.sensor_value(SensorKey::INDOOR_TEMP), 23);
  CHECK_EQ(r.sensor_value(SensorKey::INDOOR_HUMIDITY_AVG), 59);
  CHECK_EQ(r.sensors.size(), static_cast<size_t>(3));
}

TEST_CASE(page4_rh_and_temp_both_zero_publishes_nothing_no_sensor_fitted) {
  Recorder r;
  decode_page(4, "00 % 00 C 00 000", r.sink());

  CHECK_EQ(r.total_calls(), static_cast<size_t>(0));
}

TEST_CASE(page4_average_alone_being_zero_does_not_trigger_the_sentinel) {
  // Only RH==0 AND temp==0 TOGETHER means "no sensor" -- a genuinely zero
  // average with real RH/temp readings must still publish normally.
  Recorder r;
  decode_page(4, "12 % 05 C 00 000", r.sink());

  CHECK_EQ(r.sensor_value(SensorKey::INDOOR_HUMIDITY), 12);
  CHECK_EQ(r.sensor_value(SensorKey::INDOOR_TEMP), 5);
  CHECK_EQ(r.sensor_value(SensorKey::INDOOR_HUMIDITY_AVG), 0);
}

// ---------------------------------------- page 5: switched-live boost input --
// Real captures from 192.168.1.200 (firmware V32/05, 13 Aug 2026) -- see
// diagnostics.cpp's PAGE_5_FIELDS comment for the full evidence, including
// why only column 0 is decoded (cols 5-6 and 13-15 correlate but can't yet
// be told apart as "configured" vs. "remaining", and cols 8-9/10-11 are
// unexplained).

TEST_CASE(page5_switched_live_asserted_sets_the_flag) {
  Recorder r;
  decode_page(5, "1 00 05 0 00 030", r.sink());
  CHECK(r.has_binary(BinaryKey::SWITCHED_LIVE_BOOST));
  CHECK(r.binary_value(BinaryKey::SWITCHED_LIVE_BOOST));
  CHECK_EQ(r.binaries.size(), static_cast<size_t>(1));  // only column 0 decoded

  // Same episode, cols 10-11 ticking -- must not affect the decode.
  Recorder r2;
  decode_page(5, "1 00 05 0 01 030", r2.sink());
  CHECK(r2.binary_value(BinaryKey::SWITCHED_LIVE_BOOST));
}

TEST_CASE(page5_commanded_boost_leaves_the_flag_clear) {
  // The decisive capture: boosting on a HA-commanded Boost 30 min, switched
  // live released (light off) -- column 0 must read false even though the
  // unit genuinely is boosting (BinaryKey::BOOSTING, from the status-line
  // decode, is the flag for that; this one is specifically the switch
  // input).
  Recorder r;
  decode_page(5, "0 00 00 0 00 000", r.sink());
  CHECK(r.has_binary(BinaryKey::SWITCHED_LIVE_BOOST));
  CHECK(!r.binary_value(BinaryKey::SWITCHED_LIVE_BOOST));

  Recorder r2;
  decode_page(5, "0 00 00 0 05 000", r2.sink());
  CHECK(!r2.binary_value(BinaryKey::SWITCHED_LIVE_BOOST));
}

TEST_CASE(page5_idle_leaves_the_flag_clear) {
  Recorder r;
  decode_page(5, "0 00 00 0 00 000", r.sink());
  CHECK(!r.binary_value(BinaryKey::SWITCHED_LIVE_BOOST));

  Recorder r2;
  decode_page(5, "0 00 00 0 02 000", r2.sink());
  CHECK(!r2.binary_value(BinaryKey::SWITCHED_LIVE_BOOST));

  Recorder r3;
  decode_page(5, "0 00 00 0 04 000", r3.sink());
  CHECK(!r3.binary_value(BinaryKey::SWITCHED_LIVE_BOOST));
}

// --------------------------------------------- pages 6/7/8: wall switches --

TEST_CASE(page6_7_8_decode_switch_closed_from_a_captured_line) {
  // Captured with the contact open (nonzero_field: 0 == open).
  Recorder r6;
  decode_page(6, "0000 1 0 000 00 ", r6.sink());
  CHECK(!r6.binary_value(BinaryKey::SWITCH_LINE_1));

  Recorder r7;
  decode_page(7, "0000 1 1 000 00 ", r7.sink());
  CHECK(r7.binary_value(BinaryKey::SWITCH_LINE_2));  // nonzero == closed

  Recorder r8;
  decode_page(8, "0000 1 0 000 00 ", r8.sink());
  CHECK(!r8.binary_value(BinaryKey::SWITCH_LINE_3));
}

// ------------------------------------------------- page 11: wireless --

TEST_CASE(page11_decodes_fitted_flag_from_a_captured_line) {
  Recorder r;
  decode_page(11, "16 0 16 0000 000", r.sink());
  CHECK(r.has_binary(BinaryKey::WIRELESS_FITTED));
  CHECK(!r.binary_value(BinaryKey::WIRELESS_FITTED));  // this unit has none fitted
}

TEST_CASE(page11_nonzero_fitted_flag_reports_fitted) {
  Recorder r;
  decode_page(11, "16 1 16 0000 000", r.sink());
  CHECK(r.binary_value(BinaryKey::WIRELESS_FITTED));
}

// ---------------------------------------- page 19: 24V rail (INVERTED) --

TEST_CASE(page19_field_1_means_rail_ok_so_fault_is_false) {
  Recorder r;
  decode_page(19, "1               ", r.sink());
  CHECK(r.has_binary(BinaryKey::RAIL_24V_FAULT));
  CHECK(!r.binary_value(BinaryKey::RAIL_24V_FAULT));
}

TEST_CASE(page19_field_0_means_rail_fault_so_fault_is_true) {
  // The inversion this test exists to pin down: 0 on the wire is the FAULT
  // state, not "zero volts reported as a measurement".
  Recorder r;
  decode_page(19, "0               ", r.sink());
  CHECK(r.binary_value(BinaryKey::RAIL_24V_FAULT));
}

// ---------------------------------------------- page 20: west/link --

TEST_CASE(page20_state_1_is_link) {
  Recorder r;
  decode_page(20, "0410 1          ", r.sink());
  CHECK_EQ(r.text_value(TextKey::WEST_LINK_STATE), std::string("Link"));
}

TEST_CASE(page20_state_0_is_no_link) {
  Recorder r;
  decode_page(20, "0410 0          ", r.sink());
  CHECK_EQ(r.text_value(TextKey::WEST_LINK_STATE), std::string("No Link"));
}

TEST_CASE(page20_state_2_is_west) {
  Recorder r;
  decode_page(20, "0410 2          ", r.sink());
  CHECK_EQ(r.text_value(TextKey::WEST_LINK_STATE), std::string("West"));
}

TEST_CASE(page20_unknown_state_falls_back_to_generic_state_n_text) {
  Recorder r;
  decode_page(20, "0410 7          ", r.sink());
  CHECK_EQ(r.text_value(TextKey::WEST_LINK_STATE), std::string("State 7"));
}

TEST_CASE(page20_blank_field_publishes_nothing) {
  Recorder r;
  decode_page(20, "0410            ", r.sink());
  CHECK(!r.has_text(TextKey::WEST_LINK_STATE));
}

// -------------------------------------------------- page 23: filter --

TEST_CASE(page23_zero_hours_publishes_hours_and_reports_change_due) {
  Recorder r;
  decode_page(23, "00000           ", r.sink());

  CHECK(r.has_sensor(SensorKey::FILTER_HOURS));
  CHECK_EQ(r.sensor_value(SensorKey::FILTER_HOURS), 0);
  CHECK(r.filter_change_due_report.has_value());
  CHECK(*r.filter_change_due_report);
}

TEST_CASE(page23_nonzero_hours_reports_change_not_due) {
  Recorder r;
  decode_page(23, "00120           ", r.sink());

  CHECK_EQ(r.sensor_value(SensorKey::FILTER_HOURS), 120);
  CHECK(r.filter_change_due_report.has_value());
  CHECK(!*r.filter_change_due_report);
}

TEST_CASE(page23_blank_field_publishes_nothing_and_reports_nothing) {
  Recorder r;
  decode_page(23, "                ", r.sink());
  CHECK(!r.has_sensor(SensorKey::FILTER_HOURS));
  CHECK(!r.filter_change_due_report.has_value());
}

// ------------------------------------------------- page 24: antifrost --

TEST_CASE(page24_mode_10_is_bypass_and_active) {
  Recorder r;
  decode_page(24, "10 0 000  00    ", r.sink());

  CHECK(r.binary_value(BinaryKey::ANTIFROST_ACTIVE));
  CHECK_EQ(r.text_value(TextKey::ANTIFROST_MODE), std::string("Bypass"));
}

TEST_CASE(page24_mode_0_is_off_and_inactive) {
  Recorder r;
  decode_page(24, "00 0 000  00    ", r.sink());

  CHECK(!r.binary_value(BinaryKey::ANTIFROST_ACTIVE));
  CHECK_EQ(r.text_value(TextKey::ANTIFROST_MODE), std::string("Off"));
}

TEST_CASE(page24_documented_modes_1_through_4_map_to_their_airflow_text) {
  Recorder r1;
  decode_page(24, "01 0 000  00    ", r1.sink());
  CHECK_EQ(r1.text_value(TextKey::ANTIFROST_MODE), std::string("Airflow 0% / 115%"));

  Recorder r2;
  decode_page(24, "02 0 000  00    ", r2.sink());
  CHECK_EQ(r2.text_value(TextKey::ANTIFROST_MODE), std::string("Airflow 85% / 115%"));

  Recorder r3;
  decode_page(24, "03 0 000  00    ", r3.sink());
  CHECK_EQ(r3.text_value(TextKey::ANTIFROST_MODE), std::string("Airflow 55% / 115%"));

  Recorder r4;
  decode_page(24, "04 0 000  00    ", r4.sink());
  CHECK_EQ(r4.text_value(TextKey::ANTIFROST_MODE), std::string("Airflow 0% / 100%"));
}

TEST_CASE(page24_unknown_mode_falls_back_to_generic_mode_n_text) {
  Recorder r;
  decode_page(24, "07 0 000  00    ", r.sink());

  CHECK(r.binary_value(BinaryKey::ANTIFROST_ACTIVE));  // still nonzero == active
  CHECK_EQ(r.text_value(TextKey::ANTIFROST_MODE), std::string("Mode 7"));
}

// ---------------------------------------- pages 25/26: serial/firmware --

TEST_CASE(page25_serial_number_is_trimmed) {
  Recorder r;
  decode_page(25, "0000000000000000", r.sink());
  CHECK_EQ(r.text_value(TextKey::SERIAL_NUMBER), std::string("0000000000000000"));
}

TEST_CASE(page25_serial_number_trims_trailing_padding) {
  Recorder r;
  decode_page(25, "ABC123          ", r.sink());
  CHECK_EQ(r.text_value(TextKey::SERIAL_NUMBER), std::string("ABC123"));
}

TEST_CASE(page26_firmware_version_trims_correctly) {
  Recorder r;
  decode_page(26, "V32/05          ", r.sink());
  CHECK_EQ(r.text_value(TextKey::FIRMWARE_VERSION), std::string("V32/05"));
  CHECK_EQ(r.texts.size(), static_cast<size_t>(1));
}

// -------------------------------------- pages this table deliberately skips --

TEST_CASE(pages_not_in_the_table_publish_nothing) {
  // 9, 10, 12-18, 21-22 -- undecoded internal/absent state. A sample across
  // that list, all expected to be complete no-ops. Pages 5 and 20 used to be
  // in this list too, before their tri-state/column-0 fields were decoded
  // (see page5_* and page20_* tests above) -- deliberately no longer here.
  for (int page : {9, 10, 12, 13, 17, 18, 21, 22}) {
    Recorder r;
    decode_page(page, "0000000000000000", r.sink());
    CHECK_EQ(r.total_calls(), static_cast<size_t>(0));
  }
}

TEST_CASE(page27_reset_is_never_touched_even_read_only) {
  // Page 27 is "Reset" -- untried, and destructive if Set is ever pressed on
  // it (not that this stage presses anything). decode_page() must not read
  // it into any entity either, so a future table edit can't accidentally
  // wire it up to something that then looks safe to build a Set press on top
  // of.
  Recorder r;
  decode_page(27, "Reset           ", r.sink());
  CHECK_EQ(r.total_calls(), static_cast<size_t>(0));
}

TEST_CASE(an_unknown_page_publishes_nothing_no_hang_no_crash) {
  // Page 28 does not exist on firmware V32/05 (PLAN.md, README) -- the old
  // component hardcoded it as a scrape terminator and hung for 60s. This
  // table has no entry for it, or for any other number decode_page() has
  // never heard of, and must handle that as cleanly as page 27 above.
  Recorder r;
  decode_page(28, "                ", r.sink());
  CHECK_EQ(r.total_calls(), static_cast<size_t>(0));

  Recorder r2;
  decode_page(99, "whatever        ", r2.sink());
  CHECK_EQ(r2.total_calls(), static_cast<size_t>(0));
}

TEST_CASE(a_sink_with_no_callbacks_set_does_not_crash) {
  // decode_page() must tolerate a default-constructed Sink (all
  // std::functions empty) -- every dispatch site guards on the member being
  // set before calling it.
  Sink empty_sink;
  decode_page(0, "018 029 % 0994  ", empty_sink);
  decode_page(4, "59 % 23 C 59 037", empty_sink);
  decode_page(23, "00000           ", empty_sink);
  decode_page(24, "10 0 000  00    ", empty_sink);
  decode_page(26, "V32/05          ", empty_sink);
  CHECK(true);  // reaching here without crashing is the assertion
}

// --------------------------------------------------------- format_raw_page --
// The raw/trigger path itself is hub-owned (vent_axia.cpp calls this
// unconditionally for every page, decoded or not -- see diagnostics.h), but
// the formatting is portable core and testable here. Works for any page
// number, including ones decode_page() has no table entry for, which is the
// whole point: an unknown page is still visible to a human without a
// component change.

TEST_CASE(format_raw_page_pads_single_digit_pages_and_keeps_line2_verbatim) {
  CHECK_EQ(format_raw_page(7, "0000 1 0 000 00 "), std::string("07: 0000 1 0 000 00 "));
}

TEST_CASE(format_raw_page_does_not_pad_two_digit_pages) {
  CHECK_EQ(format_raw_page(27, "Reset           "), std::string("27: Reset           "));
}

TEST_CASE(format_raw_page_works_for_a_page_the_table_does_not_know_about) {
  // Page 28 does not exist on this firmware, and the table has no entry for
  // it -- but formatting it for the raw text sensor / trigger has no
  // dependency on the table at all.
  const std::string blank_line2(16, ' ');
  CHECK_EQ(format_raw_page(28, blank_line2), std::string("28: ") + blank_line2);
}
