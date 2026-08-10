#include "screens.h"

#include <cctype>

namespace esphome {
namespace vent_axia {
namespace screens {

bool starts_with_ci(const std::string &s, const std::string &prefix) {
  if (s.size() < prefix.size()) {
    return false;
  }
  for (size_t i = 0; i < prefix.size(); i++) {
    const unsigned char a = static_cast<unsigned char>(s[i]);
    const unsigned char b = static_cast<unsigned char>(prefix[i]);
    if (std::tolower(a) != std::tolower(b)) {
      return false;
    }
  }
  return true;
}

// Defined in terms of classify() rather than its own prefix list. The two
// would otherwise be parallel copies of the same five screen names, and a
// later stage adding a screen would have to remember to edit both -- with a
// divergence showing up as a menu screen being mistaken for the status loop,
// which is precisely the failure this predicate exists to prevent.
bool is_menu_screen(const std::string &line1) { return classify(line1) != ScreenKind::STATUS; }

bool is_diagnostic_screen(const std::string &line1) { return starts_with_ci(line1, "Diagnostic"); }

std::optional<int> diagnostic_page(const std::string &line1) {
  if (!is_diagnostic_screen(line1) || line1.size() < 14) {
    return std::nullopt;
  }
  const unsigned char d0 = static_cast<unsigned char>(line1[12]);
  const unsigned char d1 = static_cast<unsigned char>(line1[13]);
  if (std::isdigit(d0) == 0 || std::isdigit(d1) == 0) {
    return std::nullopt;
  }
  return (d0 - '0') * 10 + (d1 - '0');
}

std::string trim_trailing(const std::string &s) {
  size_t end = s.size();
  while (end > 0 && s[end - 1] == ' ') {
    end--;
  }
  return s.substr(0, end);
}

// The single list of known menu screens; everything else is the status loop.
//
// Matching is case-insensitive throughout, deliberately. "Diagnostic" and
// "Set Clock" were captured from this unit's own serial output and are known
// good, but "Summer Mode" and the two temperature screens are transcribed from
// the printed manual, which is not consistent about case. Rather than have
// call sites remember which strings are trustworthy, all five are matched the
// forgiving way.
ScreenKind classify(const std::string &line1) {
  if (starts_with_ci(line1, "Diagnostic")) {
    return ScreenKind::DIAGNOSTIC;
  }
  if (starts_with_ci(line1, "Set Clock")) {
    return ScreenKind::SET_CLOCK;
  }
  if (starts_with_ci(line1, "Summer Mode")) {
    return ScreenKind::SUMMER_MODE;
  }
  if (starts_with_ci(line1, "Indoor Temp")) {
    return ScreenKind::INDOOR_TEMP;
  }
  if (starts_with_ci(line1, "Outdoor Temp")) {
    return ScreenKind::OUTDOOR_TEMP;
  }
  return ScreenKind::STATUS;
}

}  // namespace screens
}  // namespace vent_axia
}  // namespace esphome
