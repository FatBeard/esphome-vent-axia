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
  line1 += static_cast<char>(0x07);  // the unit's non-ASCII "Auto" glyph
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
