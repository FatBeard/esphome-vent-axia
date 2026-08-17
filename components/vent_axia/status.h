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

/// True when the status screen's line2 is showing the sensor-boost
/// annunciator at column 15 (the last of 16). The Sentinel Kinetic manual:
/// "If the installation has proportional sensors or an internal humidity
/// sensor fitted, and any of these are boosting the airflow, an alpha symbol
/// will be displayed." Alpha is not printable ASCII, so sanitize()
/// (display.cpp) maps it to '*' before anything downstream -- including this
/// function -- ever sees the line; the asterisk *is* the alpha.
///
/// Column 15 exactly, not a scan of the whole line, because sanitize()'s
/// mapping is many-to-one: it collapses EVERY non-printable byte to '*', so
/// a whole-line search would also fire on, say, Mode 2's "Auto" glyph
/// (mhrv_orig/vent-axia-esphome-project.md:496) or any other custom
/// character the unit happens to be showing elsewhere on the line. The
/// column itself was measured, not guessed: line1 is a full 16 characters,
/// giving a pitch of 45.4 px/char from a captured screenshot ('S' at x=161
/// to 'n' at x=845 in "Summer Bypass On"), and the asterisk's own glyph run
/// sits at x=840-878 -- column 15, the last column, on a captured frame
/// reading line1 "Summer Bypass On" / line2 "31%            *" (31% being
/// neither this unit's Normal 18% nor Boost 48%, itself corroborating a
/// proportional sensor rate rather than a fixed one).
///
/// This is deliberately NOT the same annunciator PLAN.md §8 stage 10
/// recorded: `ls` at columns 14-15 (`48%           ls`), seen only while
/// Main was being tapped during a switched-live boost. Checking line2[15]
/// alone against '*' cannot match "ls" (column 15 there holds 's', not
/// '*'), so the two are structurally distinct rather than merely
/// coincidentally different in the captures seen so far.
///
/// The size guard exists because protocol::LINE_LEN is a fixed 16,
/// space-padded characters on the wire (protocol.h:19,51), but the host
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
/// Continuous boost IS modelled here, via continuous_boost() -- reopened
/// 13 Aug 2026 against live evidence from 192.168.1.200 (see the plan this
/// shipped under). The original reasoning was that continuous boost shows
/// only a plain airflow percentage on line2, indistinguishable from a high
/// Normal rate -- true, but beside the point: line1's "Boost Airflow" is
/// the discriminator boosting() itself already trusts, and continuous_boost()
/// rests on nothing new -- it is boosting_ staying true for
/// CONTINUOUS_CONFIRM_MS with no countdown ever parsed in the same episode.
/// The one real hazard is a TIMED boost's own expiry: its countdown vanishes
/// from line2 immediately, but boosting_ (sticky for ALTERNATION_TIMEOUT_MS)
/// does not drop for up to that long afterwards, so a naive "no countdown"
/// check would report continuous on every timed-boost expiry.
/// CONTINUOUS_CONFIRM_MS, required to exceed ALTERNATION_TIMEOUT_MS (see its
/// own comment), is what tells a genuine continuous episode apart from that
/// trailing window. boost_time_remaining() stays unpublished (nullopt)
/// whenever no countdown was parsed -- continuous boost included -- and
/// that remains correct: there really is no countdown to report.
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
  /// countdown vanishes immediately, but boosting_ (a Flag whose own timeout
  /// IS ALTERNATION_TIMEOUT_MS) stays sticky-true for up to that long
  /// afterwards -- it only ages out once "Boost Airflow" has genuinely
  /// stopped arriving for a full ALTERNATION_TIMEOUT_MS. Any confirm window
  /// <= ALTERNATION_TIMEOUT_MS would therefore report continuous boost on
  /// every single timed-boost expiry, not just a genuine continuous episode.
  /// Measured 13 Aug 2026 on the live unit (192.168.1.200), on a real
  /// switch-driven continuous boost actually ending: 14.0s elapsed from the
  /// LAST "Boost Airflow" frame (21:20:49) to boosting() finally dropping to
  /// false (21:21:03) -- against a nominal ALTERNATION_TIMEOUT_MS of 12000ms,
  /// a full 2s over. (The extra ~2s is most likely client-side SSE/
  /// timestamping jitter rather than the unit itself, but this is not tuned
  /// to within 1s of an empirically observed edge on the strength of an
  /// assumption about where that jitter came from.) The same capture also
  /// showed line1 and line2 changing in the SAME frame at the moment boost
  /// actually ended ("Boost Airflow"/"48%...m" -> "Low Airflow"/"18%" -- both
  /// lines moved together) -- confirming the countdown's disappearance is
  /// the right trigger for starting this accumulator's clock, not a separate
  /// event that might lag it. 20000ms is the observed 14.0s worst case plus
  /// ~6s of margin, not a round number picked for looks; a genuine switch
  /// overrun episode runs ~10 minutes end to end, so a 20s confirm window
  /// costs about 3% of it.
  static constexpr uint32_t CONTINUOUS_CONFIRM_MS = 20000;

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
  // has_sensor_boost_annunciator(), even though stage 9 measured line2 as
  // NOT alternating for airflow_percent_/countdown_minutes_ (so in
  // principle a direct read would do here too): stage 10 showed this exact
  // right-hand zone of line2 IS capable of blinking -- the `ls` annunciator
  // it recorded appeared only transiently, while Main was being tapped. A
  // direct read of a blinking annunciator would flap this entity at up to
  // ~3 Hz (the key-repeat rate). ALTERNATION_TIMEOUT_MS's 12s trailing lag
  // is cheap against a state that lasts minutes -- the captured frame's 31%
  // is a proportional rate, not a one-frame blip -- and the park-freeze
  // behaviour on menu/diagnostic screens (a diagnostics fetch or clock sync
  // must not be read as the annunciator clearing) comes for free from the
  // same touch_()/update() machinery every other Flag here already uses.
  Flag humidity_boost_{ALTERNATION_TIMEOUT_MS};

  std::optional<int> airflow_percent_;
  std::optional<int> countdown_minutes_;

  // Per-episode "how long has boosting_ been active with no countdown ever
  // parsed" clock backing continuous_boost() -- see update()'s own comment
  // for the reset rules and continuous_boost() for how it's read.
  uint32_t ms_without_countdown_{0};
};

}  // namespace status
}  // namespace vent_axia
}  // namespace esphome
