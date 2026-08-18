#include "frame_logger.h"

#include "display.h"

namespace esphome {
namespace vent_axia {

namespace {

/// Renders one byte as "0xXX" (uppercase hex, always two digits) via table
/// lookup rather than <cstdio>'s snprintf or <sstream>'s ostringstream --
/// same house style as display.cpp's own to_hex_byte() (not reused directly:
/// that one is private to display.cpp's anonymous namespace).
std::string to_hex_byte(unsigned char byte) {
  static const char digits[] = "0123456789ABCDEF";
  std::string out = "0x";
  out += digits[(byte >> 4) & 0x0F];
  out += digits[byte & 0x0F];
  return out;
}

}  // namespace

void FrameLogger::log(const protocol::DisplayFrame &frame, uint32_t now_ms) {
  // (a) The six frame bytes protocol.cpp parses but nothing else reads:
  // unknown_header[0..3] (bytes 1..4) and unknown_row1_addr/unknown_row2_addr
  // (bytes 5 and 22, "probably an HD44780 DDRAM address" per protocol.h's own
  // comment -- never confirmed). Gated on the 6-byte tuple changing AND a
  // 2000ms floor rather than either alone, because nobody knows what these
  // bytes are yet: a byte that's genuinely constant gives one line and tells
  // you everything; a byte that toggles with editor state (a cursor/blink
  // attribute) stays visible; a byte that differs on every frame (a counter
  // or frame-phase value) would flood the log at the link's ~3.3 frames/s
  // without the floor -- with it, that case shows up as a steady cadence AT
  // the 2000ms limit, and that cadence is itself the answer: it says "this
  // byte moves every frame" without needing to prove it 3.3 times a second.
  //
  // The floor only bites for the 2000ms following a line that actually got
  // logged; outside that window every frame is eligible, so a first flip is
  // caught immediately. What it would otherwise lose is a change that also
  // REVERTS inside that window -- and a blink attribute at the ~350ms editor
  // cadence is precisely that shape, so losing it silently would answer
  // question 3 "no" when the truth was "yes". unknown_change_suppressed_
  // carries the fact forward instead: the next eligible frame logs even if
  // the bytes are back where they started, and says that it happened.
  const bool first_unknown_log = !this->have_logged_unknown_bytes_;
  const bool unknown_changed = first_unknown_log ||
                                frame.unknown_header != this->last_logged_unknown_header_ ||
                                frame.unknown_row1_addr != this->last_logged_unknown_row1_addr_ ||
                                frame.unknown_row2_addr != this->last_logged_unknown_row2_addr_;
  // The first frame is exempt from the floor: one baseline line at link-up
  // costs nothing and is the reading everything else is read against.
  const bool unknown_floor_clear =
      first_unknown_log || (now_ms - this->last_unknown_log_ms_) >= RAW_LOG_MIN_INTERVAL_MS;
  if (unknown_changed && !unknown_floor_clear) {
    this->unknown_change_suppressed_ = true;
  }
  // The heartbeat re-emits an unchanged tuple every RAW_LOG_HEARTBEAT_MS --
  // see its comment (frame_logger.h) for why log-on-change alone is
  // unobservable here. It is longer than the floor, so it cannot fight it.
  const bool unknown_heartbeat_due = (now_ms - this->last_unknown_log_ms_) >= RAW_LOG_HEARTBEAT_MS;
  if ((unknown_changed || this->unknown_change_suppressed_ || unknown_heartbeat_due) && unknown_floor_clear) {
    if (this->log_.debug) {
      const std::string suffix = (this->unknown_change_suppressed_ && !unknown_changed)
                                      ? "  (these bytes also moved and came back inside the rate limit)"
                                      : (unknown_changed ? "" : "  (unchanged -- heartbeat)");
      this->log_.debug("raw frame: unknown_header=" + to_hex_byte(frame.unknown_header[0]) + " " +
                        to_hex_byte(frame.unknown_header[1]) + " " + to_hex_byte(frame.unknown_header[2]) + " " +
                        to_hex_byte(frame.unknown_header[3]) +
                        ", unknown_row1_addr=" + to_hex_byte(frame.unknown_row1_addr) +
                        ", unknown_row2_addr=" + to_hex_byte(frame.unknown_row2_addr) + suffix);
    }
    this->last_logged_unknown_header_ = frame.unknown_header;
    this->last_logged_unknown_row1_addr_ = frame.unknown_row1_addr;
    this->last_logged_unknown_row2_addr_ = frame.unknown_row2_addr;
    this->have_logged_unknown_bytes_ = true;
    this->unknown_change_suppressed_ = false;
    this->last_unknown_log_ms_ = now_ms;
  }

  // (b) Non-ASCII content in the two text lines, via describe_unprintable()
  // (display.h) -- this class's whole reason to exist. Gated on the
  // FORMATTER'S OUTPUT changing, not the raw line: a humidity boost ticks the
  // airflow percentage in line2 every frame while the annunciator byte sits
  // still in column 15 (status.cpp's humidity_boost decode), so gating on the
  // raw line would re-log an unchanged "col 15=0x??" several times a second
  // for as long as the boost lasts. Gating on the description means a steady
  // annunciator logs once and stays quiet until the non-ASCII content itself
  // actually moves.
  //
  // A clearing transition is logged as "(none)" rather than passed over in
  // silence. The falling edge is a finding in its own right -- the
  // annunciator *clearing* is the one part of humidity_boost still
  // unobserved, and an observer should not have to infer it from log lines
  // that stopped arriving.
  //
  // The same RAW_LOG_MIN_INTERVAL_MS floor applies here, per line. Without it
  // the description itself can oscillate: an open editor blinks line2
  // between its value and blank every ~350ms, so a non-ASCII byte anywhere in
  // that value alternates the description non-empty/empty at ~1.6 lines/s --
  // and LeaveMenu deliberately waits out the unit's ~2-minute editor timeout,
  // which is ~190 lines from one excursion on a network-only logger. The cost
  // is that an edge can be stamped up to 2000ms late if the same line logged
  // just before it. The stored description is NOT updated when the floor
  // suppresses a change, so the change is simply re-detected on the next
  // eligible frame rather than lost.
  //
  // The heartbeat repeats a line only while it HAS non-ASCII content: the
  // state a late-connecting observer needs restated (a boost already running
  // when they connect). An all-ASCII line stays silent -- the unknown-byte
  // heartbeat above already proves once a minute that this class is running,
  // so repeating "(none)" on both lines too would be three lines a minute to
  // say the same thing.
  const std::string desc1 = describe_unprintable(frame.line1);
  const bool line1_repeat = desc1 == this->last_logged_line1_unprintable_;
  if ((!line1_repeat || (!desc1.empty() && (now_ms - this->last_line1_log_ms_) >= RAW_LOG_HEARTBEAT_MS)) &&
      (now_ms - this->last_line1_log_ms_) >= RAW_LOG_MIN_INTERVAL_MS) {
    if (this->log_.debug) {
      this->log_.debug("raw frame: line1 non-ASCII: " + (desc1.empty() ? std::string("(none)") : desc1) +
                        (line1_repeat ? "  (unchanged -- heartbeat)" : ""));
    }
    this->last_logged_line1_unprintable_ = desc1;
    this->last_line1_log_ms_ = now_ms;
  }
  const std::string desc2 = describe_unprintable(frame.line2);
  const bool line2_repeat = desc2 == this->last_logged_line2_unprintable_;
  if ((!line2_repeat || (!desc2.empty() && (now_ms - this->last_line2_log_ms_) >= RAW_LOG_HEARTBEAT_MS)) &&
      (now_ms - this->last_line2_log_ms_) >= RAW_LOG_MIN_INTERVAL_MS) {
    if (this->log_.debug) {
      this->log_.debug("raw frame: line2 non-ASCII: " + (desc2.empty() ? std::string("(none)") : desc2) +
                        (line2_repeat ? "  (unchanged -- heartbeat)" : ""));
    }
    this->last_logged_line2_unprintable_ = desc2;
    this->last_line2_log_ms_ = now_ms;
  }
}

}  // namespace vent_axia
}  // namespace esphome
