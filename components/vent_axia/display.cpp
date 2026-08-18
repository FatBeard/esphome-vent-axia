#include "display.h"

#include <cctype>

namespace esphome {
namespace vent_axia {

namespace {

/// Renders one byte as "0xXX" (uppercase hex, always two digits) via table
/// lookup rather than <cstdio>'s snprintf or <sstream>'s ostringstream --
/// the core avoids both today and builds strings with std::to_string and
/// concatenation instead (see keypad.cpp's describe_mask for the same house
/// style, applied there to a key mask rather than a raw byte).
std::string to_hex_byte(unsigned char byte) {
  static const char digits[] = "0123456789ABCDEF";
  std::string out = "0x";
  out += digits[(byte >> 4) & 0x0F];
  out += digits[byte & 0x0F];
  return out;
}

}  // namespace

std::string describe_unprintable(const std::string &raw) {
  std::string out;
  for (size_t i = 0; i < raw.size(); i++) {
    const auto byte = static_cast<unsigned char>(raw[i]);
    if (std::isprint(byte) == 0) {
      if (!out.empty()) {
        out += ", ";
      }
      out += "col " + std::to_string(i) + "=" + to_hex_byte(byte);
    }
  }
  return out;
}

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
