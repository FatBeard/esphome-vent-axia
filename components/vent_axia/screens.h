#pragma once

// Pure string predicates over display line1, used to classify which screen
// the unit is showing. Plain C++17, no ESPHome headers -- see README
// "Portable core".

#include <optional>
#include <string>

namespace esphome {
namespace vent_axia {
namespace screens {

enum class ScreenKind {
  STATUS,
  SET_CLOCK,
  SUMMER_MODE,
  INDOOR_TEMP,
  OUTDOOR_TEMP,
  DIAGNOSTIC,
};

/// Case-insensitive "does `s` start with `prefix`". Some of the reference
/// strings below (see is_menu_screen) came from the printed manual, whose
/// casing is inconsistent, rather than from the unit itself -- so every
/// match in this file goes through here rather than trusting either source's
/// case to be right.
bool starts_with_ci(const std::string &s, const std::string &prefix);

/// True for the handful of screens known to be menu entries: "Set Clock",
/// "Summer Mode", "Indoor Temp", "Outdoor Temp", "Diagnostic". Anything else
/// is treated as the status loop.
///
/// Deliberately a whitelist of menu screens rather than a blacklist of
/// status screens: the status loop's contents vary with which options and
/// sensors are fitted to a given unit, and the manual never claims to
/// enumerate it completely. "Not in this list" is safe to mean "status" --
/// worst case a not-yet-seen menu screen is briefly misread as status.
/// The reverse (a status screen misread as a menu) is the direction that
/// would actually be dangerous once later stages gate keypresses on it.
bool is_menu_screen(const std::string &line1);

bool is_diagnostic_screen(const std::string &line1);

/// Extracts the page number from a "Diagnostic  NN" line1 (the literal word,
/// two spaces, two ASCII digits at offsets 12..13). Returns nullopt for
/// anything that doesn't match exactly -- including a short or mid-blink
/// line -- rather than guessing. No exceptions: this targets an ESP8266
/// build with exceptions disabled, and std::stoi on a non-numeric page was a
/// documented cause of a reboot in the component this replaces.
std::optional<int> diagnostic_page(const std::string &line1);

std::string trim_trailing(const std::string &s);

ScreenKind classify(const std::string &line1);

}  // namespace screens
}  // namespace vent_axia
}  // namespace esphome
