#pragma once

// Decodes the MVHR's status loop: which of the seven alternating line1
// messages is currently showing, and what line2's numeric fields (airflow %,
// boost/purge countdown) currently read. Plain C++17, no ESPHome headers --
// see README "Portable core".

#include <cstdint>
#include <optional>
#include <string>

#include "display.h"  // glyphs::ALPHA -- the one measured byte this predicate tests against

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

/// True when the status screen's line2 shows the sensor-boost annunciator at
/// column 15 (the last of 16). The Sentinel Kinetic manual: "If the
/// installation has proportional sensors or an internal humidity sensor
/// fitted, and any of these are boosting the airflow, an alpha symbol will
/// be displayed." The glyph is byte glyphs::ALPHA (0xE0, display.h),
/// measured live 18 Aug 2026 during a real humidity boost (`GET /events`
/// capture: raw line2 non-ASCII byte at col 15 = 0xE0). Reads line2 from the
/// RAW lane (Display::raw_line2()), an exact byte comparison, not a
/// sanitised/transcoded copy.
///
/// Column 15 exactly, not a whole-line scan: measured from a captured
/// screenshot (16 chars at 45.4 px/char; the annunciator glyph sits at
/// x=840-878, the last column) and never observed elsewhere on this unit,
/// so widening the scan would be reasoning ahead of a capture.
///
/// Confirmed on a real humidity boost 17 Aug 2026 (indoor humidity 74%):
/// line1 alternated "Normal Airflow"/"Summer Bypass On" and never showed
/// "Boost Airflow", so on this unit a sensor boost is invisible everywhere
/// except column 15 -- the whole reason this function exists. The airflow
/// rate also modulated (36% -> 34%) while line1 held, consistent with
/// proportional control. (This unit's Low/Normal/Boost rates run roughly
/// 18%/30%/48%.)
///
/// Distinct from the `ls` annunciator seen at columns 14-15 during a
/// switched-live boost while Main was being tapped (`48%           ls`):
/// column 15 there is 's' (0x73), not 0xE0, so the two cannot collide.
///
/// The size guard exists because protocol::LINE_LEN is a fixed 16,
/// space-padded characters on the wire (see protocol.h), but the host
/// tests (and conceivably a not-yet-fully-arrived frame) pass shorter
/// strings.
bool has_sensor_boost_annunciator(const std::string &line2);

/// The numeric content of the status screen's two lines.
struct LineValues {
  std::optional<int> airflow_percent;    // "18%", "48%       30m" -> 18, 48
  std::optional<int> countdown_minutes;  // "48%       30m" -> 30; "Purge      120 m" -> 120
  bool purge{false};                     // the word "Purge" was seen on either line
};

/// Extracts airflow %, a countdown and the Purge keyword from BOTH display
/// lines.
///
/// UNRESOLVED, deliberately coded around: the captured notes this is ported
/// from (mhrv_orig/vent-axia-esphome-project.md, "Boost and Purge") record
/// `Purge      120 m` / `100%` as the purge display but do not establish
/// which physical line carries which half. So this scans both lines for a
/// `NN%`/`NNN%` percentage, a `<digits>[ ]m` countdown and the word "Purge",
/// wherever any land, rather than assuming a layout -- to be settled once
/// purge is exercised on real hardware.
LineValues parse_line_values(const std::string &line1, const std::string &line2);

/// Sticky decode of the status loop's flags -- the hard part, and the reason
/// this is its own class.
///
/// Line1 *alternates* (see classify_line), so at any instant most flags are
/// simply not visible in the current frame. A flag going false is therefore
/// a timeout decision, never "not seen in this frame": the latter would
/// flicker every flag false on every other ~3.2-3.5s frame purely because
/// it wasn't that message's turn.
///
/// The aging clock only advances while the display is on the status screen
/// (the `is_status_screen` argument to update()). If a diagnostics fetch or
/// clock sync has parked the display in a menu for 20s, that time must not
/// count against any flag's timeout -- it would otherwise read as, say, the
/// bypass closing, when the display just looked elsewhere for a while.
///
/// Continuous boost is modelled via continuous_boost(): line1's "Boost
/// Airflow" is the discriminator boosting() already trusts, and
/// continuous_boost() rests on nothing new -- boosting_ staying true for
/// CONTINUOUS_CONFIRM_MS with no countdown ever parsed in the same episode.
/// The hazard is a TIMED boost's own expiry: its countdown vanishes from
/// line2 immediately, but boosting_ (sticky for ALTERNATION_TIMEOUT_MS)
/// does not drop for up to that long afterwards, so a naive "no countdown"
/// check would report continuous on every timed-boost expiry.
/// CONTINUOUS_CONFIRM_MS, required to exceed ALTERNATION_TIMEOUT_MS (see its
/// own comment), is what tells a genuine continuous episode apart from that
/// trailing window. boost_time_remaining() stays unpublished (nullopt)
/// whenever no countdown was parsed -- continuous boost included.
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

  /// Must EXCEED ALTERNATION_TIMEOUT_MS. At a timed boost's expiry, line2's
  /// countdown vanishes immediately, but boosting_ (timeout
  /// ALTERNATION_TIMEOUT_MS) stays sticky-true for up to that long
  /// afterwards -- it only ages out once "Boost Airflow" has genuinely
  /// stopped arriving for a full ALTERNATION_TIMEOUT_MS. Any confirm window
  /// <= ALTERNATION_TIMEOUT_MS would therefore report continuous boost on
  /// every timed-boost expiry, not just a genuine continuous episode.
  /// Measured 13 Aug 2026 on the live unit (192.168.1.200) on a real
  /// switch-driven continuous boost ending: 14.0s from the LAST "Boost
  /// Airflow" frame (21:20:49) to boosting() dropping false (21:21:03) --
  /// against a nominal ALTERNATION_TIMEOUT_MS of 12000ms, 2s over (likely
  /// SSE/timestamping jitter, but not tuned within 1s of an observed edge on
  /// an assumption about its source). The same capture showed line1 and
  /// line2 changing in the SAME frame at the moment boost ended
  /// ("Boost Airflow"/"48%...m" -> "Low Airflow"/"18%") -- confirming the
  /// countdown's disappearance is the right trigger for this accumulator's
  /// clock. 20000ms is the observed 14.0s worst case plus ~6s margin; a
  /// switch overrun episode runs ~10 minutes end to end, so a 20s confirm
  /// window costs about 3% of it.
  static constexpr uint32_t CONTINUOUS_CONFIRM_MS = 20000;

  // Enforced rather than left to prose: a confirm window at or below
  // ALTERNATION_TIMEOUT_MS would report continuous boost on every
  // timed-boost expiry, not just a genuine continuous episode. Do NOT
  // "tidy" these two constants toward each other -- see
  // CONTINUOUS_CONFIRM_MS's own comment for the 14.0s live measurement this
  // headroom is set against.
  static_assert(CONTINUOUS_CONFIRM_MS > ALTERNATION_TIMEOUT_MS,
                "CONTINUOUS_CONFIRM_MS must exceed ALTERNATION_TIMEOUT_MS, or continuous_boost() reports true on "
                "every timed-boost expiry (boosting_ stays sticky-true for up to ALTERNATION_TIMEOUT_MS after the "
                "countdown vanishes from line2, so a confirm window at or below it never distinguishes that "
                "trailing window from a genuine continuous episode)");

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

  /// See humidity_boost_'s own comment (below) for why this rides the same
  /// sticky-Flag machinery as summer_bypass()/boosting()/etc. rather than a
  /// direct per-frame read of has_sensor_boost_annunciator().
  std::optional<bool> humidity_boost() const { return this->get_(this->humidity_boost_); }

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

  /// Continuous boost: boosting_ currently active, not purging_, and no
  /// countdown has been seen at any point in THIS boost episode for longer
  /// than CONTINUOUS_CONFIRM_MS -- see that constant's own comment for why
  /// the threshold must exceed ALTERNATION_TIMEOUT_MS. std::nullopt until
  /// has_status_screen_, same as every other accessor here.
  std::optional<bool> continuous_boost() const;

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
  // Sticky Flag rather than a direct per-frame read of
  // has_sensor_boost_annunciator(): this same zone of line2 was seen to
  // blink -- the `ls` annunciator, transient, appearing only while Main was
  // being tapped -- so a direct read could flap this entity at up to ~3 Hz
  // (the key-repeat rate). 12s of trailing lag is cheap against a state
  // that lasts minutes, and park-freeze on menu/diagnostic screens comes
  // free from the same touch_()/update() machinery every other Flag here
  // uses.
  //
  // Retro-checked against a live boost 17 Aug 2026: the annunciator was
  // STEADY, not blinking, over ~114s of frames. But that capture never
  // tapped a key -- the only condition under which blinking was seen -- so
  // the Flag is KEPT rather than simplified to a direct read on the
  // strength of one keypress-free capture.
  Flag humidity_boost_{ALTERNATION_TIMEOUT_MS};

  std::optional<int> airflow_percent_;
  std::optional<int> countdown_minutes_;

  // Per-episode "how long has boosting_ been active with no countdown ever
  // parsed" clock backing continuous_boost() -- see update()'s own comment
  // for the reset rules and continuous_boost() for how it's read.
  uint32_t ms_without_countdown_{0};
};

/// The airflow mode as derived from the passively decoded status line.
/// Mirrors AirflowTarget (sequence.h) and select.py's AIRFLOW_MODE_OPTIONS
/// index-for-index -- see to_string() below for the mapping that keeps them
/// in lockstep.
enum class AirflowMode : uint8_t { NORMAL, BOOST_30, BOOST_60, BOOST_CONTINUOUS, PURGE };

/// The exact strings select.py's AIRFLOW_MODE_OPTIONS expects -- both lists,
/// and AirflowTarget's enum order, must stay in lockstep, since select.py
/// maps the select's chosen index straight back onto AirflowTarget in
/// write_select(). Do not change any of these literals without updating
/// select.py alongside them.
const char *to_string(AirflowMode mode);

/// Derives airflow_mode's confirmed state purely from what a StatusTracker
/// already decodes (purging()/boosting()/boost_time_remaining()/
/// continuous_boost()) -- no direct knowledge of the wire, only of the
/// tracker's own accessors. One instance per hub, long-lived across the
/// whole run, because the derivation carries one piece of state across
/// frames within a boost episode (see was_boost_60_this_episode_ below).
class AirflowModeTracker {
 public:
  /// nullopt until purging() and boosting() are BOTH known -- i.e. until the
  /// first status-screen frame has ever been decoded. Publishing a guess
  /// before then would be exactly the "unpublished rather than a guess"
  /// violation CLAUDE.md's "Blank != zero" invariant warns about for the
  /// temperature fields, applied here to a derived value instead of a
  /// directly parsed one.
  std::optional<AirflowMode> update(const StatusTracker &status);

 private:
  // "This boost episode was seen above 30 minutes remaining" -- a countdown
  // above 30 is unambiguous proof of a 60-minute boost (a 30-minute one
  // never shows more than 30), so update() latches that the moment it's
  // seen and keeps reporting BOOST_60 for the rest of THIS episode, rather
  // than silently flipping to BOOST_30 once the countdown drops into the
  // 1-30 range every 30-minute boost also passes through. Cleared the
  // moment boosting() goes false -- a fresh episode starts with no evidence
  // yet, same as the very first time.
  bool was_boost_60_this_episode_{false};
};

}  // namespace status
}  // namespace vent_axia
}  // namespace esphome
