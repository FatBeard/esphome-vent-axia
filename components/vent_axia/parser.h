#pragma once

// Numeric and text field extraction ported from mhrv_orig/vask_decode.h,
// verbatim in rule if not in code shape. Plain C++17, no ESPHome headers --
// see README "Portable core". Used by status.cpp and heavily by the
// diagnostics page table -- every page field goes through parse_field.

#include <cstddef>
#include <string>

namespace esphome {
namespace vent_axia {
namespace parser {

/// Reads the integer occupying s[pos, pos+len) into out. Returns false --
/// leaving out UNTOUCHED -- if the line is too short, the field is blank, or
/// it contains anything that is not a digit/space/sign.
///
/// Blank is not zero, and that distinction is the entire reason this
/// function exists rather than a one-line atoi() call: the unit leaves a
/// field blank when it has nothing to report (no sensor fitted, a value not
/// yet settled, ...), and publishing 0 in that case would assert "the
/// reading is zero" when the truth is "there is no reading" -- two different
/// facts. Every caller depends on out being left alone on failure so it can
/// simply skip publishing rather than publish a wrong number.
///
/// Spaces and a leading sign are accepted, not just digits, because that is
/// what the -20..50 degC temperature fields actually render, e.g. "-05" or
/// " -5". A field with no digit at all -- all spaces, or a lone sign -- is
/// still a failure: a sign with nothing to sign is not a value.
bool parse_field(const std::string &s, size_t pos, size_t len, int &out);

/// Trailing-space strip. There is exactly one implementation of this in the
/// codebase -- screens::trim_trailing -- and this is a thin alias over it so
/// parser.cpp and screens.cpp can never drift apart on what "trim" means.
std::string trim(const std::string &s);

/// Reads "On"/"ON"/"on" or "Off"/"OFF"/"off" (after trim) into out. False --
/// out untouched -- for anything else, including the blank line the unit
/// renders while the value is mid-blink: a caller must not treat "I could
/// not read it this frame" as "it is off".
bool parse_on_off(const std::string &s, bool &out);

// ---------------------------------------------------------------- Set Clock --
// line2 of the Set Clock screen is "Ddd HH:MM". While a field is being
// edited the unit blanks it on alternate frames (the same blink
// Display::editor_open relies on), so a caller must only trust a frame that
// clock_rendered() passed -- clock_day/hour/minute below assume that has
// already happened and will read garbage out of a blinking field otherwise.

/// Validates the whole "Ddd HH:MM" layout char by char: size >= 9, [0..2]
/// alphabetic, [3]==' ', [4..5] digits, [6]==':', [7..8] digits. Exists so a
/// mid-blink frame -- e.g. "    23:49" with the day blanked -- is rejected
/// outright rather than partially decoded.
bool clock_rendered(const std::string &s);

/// Mon=0 .. Sun=6, or -1 if unrecognised.
int clock_day(const std::string &s);

/// Assumes clock_rendered(s) passed -- no bounds or digit checking here.
int clock_hour(const std::string &s);

/// Assumes clock_rendered(s) passed -- no bounds or digit checking here.
int clock_minute(const std::string &s);

/// ESPHome's ESPTime::day_of_week is 1=Sunday..7=Saturday; the display's week
/// runs Mon=0..Sun=6.
int dow_to_display(int esphome_day_of_week);

/// Fewest signed presses from cur to want in a field that wraps at mod --
/// positive is "up", negative is "down". The exact-half case rounds to "up"
/// (the `<=`, kept from vask_decode.h) rather than picking a direction
/// arbitrarily.
///
/// This is for hour (mod 24) and minute (mod 60) ONLY. The day field does
/// not wrap (Up on Sun does nothing) and neither temperature field wraps --
/// using this on any of those would silently produce a small wrapped-around
/// delta instead of the large, correct, non-wrapping one.
int wrapped_delta(int cur, int want, int mod);

}  // namespace parser
}  // namespace vent_axia
}  // namespace esphome
