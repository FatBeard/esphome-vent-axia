#pragma once

// Instrumentation only -- no decode behaviour depends on this class. Built
// (stage 15, DISPLAY-REVIEW.md §6/§7 and DISPLAY-INSTRUMENTATION-PLAN.md) to
// make a live capture (GET /events during a humidity boost, per CLAUDE.md)
// settle three questions without guessing from a datasheet. Question 1
// (which byte the alpha annunciator is) is now answered -- glyphs::ALPHA,
// display.h -- and stage 16 built the two-lane split on that measurement.
// Still open: which CGRAM slots (0x00-0x07) this unit uses, and whether
// unknown_row1_addr/unknown_row2_addr carry a cursor or blink attribute
// during an OPEN EDITOR specifically (stage 15's own capture never opened
// one) that could replace Display::editor_open()'s 1200ms staleness
// heuristic with a direct protocol read -- that heuristic is the one
// CLAUDE.md records as having silently taken a 14C setpoint to 19C. See
// DISPLAY-INSTRUMENTATION-PLAN.md and DISPLAY-REVIEW.md §6/§7.
//
// Originally a VentAxiaHub method (log_raw_frame_bytes_()) with nine
// supporting hub members; moved into the portable core so its rate-limit,
// heartbeat and change-suppress-and-revert logic can be driven by explicit
// timestamps in a host test instead of living only in vent_axia.cpp, which
// the host test suite cannot compile (README "Portable core"). The
// suppress-and-revert path in particular exists specifically to catch a
// sub-2-second blink attribute during an open editor and, before this move,
// could not be exercised at all.
//
// Plain C++17, no ESPHome headers -- see README "Portable core". Uses
// Keypad::LogSink, not a type of its own, same reasoning as Runner reusing it
// (sequence.h): this file must stay ESPHome-free but some events here are
// meant to be loud (or, here, merely chatty at DEBUG -- see LogSink::debug's
// own comment, keypad.h).

#include <array>
#include <cstdint>
#include <string>

#include "keypad.h"
#include "protocol.h"

namespace esphome {
namespace vent_axia {

class FrameLogger {
 public:
  using LogSink = Keypad::LogSink;

  void set_log_sink(LogSink sink) { this->log_ = std::move(sink); }

  /// Call once per received frame, BEFORE Display::update() -- must be fed
  /// frame.line1/line2, the strings straight off the wire for THIS frame,
  /// never Display::raw_line1()/raw_line2(): at the point the hub calls this,
  /// the raw lane still holds the PREVIOUS frame's text, not the one being
  /// logged.
  void log(const protocol::DisplayFrame &frame, uint32_t now_ms);

  /// Floor between log lines, applied separately to the unknown-byte tuple
  /// and to each line's non-ASCII description -- see log()'s own comment for
  /// why the change-gate alone isn't enough on either path (frames arrive
  /// ~3.3/s; anything that varies per frame would otherwise flood a
  /// network-only logger at that rate for as long as it kept varying).
  static constexpr uint32_t RAW_LOG_MIN_INTERVAL_MS = 2000;

  /// Interval at which log() re-emits a line even though nothing changed.
  /// Log-on-change alone is unobservable on this device: the logger is
  /// network-only (mhrv.yaml sets `baud_rate: 0`, since UART0 is the MVHR
  /// link), so the first-frame baseline is written a few hundred ms after
  /// boot, long before WiFi and the /events stream are up, and goes nowhere.
  /// If the unknown bytes are then constant -- the likeliest case -- nothing
  /// is ever logged again, and an observer connecting later cannot tell
  /// "these bytes never move" from "the instrumentation is broken". The same
  /// holds for an annunciator that appeared before they connected. One line
  /// a minute costs nothing on a logger that only transmits to a connected
  /// client, and converts both silences into a positive statement.
  static constexpr uint32_t RAW_LOG_HEARTBEAT_MS = 60000;

 private:
  LogSink log_;

  // Anti-flood state. Non-ASCII line descriptions gate on the FORMATTER'S
  // OUTPUT changing rather than the raw line -- during a humidity boost the
  // airflow percentage in line2 ticks every frame while the annunciator byte
  // sits still in column 15, and raw-line gating would re-log an unchanged
  // "col 15=0x??" several times a second for as long as the boost lasts.
  // Every path additionally gates on RAW_LOG_MIN_INTERVAL_MS above, each
  // against its own last-logged timestamp so a chatty line cannot silence
  // the other one.
  //
  // have_logged_unknown_bytes_ does two jobs: it distinguishes "never
  // logged" from a zero-initialised tuple that happens to match a real
  // frame's, and it exempts the very first frame from the rate limit, so the
  // at-rest baseline -- the most informative line in the whole capture -- is
  // stamped when the link comes up rather than up to 2s later.
  //
  // unknown_change_suppressed_ closes the one hole the floor would otherwise
  // open: a tuple that changes and reverts entirely inside the rate limit
  // leaves no trace, and question 3 (a cursor or blink attribute) is exactly
  // a sub-2s signal. The flag remembers that something moved, so the next
  // eligible frame logs and says so even if the bytes have since gone back
  // to their previous values.
  std::string last_logged_line1_unprintable_;
  std::string last_logged_line2_unprintable_;
  uint32_t last_line1_log_ms_{0};
  uint32_t last_line2_log_ms_{0};
  std::array<uint8_t, 4> last_logged_unknown_header_{};
  uint8_t last_logged_unknown_row1_addr_{0};
  uint8_t last_logged_unknown_row2_addr_{0};
  bool have_logged_unknown_bytes_{false};
  bool unknown_change_suppressed_{false};
  uint32_t last_unknown_log_ms_{0};
};

}  // namespace vent_axia
}  // namespace esphome
