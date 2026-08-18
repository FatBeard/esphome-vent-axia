#include "test_framework.h"

#include <string>
#include <vector>

#include "display.h"
#include "frame_logger.h"
#include "frame_test_helper.h"
#include "protocol.h"

using namespace esphome::vent_axia;
using namespace vatest;

namespace {

/// Captures every debug line FrameLogger emits, in order -- FrameLogger is
/// the only portable-core user of Keypad::LogSink::debug (its own class
/// comment), so a fixture this small is not worth promoting into
/// sequence_test_helpers.h's shared RecordingLog, which several other test
/// files use without ever touching this severity.
struct RecordingLog {
  std::vector<std::string> debug_lines;

  Keypad::LogSink as_log_sink() {
    Keypad::LogSink sink;
    sink.debug = [this](const std::string &m) { this->debug_lines.push_back(m); };
    return sink;
  }
};

/// Builds a DisplayFrame directly -- FrameLogger takes one straight (its own
/// class comment: protocol.h/display.h are both portable core), so there is
/// no need to round-trip through build_rx_frame()'s raw bytes the way
/// test_protocol.cpp/test_display.cpp do. header/row addrs default to
/// distinct, non-zero, plausible values (frame_test_helper.h's own
/// reasoning) so a failing test can tell them apart from real data.
protocol::DisplayFrame make_frame(const std::string &line1_raw, const std::string &line2_raw,
                                   std::array<uint8_t, 4> header = {0x11, 0x22, 0x33, 0x44},
                                   uint8_t row1_addr = 0x80, uint8_t row2_addr = 0xC0) {
  protocol::DisplayFrame f;
  f.unknown_header = header;
  f.unknown_row1_addr = row1_addr;
  f.line1 = pad16(line1_raw);
  f.unknown_row2_addr = row2_addr;
  f.line2 = pad16(line2_raw);
  return f;
}

/// The humidity-boost fixture (status.cpp's has_sensor_boost_annunciator(),
/// status.h's own comment): glyphs::ALPHA at column 15 (the last of 16),
/// with an airflow percentage in the columns before it -- the two-per-second
/// ticking value that describe_unprintable() must NOT be sensitive to,
/// because column 15 is the only non-ASCII byte in the line.
std::string boost_line2(const std::string &percent) {
  std::string line2 = percent;
  line2.resize(15, ' ');
  line2 += static_cast<char>(esphome::vent_axia::glyphs::ALPHA);
  return line2;  // already 16 chars -- pad16() in make_frame() is a no-op on this
}

}  // namespace

TEST_CASE(first_frame_logs_the_unknown_byte_tuple_immediately_exempt_from_the_floor) {
  // first_unknown_log (frame_logger.cpp) bypasses RAW_LOG_MIN_INTERVAL_MS
  // entirely -- calling log() at now_ms=0, i.e. with no time at all having
  // elapsed since last_unknown_log_ms_'s zero-initialised default, must
  // still produce a line. All-ASCII text on both lines keeps this test
  // about only the unknown-byte tuple, not the non-ASCII paths below.
  FrameLogger logger;
  RecordingLog log;
  logger.set_log_sink(log.as_log_sink());

  logger.log(make_frame("Status Screen", "Normal Airflow"), 0);

  CHECK_EQ(static_cast<int>(log.debug_lines.size()), 1);
  CHECK(log.debug_lines[0].find("unknown_header=0x11 0x22 0x33 0x44") != std::string::npos);
  CHECK(log.debug_lines[0].find("unknown_row1_addr=0x80") != std::string::npos);
  CHECK(log.debug_lines[0].find("unknown_row2_addr=0xC0") != std::string::npos);
  // Genuinely the first line -- neither the revert-suppress nor the
  // heartbeat suffix applies here.
  CHECK(log.debug_lines[0].find("(unchanged") == std::string::npos);
  CHECK(log.debug_lines[0].find("also moved") == std::string::npos);
}

TEST_CASE(an_unchanged_tuple_stays_silent_until_the_heartbeat_is_due_then_logs_once) {
  FrameLogger logger;
  RecordingLog log;
  logger.set_log_sink(log.as_log_sink());

  const auto frame = make_frame("Status Screen", "Normal Airflow");

  logger.log(frame, 0);  // baseline -- first-frame exemption, see the test above
  CHECK_EQ(static_cast<int>(log.debug_lines.size()), 1);

  // Past RAW_LOG_MIN_INTERVAL_MS (floor clear) but well short of
  // RAW_LOG_HEARTBEAT_MS -- unchanged and no suppressed revert pending, so
  // this must stay silent.
  logger.log(frame, FrameLogger::RAW_LOG_MIN_INTERVAL_MS);
  CHECK_EQ(static_cast<int>(log.debug_lines.size()), 1);

  // The heartbeat firing is the only thing that should produce a SECOND
  // line for a tuple that has never actually changed.
  logger.log(frame, FrameLogger::RAW_LOG_HEARTBEAT_MS);
  CHECK_EQ(static_cast<int>(log.debug_lines.size()), 2);
  CHECK(log.debug_lines[1].find("(unchanged -- heartbeat)") != std::string::npos);
}

TEST_CASE(a_change_that_reverts_inside_the_rate_limit_window_still_logs_that_it_happened) {
  // The path that has never been testable before this move: a tuple that
  // changes and reverts entirely within RAW_LOG_MIN_INTERVAL_MS leaves no
  // trace in last_logged_unknown_header_ (the floor blocks the update, not
  // just the log line) -- unknown_change_suppressed_ is what remembers a
  // blink happened so the next eligible frame still reports it, exactly the
  // sub-2-second blink-attribute signal frame_logger.h's class comment
  // names as the reason this class exists.
  FrameLogger logger;
  RecordingLog log;
  logger.set_log_sink(log.as_log_sink());

  const auto steady = make_frame("Status Screen", "Normal Airflow", {0x11, 0x22, 0x33, 0x44});
  const auto blinked = make_frame("Status Screen", "Normal Airflow", {0x11, 0x22, 0x33, 0x55});

  logger.log(steady, 0);  // baseline
  CHECK_EQ(static_cast<int>(log.debug_lines.size()), 1);

  // Inside the 2000ms floor: changes, but the floor blocks both the log AND
  // the update to last_logged_unknown_header_ -- so as far as that stored
  // value is concerned, nothing here ever happened yet.
  logger.log(blinked, 500);
  CHECK_EQ(static_cast<int>(log.debug_lines.size()), 1);

  // Reverts, STILL inside the floor relative to the baseline at t=0 (floor
  // clears at t=2000) -- so this is silent too, but unknown_change_suppressed_
  // is now carrying the fact that a change happened.
  logger.log(steady, 1800);
  CHECK_EQ(static_cast<int>(log.debug_lines.size()), 1);

  // Floor clear, tuple reads as "unchanged" against last_logged_unknown_header_
  // (which is still `steady`'s header -- it was never overwritten) -- but the
  // suppressed flag forces a line anyway, saying so.
  logger.log(steady, 2000);
  CHECK_EQ(static_cast<int>(log.debug_lines.size()), 2);
  CHECK(log.debug_lines[1].find("(these bytes also moved and came back inside the rate limit)") != std::string::npos);
}

TEST_CASE(a_line_whose_description_is_unchanged_does_not_relog_even_though_the_raw_line_changed) {
  // The humidity-boost case (status.cpp's has_sensor_boost_annunciator(),
  // status.h's own comment): line2's airflow percentage ticks every frame
  // while the annunciator byte at column 15 sits still. describe_unprintable()
  // only reports the non-ASCII byte, so two different percentages describe
  // identically, and gating on the FORMATTER'S OUTPUT (not the raw line)
  // must keep this silent on the second frame.
  FrameLogger logger;
  RecordingLog log;
  logger.set_log_sink(log.as_log_sink());

  auto count_line2_lines = [&] {
    int n = 0;
    for (const auto &line : log.debug_lines) {
      if (line.find("line2 non-ASCII") != std::string::npos) {
        n++;
      }
    }
    return n;
  };

  // now_ms=5000, well past last_line2_log_ms_'s zero-initialised default, so
  // this baseline call is not itself blocked by the floor.
  logger.log(make_frame("Boost Airflow", boost_line2("48%")), 5000);
  CHECK_EQ(count_line2_lines(), 1);

  // 2500ms later -- past RAW_LOG_MIN_INTERVAL_MS, so the floor is NOT why
  // this stays silent -- with a DIFFERENT percentage but the SAME column-15
  // byte, hence the SAME description.
  logger.log(make_frame("Boost Airflow", boost_line2("49%")), 7500);
  CHECK_EQ(count_line2_lines(), 1);
}
