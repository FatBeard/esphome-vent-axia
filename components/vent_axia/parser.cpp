#include "parser.h"

#include <cctype>
#include <cstdlib>

#include "screens.h"

namespace esphome {
namespace vent_axia {
namespace parser {

bool parse_field(const std::string &s, size_t pos, size_t len, int &out) {
  if (s.size() < pos + len) {
    return false;
  }
  const std::string field = s.substr(pos, len);
  bool has_digit = false;
  for (char c : field) {
    if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
      has_digit = true;
    } else if (c != ' ' && c != '-' && c != '+') {
      return false;
    }
  }
  if (!has_digit) {
    return false;  // blank (or a lone sign) is not zero -- see header comment
  }
  out = std::atoi(field.c_str());  // no exceptions: atoi, never std::stoi -- see screens.cpp
  return true;
}

std::string trim(const std::string &s) { return screens::trim_trailing(s); }

bool parse_on_off(const std::string &s, bool &out) {
  const std::string v = trim(s);
  if (v == "On" || v == "ON" || v == "on") {
    out = true;
    return true;
  }
  if (v == "Off" || v == "OFF" || v == "off") {
    out = false;
    return true;
  }
  return false;
}

bool clock_rendered(const std::string &s) {
  return s.size() >= 9 && std::isalpha(static_cast<unsigned char>(s[0])) != 0 &&
         std::isalpha(static_cast<unsigned char>(s[1])) != 0 &&
         std::isalpha(static_cast<unsigned char>(s[2])) != 0 && s[3] == ' ' &&
         std::isdigit(static_cast<unsigned char>(s[4])) != 0 &&
         std::isdigit(static_cast<unsigned char>(s[5])) != 0 && s[6] == ':' &&
         std::isdigit(static_cast<unsigned char>(s[7])) != 0 &&
         std::isdigit(static_cast<unsigned char>(s[8])) != 0;
}

int clock_day(const std::string &s) {
  static const char *const kDays[7] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
  if (s.size() < 3) {
    return -1;
  }
  for (int i = 0; i < 7; i++) {
    if (s.compare(0, 3, kDays[i]) == 0) {
      return i;
    }
  }
  return -1;
}

int clock_hour(const std::string &s) { return (s[4] - '0') * 10 + (s[5] - '0'); }

int clock_minute(const std::string &s) { return (s[7] - '0') * 10 + (s[8] - '0'); }

int dow_to_display(int esphome_day_of_week) { return (esphome_day_of_week + 5) % 7; }

int wrapped_delta(int cur, int want, int mod) {
  const int up = ((want - cur) % mod + mod) % mod;
  return up <= mod / 2 ? up : up - mod;
}

}  // namespace parser
}  // namespace vent_axia
}  // namespace esphome
