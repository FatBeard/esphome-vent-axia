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

std::string to_utf8(const std::string &raw) {
  std::string out;
  out.reserve(raw.size());  // exact for the all-ASCII case, which is the common one
  for (char c : raw) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte >= 0x20 && byte <= 0x7E) {
      out += static_cast<char>(byte);
    } else if (byte == glyphs::ALPHA) {
      out += "\xCE\xB1";  // U+03B1 GREEK SMALL LETTER ALPHA, UTF-8
    } else {
      out += '<';
      out += to_hex_byte(byte).substr(2);  // to_hex_byte() returns "0xXX"; want just "XX"
      out += '>';
    }
  }
  return out;
}

void Display::update(const std::string &raw_line1, const std::string &raw_line2, uint32_t now_ms) {
  have_frame_ = true;

  bool line1_changed = false;
  bool line2_changed = false;

  // Dedup on the RAW text, not a transcoded/sanitised copy: two distinct
  // non-printable bytes in the same column (e.g. a genuine byte change that
  // both happen to render as "<XX>" or, pre-stage-16, both collapsed to the
  // same '*') must each be seen as a change. Deduplicating on any lossy
  // representation reintroduces exactly the glyph-to-glyph blindness
  // DISPLAY-REVIEW.md §5 identifies -- see test_display.cpp's regression
  // test for the case this line exists to fix.
  if (raw_line1 != raw_line1_) {
    raw_line1_ = raw_line1;
    text_line1_ = to_utf8(raw_line1_);  // only a changed line pays the transcode cost
    line1_changed_at_ms_ = now_ms;
    line1_changed = true;
  }
  if (raw_line2 != raw_line2_) {
    raw_line2_ = raw_line2;
    text_line2_ = to_utf8(raw_line2_);
    line2_changed_at_ms_ = now_ms;
    line2_changed = true;
  }

  if ((line1_changed || line2_changed) && on_change_) {
    on_change_(line1_changed, line2_changed);
  }
}

}  // namespace vent_axia
}  // namespace esphome
