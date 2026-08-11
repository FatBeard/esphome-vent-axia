#pragma once

// The diagnostic page/field table and its dispatch (PLAN.md §4), replacing
// mhrv_orig/diagnostic_sensors.yaml's 14 hand-written lambdas with data.
// Plain C++17, no ESPHome headers -- see README "Portable core".
//
// Decoding is entirely passive, driven off the display: whatever page the
// unit (or a later stage's scrape sequence) happens to be showing gets fed
// through decode_page() below. There is no polling and nothing in this file
// transmits or navigates anything.

#include <functional>
#include <string>

#include "entities.h"

namespace esphome {
namespace vent_axia {
namespace diagnostics {

/// Where a decoded value goes. Plain std::function members -- the same
/// shape as Display::ChangeCallback -- so decode_page() can hand values to
/// the hub without this file knowing that sensor::Sensor et al. exist. Only
/// called for a field that actually parsed: a blank field (parser::parse_field
/// returning false) simply never invokes the corresponding member, which is
/// what keeps "the unit reported nothing here" from ever becoming a
/// published 0/false.
struct Sink {
  std::function<void(SensorKey, int)> publish_sensor;
  std::function<void(BinaryKey, bool)> publish_binary;
  std::function<void(TextKey, const std::string &)> publish_text;

  /// Page 23's filter-hours-reaching-zero, reported rather than published
  /// directly through publish_binary. FILTER_CHANGE_DUE already has a live
  /// source -- status::StatusTracker, off the status screen's "Check
  /// Filter" message, refreshed roughly 3x/second -- and only the hub can
  /// decide how the two reconcile (PLAN.md risk 7: they are expected to
  /// agree, and a disagreement is worth a log line, not a silent pick). A
  /// caller that does not care about the cross-check -- e.g. a test -- can
  /// simply leave this unset.
  std::function<void(bool)> report_filter_change_due;
};

/// Feeds one diagnostic page's line2 through the page/field table. Safe to
/// call with ANY page number: an entry the table doesn't have -- 5, 9, 10,
/// 12-18, 20-22, an untried 27, or anything else including a page 28 that
/// does not exist on firmware V32/05 (see diagnostics.cpp) -- is a no-op,
/// deliberately, never an error. That is what makes an unrecognised page
/// harmless: the old component's scrape hung forever waiting for a page
/// number it treated as a required terminator; this one simply has nothing
/// to do for a page it does not understand and returns.
///
/// Publishing the raw "NN: <line2>" text sensor and firing the
/// on_diagnostic_page trigger for every page -- decoded or not -- is the
/// hub's job (vent_axia.cpp calls format_raw_page() below directly), not
/// this function's: both are ESPHome-automation concerns and belong on the
/// other side of the portable-core line.
void decode_page(int page, const std::string &line2, const Sink &sink);

/// Formats a page for the optional `raw_diagnostic_page` text sensor and
/// the on_diagnostic_page trigger payload: "NN: <line2>", e.g.
/// "07: 0000 1 0 000 00 ". line2 is passed through verbatim, untrimmed --
/// this is the raw escape hatch, so it shows exactly what arrived. Works
/// for any page number, including ones decode_page() has no table entry
/// for, which is the point: a page nobody has decoded yet is still visible
/// to a human without a component change.
std::string format_raw_page(int page, const std::string &line2);

}  // namespace diagnostics
}  // namespace vent_axia
}  // namespace esphome
