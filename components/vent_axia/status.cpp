#include "status.h"

#include <cctype>
#include <cstdlib>

#include "screens.h"

namespace esphome {
namespace vent_axia {
namespace status {

namespace {

/// Case-insensitive "does `haystack` contain `needle` anywhere". Unlike
/// screens::starts_with_ci this is a substring search, not an anchored
/// prefix match: the Purge keyword's line is unresolved (see
/// parse_line_values), so it has to be found wherever it lands.
bool contains_ci(const std::string &haystack, const std::string &needle) {
  if (needle.size() > haystack.size()) {
    return false;
  }
  for (size_t i = 0; i + needle.size() <= haystack.size(); i++) {
    bool match = true;
    for (size_t k = 0; k < needle.size(); k++) {
      if (std::tolower(static_cast<unsigned char>(haystack[i + k])) !=
          std::tolower(static_cast<unsigned char>(needle[k]))) {
        match = false;
        break;
      }
    }
    if (match) {
      return true;
    }
  }
  return false;
}

/// First "<digits>%" run found scanning left to right, or nullopt.
std::optional<int> find_percent(const std::string &s) {
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] != '%') {
      continue;
    }
    size_t start = i;
    while (start > 0 && std::isdigit(static_cast<unsigned char>(s[start - 1])) != 0) {
      start--;
    }
    if (start == i) {
      continue;  // a bare '%' with no digit in front of it
    }
    return std::atoi(s.substr(start, i - start).c_str());
  }
  return std::nullopt;
}

/// First "<digits>[ ]m" run found scanning left to right, or nullopt. The
/// optional single space is what lets this match both "30m" and "120 m".
std::optional<int> find_countdown_minutes(const std::string &s) {
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] != 'm') {
      continue;
    }
    size_t digits_end = i;
    if (digits_end > 0 && s[digits_end - 1] == ' ') {
      digits_end--;
    }
    size_t start = digits_end;
    while (start > 0 && std::isdigit(static_cast<unsigned char>(s[start - 1])) != 0) {
      start--;
    }
    if (start == digits_end) {
      continue;  // an 'm' with no digits (allowing one space) in front of it
    }
    return std::atoi(s.substr(start, digits_end - start).c_str());
  }
  return std::nullopt;
}

/// First non-nullopt of f(line1), f(line2) -- the "scan both lines" policy
/// parse_line_values documents.
template<typename Extractor>
std::optional<int> find_in_either(const std::string &line1, const std::string &line2, Extractor f) {
  if (auto v = f(line1)) {
    return v;
  }
  return f(line2);
}

}  // namespace

LineMessage classify_line(const std::string &line1) {
  // "Summer Bypass On" is checked before the shorter messages purely so the
  // list reads top-to-bottom with no significance to the order -- none of
  // these seven share a prefix, so no ordering here is actually load-bearing.
  if (screens::starts_with_ci(line1, "Summer Bypass On")) {
    return LineMessage::SUMMER_BYPASS_ON;
  }
  if (screens::starts_with_ci(line1, "Boost Airflow")) {
    return LineMessage::BOOST_AIRFLOW;
  }
  if (screens::starts_with_ci(line1, "Check Filter")) {
    return LineMessage::CHECK_FILTER;
  }
  if (screens::starts_with_ci(line1, "Dryout Mode")) {
    return LineMessage::DRYOUT_MODE;
  }
  if (screens::starts_with_ci(line1, "Defrost Active")) {
    return LineMessage::DEFROST_ACTIVE;
  }
  if (screens::starts_with_ci(line1, "Low Airflow")) {
    return LineMessage::LOW_AIRFLOW;
  }
  if (screens::starts_with_ci(line1, "Normal Airflow")) {
    return LineMessage::NORMAL_AIRFLOW;
  }
  return LineMessage::NONE;
}

bool has_sensor_boost_annunciator(const std::string &line2) {
  return line2.size() >= 16 && static_cast<unsigned char>(line2[15]) == glyphs::ALPHA;
}

LineValues parse_line_values(const std::string &line1, const std::string &line2) {
  LineValues out;
  out.airflow_percent = find_in_either(line1, line2, find_percent);
  out.countdown_minutes = find_in_either(line1, line2, find_countdown_minutes);
  out.purge = contains_ci(line1, "Purge") || contains_ci(line2, "Purge");
  return out;
}

void StatusTracker::touch_(Flag &flag, bool matched_this_frame, uint32_t delta_ms) {
  if (matched_this_frame) {
    flag.ever_matched = true;
    flag.ms_since_seen = 0;
    flag.active = true;
  } else if (flag.ever_matched) {
    flag.ms_since_seen += delta_ms;
    flag.active = flag.ms_since_seen < flag.timeout_ms;
  }
  // else: never matched, and not matched now -- stays inactive (the default).
}

std::optional<bool> StatusTracker::get_(const Flag &flag) const {
  if (!this->has_status_screen_) {
    return std::nullopt;
  }
  return flag.active;
}

void StatusTracker::update(const std::string &line1, const std::string &line2, bool is_status_screen,
                            uint32_t now_ms) {
  // Advance the "time since update() last ran" clock unconditionally, before
  // the early return below, and independently of is_status_screen. This is
  // what makes a park invisible to the timeouts: last_frame_ms_ keeps
  // tracking real time throughout the park, so the delta computed on the
  // *first* status-screen frame after it resumes is small (time since that
  // last call), not the large one spanning the whole parked interval. If
  // this instead only advanced on status-screen frames, the flags would see
  // one huge delta on return and age out immediately -- exactly the bug
  // is_status_screen exists to prevent.
  const uint32_t delta_ms = this->have_last_frame_ ? (now_ms - this->last_frame_ms_) : 0;
  this->last_frame_ms_ = now_ms;
  this->have_last_frame_ = true;

  if (!is_status_screen) {
    return;  // parked in a menu/diagnostic screen: freeze everything, age nothing
  }
  this->has_status_screen_ = true;

  const LineMessage msg = classify_line(line1);
  touch_(this->summer_bypass_, msg == LineMessage::SUMMER_BYPASS_ON, delta_ms);
  touch_(this->boosting_, msg == LineMessage::BOOST_AIRFLOW, delta_ms);
  touch_(this->defrost_active_, msg == LineMessage::DEFROST_ACTIVE, delta_ms);
  touch_(this->dryout_active_, msg == LineMessage::DRYOUT_MODE, delta_ms);
  touch_(this->filter_change_due_, msg == LineMessage::CHECK_FILTER, delta_ms);

  // Gated on is_status_screen the same as every touch_() call above (all of
  // them unreachable unless is_status_screen was true -- see the early
  // return at the top of this function): menu and diagnostic screens
  // legitimately carry their own custom LCD glyphs (page headers and the
  // like), any of which could in principle land on column 15 too. Only
  // the status loop's line2 is a field where "glyphs::ALPHA at column 15"
  // is known to mean the sensor-boost annunciator and nothing else -- see
  // has_sensor_boost_annunciator()'s own comment.
  touch_(this->humidity_boost_, has_sensor_boost_annunciator(line2), delta_ms);

  const LineValues values = parse_line_values(line1, line2);
  touch_(this->purging_, values.purge, delta_ms);
  this->airflow_percent_ = values.airflow_percent;
  this->countdown_minutes_ = values.countdown_minutes;

  // Per-episode accumulator for continuous_boost() -- see CONTINUOUS_CONFIRM_MS
  // and continuous_boost()'s own comments (status.h). Reset to 0, rather than
  // advanced, whenever ANY of: the episode is over (boosting_ no longer
  // active, read POST-touch_ above -- the same reading the rest of this
  // class already trusts), a countdown WAS parsed this frame (a timed boost,
  // however briefly its countdown has been visible this episode), or purge
  // was seen (a separate axis from the boost counter -- see
  // SetAirflowMode's own class comment in sequence.h -- not to be conflated
  // with continuous boost even though Purge also shows "Boost Airflow"-like
  // alternation). Otherwise it advances by delta_ms, the same "time since
  // update() last ran" interval every Flag in this class ages by, so a menu
  // park freezes this accumulator exactly like it freezes every Flag.
  if (!this->boosting_.active || values.countdown_minutes.has_value() || values.purge) {
    this->ms_without_countdown_ = 0;
  } else {
    this->ms_without_countdown_ += delta_ms;
  }
}

std::optional<bool> StatusTracker::continuous_boost() const {
  if (!this->has_status_screen_) {
    return std::nullopt;
  }
  return this->boosting_.active && !this->purging_.active &&
         this->ms_without_countdown_ >= CONTINUOUS_CONFIRM_MS;
}

const char *to_string(AirflowMode mode) {
  switch (mode) {
    case AirflowMode::NORMAL:
      return "Normal";
    case AirflowMode::BOOST_30:
      return "Boost 30 min";
    case AirflowMode::BOOST_60:
      return "Boost 60 min";
    case AirflowMode::BOOST_CONTINUOUS:
      return "Boost Continuous";
    case AirflowMode::PURGE:
      return "Purge";
  }
  return "Normal";  // unreachable -- every enumerator handled above
}

std::optional<AirflowMode> AirflowModeTracker::update(const StatusTracker &status) {
  // Both must actually be known -- see update()'s own comment (status.h).
  const std::optional<bool> purging = status.purging();
  const std::optional<bool> boosting = status.boosting();
  if (!purging.has_value() || !boosting.has_value()) {
    return std::nullopt;
  }

  // Purge wins outright, and deliberately leaves the latch untouched either
  // way: purge is a separate axis from the boost counter (SetAirflowMode's
  // own class comment, sequence.h), not something that should clear or set
  // evidence about a boost episode that may resume once purge ends.
  if (*purging) {
    return AirflowMode::PURGE;
  }

  if (*boosting) {
    const std::optional<int> remaining = status.boost_time_remaining();
    // See was_boost_60_this_episode_'s own comment (status.h) for what this
    // latches and why.
    if (remaining.has_value() && *remaining > 30) {
      this->was_boost_60_this_episode_ = true;
    }
    const std::optional<bool> continuous = status.continuous_boost();
    if (!remaining.has_value() && continuous.has_value() && *continuous) {
      // Continuous boost, CONFIRMED -- see StatusTracker::continuous_boost()'s
      // own comment for the CONTINUOUS_CONFIRM_MS window this rests on.
      // Deliberately leaves the latch untouched either way: continuous
      // offers no 30-vs-60 evidence of its own, and it does not clear the
      // episode either -- boosting() stays true straight through it (all
      // three of Boost30/Boost60/Continuous show "Boost Airflow" on line1),
      // so whatever the latch already knew from before continuous still
      // applies if a countdown reappears afterwards.
      return AirflowMode::BOOST_CONTINUOUS;
    }
    if (!remaining.has_value()) {
      // Not (yet) confirmed continuous: either CONTINUOUS_CONFIRM_MS hasn't
      // elapsed yet since the countdown last disappeared (which includes the
      // ordinary case of a timed boost whose countdown simply hasn't landed
      // on this particular frame), or this is squarely inside the trailing
      // ALTERNATION_TIMEOUT_MS window right after a timed boost's own
      // expiry -- see CONTINUOUS_CONFIRM_MS's comment for why that window
      // must not be misread as continuous. Reads as Normal until
      // continuous_boost() itself confirms; never guess ahead of it.
      return AirflowMode::NORMAL;
    }
    // Either >30 right now, or latched from earlier this same episode --
    // else never seen above 30 this episode: genuinely 30, or a 60 not yet
    // past its midpoint.
    return this->was_boost_60_this_episode_ ? AirflowMode::BOOST_60 : AirflowMode::BOOST_30;
  }

  // Episode over -- clear the latch. A fresh episode starts with no
  // evidence yet, same as it did the very first time.
  this->was_boost_60_this_episode_ = false;
  return AirflowMode::NORMAL;
}

}  // namespace status
}  // namespace vent_axia
}  // namespace esphome
