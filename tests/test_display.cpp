#include "test_framework.h"

#include "display.h"

using namespace esphome::vent_axia;

TEST_CASE(publish_on_change_fires_once_for_repeated_identical_text) {
  Display display;
  int changes = 0;
  display.set_on_change([&](bool, bool) { changes++; });

  display.update("Status          ", "18%             ", 1000);
  display.update("Status          ", "18%             ", 1300);  // same text again

  CHECK_EQ(changes, 1);
}

TEST_CASE(lines_are_deduplicated_independently) {
  Display display;
  int changes = 0;
  bool seen_l1 = false;
  bool seen_l2 = false;
  display.set_on_change([&](bool l1, bool l2) {
    changes++;
    seen_l1 = l1;
    seen_l2 = l2;
  });

  display.update("Status          ", "18%             ", 1000);
  CHECK_EQ(changes, 1);
  CHECK(seen_l1);
  CHECK(seen_l2);

  // Only line2 changes -- line1 must not be reported as changed.
  display.update("Status          ", "19%             ", 1300);
  CHECK_EQ(changes, 2);
  CHECK(!seen_l1);
  CHECK(seen_l2);

  // Only line1 changes -- line2 must not be reported as changed, even
  // though the callback fires (this is the "whole-frame memcmp" bug this
  // class replaces: an unrelated byte changing must not look like a line2
  // republish).
  display.update("Set Clock       ", "19%             ", 1600);
  CHECK_EQ(changes, 3);
  CHECK(seen_l1);
  CHECK(!seen_l2);

  // Neither line changes -- no callback at all.
  display.update("Set Clock       ", "19%             ", 1900);
  CHECK_EQ(changes, 3);
}

TEST_CASE(non_printable_glyphs_are_replaced_with_asterisk) {
  Display display;
  std::string line1 = "Auto";
  // Assumed to be the unit's "Auto" glyph (CGRAM slot 7), never measured --
  // see the describe_unprintable() control-range case below. What this test
  // pins is the collapse itself, which holds for any non-printable byte.
  line1 += static_cast<char>(0x07);
  line1 += "           ";            // pad to 16
  CHECK_EQ(line1.size(), static_cast<size_t>(16));

  display.update(line1, "                ", 0);

  CHECK_EQ(display.line1(), std::string("Auto*           "));
}

TEST_CASE(editor_open_is_false_before_the_first_frame) {
  // The changed-at timestamp starts at 0, so a bare staleness test would
  // report an open editor for the whole first settle window after boot --
  // while the link may not even be up. Nothing known must read as "no".
  Display display;

  CHECK(!display.have_frame());
  CHECK(!display.editor_open(0));
  CHECK(!display.editor_open(500));
  CHECK(!display.editor_open(1199));

  display.update("Indoor Temp     ", "     20 C       ", 1500);
  CHECK(display.have_frame());
  CHECK(display.editor_open(1500));
}

TEST_CASE(editor_open_true_just_under_settle_window_false_just_over) {
  Display display;
  display.update("Indoor Temp     ", "     20 C       ", 1000);  // line2 last changed at t=1000

  CHECK(display.editor_open(1000 + 1199));   // 1199ms since last blink: still open
  CHECK(!display.editor_open(1000 + 1200));  // 1200ms since last blink: settled/closed
}

TEST_CASE(editor_open_honours_a_custom_settle_ms) {
  Display display;
  display.set_settle_ms(500);
  display.update("Indoor Temp     ", "     20 C       ", 0);

  CHECK(display.editor_open(499));
  CHECK(!display.editor_open(500));
}

TEST_CASE(editor_open_reopens_on_a_fresh_line2_change) {
  Display display;
  display.update("Indoor Temp     ", "     20 C       ", 0);
  CHECK(!display.editor_open(2000));  // long settled

  display.update("Indoor Temp     ", "        C       ", 2000);  // blink: value blanks out
  CHECK(display.editor_open(2100));
}

// describe_unprintable() -- sanitize()'s diagnostic counterpart (display.h).
// Only the pure formatter is covered here: the gating/rate-limit logic that
// calls it lives in vent_axia.cpp, which tests/CMakeLists.txt deliberately
// excludes from the host build (it pulls in ESPHome headers), so it is
// verified only by the firmware compiles and, eventually, a live capture --
// see DISPLAY-INSTRUMENTATION-PLAN.md §4. Not restructuring the core just to
// make that two-line comparison testable.

TEST_CASE(describe_unprintable_returns_empty_for_all_ascii) {
  CHECK_EQ(describe_unprintable("Status          "), std::string(""));
}

TEST_CASE(describe_unprintable_reports_a_high_range_byte) {
  std::string line = "36%             ";  // 16 chars, matching a real display line
  CHECK_EQ(line.size(), static_cast<size_t>(16));  // guard before the write, not after
  line[15] = static_cast<char>(0xE0);              // the inferred alpha annunciator, column 15
  CHECK_EQ(describe_unprintable(line), std::string("col 15=0xE0"));
}

TEST_CASE(describe_unprintable_reports_a_control_range_byte) {
  std::string line = "AutoXXXXXXXXXXXX";
  // 0x07 is CGRAM slot 7, ASSUMED rather than measured to be the unit's
  // "Auto" glyph -- which CGRAM slots this unit actually loads is question 2
  // of the capture this instrumentation exists to make possible. The test
  // holds either way: it pins the control-range half of the formatter, and
  // any byte below 0x20 would do.
  line[4] = static_cast<char>(0x07);
  CHECK_EQ(describe_unprintable(line), std::string("col 4=0x07"));
}

TEST_CASE(describe_unprintable_joins_multiple_bytes_in_column_order) {
  std::string line = "AAAAAAAAAAAAAAAA";
  line[2] = static_cast<char>(0x01);
  line[9] = static_cast<char>(0xFF);
  line[15] = static_cast<char>(0x7F);
  CHECK_EQ(describe_unprintable(line), std::string("col 2=0x01, col 9=0xFF, col 15=0x7F"));
}

TEST_CASE(describe_unprintable_boundary_bytes_0x1f_0x20_0x7e_0x7f) {
  // 0x20 and 0x7E are the printable range's own edges (isprint() true for
  // both); 0x1F and 0x7F sit one step outside it on either side and must be
  // the only two reported.
  std::string line = "AAAA";
  line += static_cast<char>(0x1F);
  line += static_cast<char>(0x20);
  line += static_cast<char>(0x7E);
  line += static_cast<char>(0x7F);
  CHECK_EQ(describe_unprintable(line), std::string("col 4=0x1F, col 7=0x7F"));
}
