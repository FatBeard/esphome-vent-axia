#include "display.h"

#include <cctype>

namespace esphome {
namespace vent_axia {

std::string sanitize(const std::string &raw) {
  std::string out = raw;
  for (char &c : out) {
    if (std::isprint(static_cast<unsigned char>(c)) == 0) {
      c = '*';
    }
  }
  return out;
}

void Display::update(const std::string &raw_line1, const std::string &raw_line2, uint32_t now_ms) {
  const std::string s1 = sanitize(raw_line1);
  const std::string s2 = sanitize(raw_line2);

  have_frame_ = true;

  bool line1_changed = false;
  bool line2_changed = false;

  if (s1 != line1_) {
    line1_ = s1;
    line1_changed_at_ms_ = now_ms;
    line1_changed = true;
  }
  if (s2 != line2_) {
    line2_ = s2;
    line2_changed_at_ms_ = now_ms;
    line2_changed = true;
  }

  if ((line1_changed || line2_changed) && on_change_) {
    on_change_(line1_changed, line2_changed);
  }
}

}  // namespace vent_axia
}  // namespace esphome
