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

// non_printable_glyphs_are_replaced_with_asterisk (this test's pre-stage-16
// name) asserted the single-lane collapse: EVERY non-printable byte reads
// back as the same '*' -- a many-to-one ambiguity that made two distinct
// bytes in one column indistinguishable. That premise is gone, replaced
// below with the two-lane behaviour: the raw lane keeps the byte exactly,
// the text lane renders it distinguishably.
TEST_CASE(raw_lane_keeps_the_byte_text_lane_renders_it) {
  Display display;
  std::string line1 = "Auto";
  // Assumed to be the unit's "Auto" glyph (CGRAM slot 7), never measured --
  // see the describe_unprintable() control-range case below. What this test
  // pins is that the byte survives to the raw lane, whatever it turns out
  // to mean.
  line1 += static_cast<char>(0x07);
  line1 += "           ";            // pad to 16
  CHECK_EQ(line1.size(), static_cast<size_t>(16));

  display.update(line1, "                ", 0);

  // Raw lane: the byte at index 4 is exactly 0x07, untouched.
  CHECK_EQ(display.raw_line1(), line1);
  CHECK_EQ(static_cast<int>(static_cast<unsigned char>(display.raw_line1()[4])), 0x07);
  // Text lane: the SAME byte, hex-escaped since it has no measured mapping
  // (glyphs::ALPHA is the only byte that does).
  CHECK_EQ(display.text_line1(), std::string("Auto<07>           "));
}

// THE latent bug this guards against: deduplicating on a lossy
// representation (sanitize()'s old '*' collapse, or any other many-to-one
// map) makes a change from one non-printable byte to a DIFFERENT
// non-printable byte in the SAME column invisible, because both collapse to
// the same representation and the dedup compare never sees a difference.
// Dedup now runs on the raw lane, so this must fire twice.
TEST_CASE(two_different_non_printable_bytes_in_the_same_column_both_fire_change) {
  Display display;
  int changes = 0;
  bool seen_l2 = false;
  display.set_on_change([&](bool, bool l2) {
    changes++;
    seen_l2 = l2;
  });

  std::string line2a(16, ' ');
  line2a[0] = '1';
  line2a[1] = '8';
  line2a[2] = '%';
  line2a[15] = static_cast<char>(0x07);  // some CGRAM glyph
  CHECK_EQ(line2a.size(), static_cast<size_t>(16));
  display.update("Status          ", line2a, 0);
  CHECK_EQ(changes, 1);
  CHECK(seen_l2);

  std::string line2b = line2a;
  line2b[15] = static_cast<char>(0xFF);  // a DIFFERENT non-printable byte, same column
  display.update("Status          ", line2b, 300);
  // Pre-stage-16, both 0x07 and 0xFF sanitized to the same '*' and this
  // second update() would have reported NO change at all -- the bug this
  // test exists to catch.
  CHECK_EQ(changes, 2);
  CHECK(seen_l2);
  CHECK_EQ(display.raw_line2(), line2b);
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

// to_utf8() -- the byte->UTF-8 transcode table (display.h), built on the
// single measured code this unit has (glyphs::ALPHA = 0xE0, captured live)
// and hex-escaping everything else rather than guessing from a datasheet.

TEST_CASE(to_utf8_passes_ascii_through_unchanged) {
  CHECK_EQ(to_utf8("Status          "), std::string("Status          "));
  CHECK_EQ(to_utf8(""), std::string(""));
}

TEST_CASE(to_utf8_renders_the_measured_alpha_byte) {
  std::string line = "36%            ";
  line += static_cast<char>(glyphs::ALPHA);  // column 15, the measured byte
  CHECK_EQ(to_utf8(line), std::string("36%            \xCE\xB1"));
}

TEST_CASE(to_utf8_hex_escapes_an_unmeasured_high_byte) {
  std::string line(16, 'A');
  line[15] = static_cast<char>(0xDF);  // the datasheet's degree symbol -- never measured here
  CHECK_EQ(to_utf8(line), std::string("AAAAAAAAAAAAAAA<DF>"));
}

TEST_CASE(to_utf8_hex_escapes_a_control_range_byte) {
  std::string line = "AutoXXXXXXXXXXXX";
  line[4] = static_cast<char>(0x07);  // CGRAM slot 7 -- see describe_unprintable's own comment below
  CHECK_EQ(to_utf8(line), std::string("Auto<07>XXXXXXXXXXX"));
}

TEST_CASE(to_utf8_boundary_bytes_0x1f_0x20_0x7e_0x7f) {
  // 0x20 and 0x7E are the printable range's own edges and must pass through
  // unchanged; 0x1F and 0x7F sit one step outside it on either side and
  // must each become a hex escape.
  std::string line;
  line += static_cast<char>(0x1F);
  line += static_cast<char>(0x20);
  line += static_cast<char>(0x7E);
  line += static_cast<char>(0x7F);
  CHECK_EQ(to_utf8(line), std::string("<1F> ~<7F>"));
}

TEST_CASE(to_utf8_a_mixed_line_combines_all_three_cases) {
  std::string line = "18%";
  line += static_cast<char>(glyphs::ALPHA);
  line += "m";
  line += static_cast<char>(0x01);
  CHECK_EQ(to_utf8(line), std::string("18%\xCE\xB1m<01>"));
}

TEST_CASE(to_utf8_output_never_contains_a_nul_byte) {
  // A literal 0x00 anywhere in the result would truncate anything passing
  // it through .c_str() downstream -- every
  // 0x00-0x1F control byte must come out as a multi-character hex escape,
  // never as itself.
  std::string line(16, '\0');
  const std::string out = to_utf8(line);
  CHECK(out.find('\0') == std::string::npos);
  CHECK_EQ(out, std::string("<00><00><00><00><00><00><00><00><00><00><00><00><00><00><00><00>"));
}

// describe_unprintable() -- to_utf8()'s diagnostic counterpart (display.h).
// Only the pure formatter is covered here: the gating/rate-limit logic that
// calls it lives in vent_axia.cpp, which tests/CMakeLists.txt deliberately
// excludes from the host build (it pulls in ESPHome headers), so it is
// verified only by the firmware compiles and, eventually, a live capture.
// Not restructuring the core just to make that two-line comparison
// testable.

TEST_CASE(describe_unprintable_returns_empty_for_all_ascii) {
  CHECK_EQ(describe_unprintable("Status          "), std::string(""));
}

TEST_CASE(describe_unprintable_reports_a_high_range_byte) {
  std::string line = "36%             ";  // 16 chars, matching a real display line
  CHECK_EQ(line.size(), static_cast<size_t>(16));  // guard before the write, not after
  line[15] = static_cast<char>(0xE0);              // the measured alpha annunciator, column 15
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
