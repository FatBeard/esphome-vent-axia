#pragma once

// Decodes the MVHR's status loop: which of the seven alternating line1
// messages is currently showing, and what line2's numeric fields (airflow %,
// boost/purge countdown) currently read. Plain C++17, no ESPHome headers --
// see README "Portable core".
//
// This is new capability: the setup this replaces never decoded the status
// line at all, only republished it as raw text (PLAN.md §4).

#include <cstdint>
#include <optional>
#include <string>

namespace esphome {
namespace vent_axia {
namespace status {

/// The line1 messages the status loop is known to alternate between, every
/// ~3.2-3.5s (mhrv_orig/vent-axia-esphome-project.md, "Boost and Purge" and
/// "Summer bypass"). NONE means line1 is on the status loop but is not
/// currently showing one of these -- which is routine, not an error: the
/// loop alternates, so at any instant the *other* half of the cycle is
/// simply a message this frame isn't.
enum class LineMessage {
  NORMAL_AIRFLOW,
  LOW_AIRFLOW,
  BOOST_AIRFLOW,
  CHECK_FILTER,
  SUMMER_BYPASS_ON,
  DRYOUT_MODE,
  DEFROST_ACTIVE,
  NONE,
};

/// Case-insensitive match against the seven known messages
/// (screens::starts_with_ci) -- same reasoning as screens.cpp: some of these
/// names came off the printed manual rather than the wire, and the manual is
/// not consistent about case.
LineMessage classify_line(const std::string &line1);

/// The numeric content of the status screen's two lines.
struct LineValues {
  std::optional<int> airflow_percent;    // "18%", "48%       30m" -> 18, 48
  std::optional<int> countdown_minutes;  // "48%       30m" -> 30; "Purge      120 m" -> 120
  bool purge{false};                     // the word "Purge" was seen on either line
};

/// Extracts airflow %, a countdown and the Purge keyword from BOTH display
/// lines.
///
/// UNRESOLVED, and deliberately coded around: the captured notes this is
/// ported from (mhrv_orig/vent-axia-esphome-project.md, "Boost and Purge")
/// record `Purge      120 m` / `100%` as the purge display but do not
/// establish which physical line carries which half. So this scans both
/// lines for a `NN%`/`NNN%` percentage, a `<digits>[ ]m` countdown and the
/// word "Purge", wherever any of them land, instead of assuming a layout.
/// This will get settled by observation once purge is exercised on real
/// hardware (PLAN.md risk 4); guessing a fixed layout now would be worse
/// than scanning both lines for everything.
LineValues parse_line_values(const std::string &line1, const std::string &line2);

/// Sticky decode of the status loop's flags -- the hard part, and the reason
/// this is its own class.
///
/// Line1 *alternates* (see classify_line), so at any instant most flags are
/// simply not visible in the current frame. A flag going false is therefore
/// a timeout decision, never "not seen in this frame": the latter would
/// make every flag flicker false on every other ~3.2-3.5s frame purely
/// because it wasn't that message's turn.
///
/// The aging clock only advances while the display is actually on the
/// status screen (the `is_status_screen` argument to update()). If a
/// diagnostics fetch or a clock sync has parked the display in a menu for
/// 20s, that time must not be spent against any flag's timeout -- it would
/// otherwise read as, say, the bypass having closed, when all that happened
/// is the display looked elsewhere for a while.
///
/// Continuous boost is deliberately NOT modelled here (PLAN.md §4, risk 2):
/// timed boost shows a countdown and purge shows the "Purge" keyword, but
/// continuous boost shows only a plain airflow percentage indistinguishable
/// from a high normal rate. `boosting()` still goes true off `Boost
/// Airflow` (a fine hint), but there is no way to add "is it continuous"
/// without resting the whole answer on catching one 3.4s-alternating line,
/// so nobody should try. boost_time_remaining() simply stays unpublished
/// (nullopt) whenever no countdown was parsed -- continuous boost included
/// -- and that is the correct state, not a gap. Do not "fix" this later.
class StatusTracker {
 public:
  /// Bypass is the one flag the old config had hard data for
  /// (`bypass_timeout_ms` in summer_bypass.yaml): 45s, kept long because a
  /// scheduled diagnostics fetch or clock sync parks the display in a menu
  /// for up to ~20s and the flag must survive that without being read as
  /// the bypass closing.
  static constexpr uint32_t BYPASS_TIMEOUT_MS = 45000;

  /// Everything else that rides line1's ~3.2-3.5s alternation just needs to
  /// outlive a few cycles. Named (rather than inlined at each call site) so
  /// it reads as obviously tunable if hardware turns out to alternate
  /// slower than observed.
  static constexpr uint32_t ALTERNATION_TIMEOUT_MS = 12000;

  /// Feeds one decoded frame. `is_status_screen` must be false whenever
  /// line1 is a menu/diagnostic screen -- see the class comment -- so a
  /// diagnostics fetch or clock sync parking the display elsewhere freezes
  /// every flag instead of ageing it toward false.
  void update(const std::string &line1, const std::string &line2, bool is_status_screen, uint32_t now_ms);

  /// False until the first status-screen frame has ever been seen. Mirrors
  /// Display::have_frame(): "we don't know yet" must not read as "false",
  /// because a definite false is itself a claim about the unit.
  bool has_state() const { return this->has_status_screen_; }

  std::optional<bool> summer_bypass() const { return this->get_(this->summer_bypass_); }
  std::optional<bool> boosting() const { return this->get_(this->boosting_); }
  std::optional<bool> purging() const { return this->get_(this->purging_); }
  std::optional<bool> defrost_active() const { return this->get_(this->defrost_active_); }
  std::optional<bool> dryout_active() const { return this->get_(this->dryout_active_); }
  std::optional<bool> filter_change_due() const { return this->get_(this->filter_change_due_); }

  /// Refreshed on every status-screen frame; frozen (not cleared) while the
  /// display is elsewhere. Not itself sticky/timeout-based like the line1
  /// flags above: line2's numeric fields do not alternate away the way
  /// line1's messages do, so the latest status-screen reading is always
  /// current.
  std::optional<int> airflow_percent() const {
    return this->has_status_screen_ ? this->airflow_percent_ : std::nullopt;
  }

  /// Published only when a countdown was actually parsed on the status
  /// screen -- see the class comment on continuous boost.
  std::optional<int> boost_time_remaining() const {
    return this->has_status_screen_ ? this->countdown_minutes_ : std::nullopt;
  }

 private:
  struct Flag {
    explicit Flag(uint32_t timeout) : timeout_ms(timeout) {}
    uint32_t timeout_ms;
    bool ever_matched{false};
    uint32_t ms_since_seen{0};
    bool active{false};
  };

  static void touch_(Flag &flag, bool matched_this_frame, uint32_t delta_ms);
  std::optional<bool> get_(const Flag &flag) const;

  bool has_status_screen_{false};
  // "Last time update() ran at all", updated on every call regardless of
  // is_status_screen -- this is what lets the very next status-screen call
  // after a park see a small delta (time since that call), rather than the
  // large one spanning the whole park. See update()'s definition.
  bool have_last_frame_{false};
  uint32_t last_frame_ms_{0};

  Flag summer_bypass_{BYPASS_TIMEOUT_MS};
  Flag boosting_{ALTERNATION_TIMEOUT_MS};
  Flag purging_{ALTERNATION_TIMEOUT_MS};
  Flag defrost_active_{ALTERNATION_TIMEOUT_MS};
  Flag dryout_active_{ALTERNATION_TIMEOUT_MS};
  Flag filter_change_due_{ALTERNATION_TIMEOUT_MS};

  std::optional<int> airflow_percent_;
  std::optional<int> countdown_minutes_;
};

}  // namespace status
}  // namespace vent_axia
}  // namespace esphome
