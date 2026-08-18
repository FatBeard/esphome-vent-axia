#pragma once

// The primitives every concrete sequence is built from, plus the per-field
// parser and direction functions the editing model needs. Plain C++17, no
// ESPHome headers -- see README "Portable core". Bodies live in sequence.cpp
// alongside the engine's own.
//
// OpenEditor, AdjustField and ExitEditChain are the three primitives behind
// the unit's editing model. Set is the only key that is safe once an editor
// is open (walking Up out of one silently took a 14C setpoint to 19C on the
// real unit), so these three are the only things in this component that are
// allowed to open one, adjust a value inside one, or walk one closed.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "protocol.h"
#include "sequence.h"

namespace esphome {
namespace vent_axia {

// --------------------------------------------------------------- primitives --
// Needed by FetchDiagnostics and every sequence that comes after it. A
// single tap-and-wait is Sequence::tap_then_() above, not a class here -- it
// needs no temporary Sequence "with nowhere long-lived to live"
// (seq_sync_clock.cpp's own phrase for exactly this), since it is a helper
// every sequence's poll() calls directly, at whatever step it is already on.

// The two inputs every worst_case_ms()/timeout_ms() derivation below needs
// for "how long does one queued tap actually occupy the keypad" -- both are
// runtime-configurable (Runner::set_tap_duration_ms(), Keypad::
// set_key_gap_ms()) and so cannot themselves be constexpr, but every root
// budget in this file is sized against the DOCUMENTED defaults (50ms tap,
// 400ms key_gap -- keypad.h's key_gap_ms_{400}, Runner's own
// tap_duration_ms_{50}), same as the hand-arithmetic comments these
// constants replace already assumed. If YAML ever raises either default
// (key_gap.h's validate_key_gap floors it at 400ms but does not cap it, and
// raising tap_duration is the documented remedy for a loop()-stall-dropped
// press -- keypad.h's under_emitting_presses()), the REAL worst case grows
// past what the static_asserts below encode; they are only as good as this
// assumption, which is why it is spelled out here rather than left implicit.
constexpr uint32_t kDefaultTapMs = 50;
constexpr uint32_t kDefaultKeyGapMs = 400;

/// Asserts `mask` and holds it until `predicate` is true, or fails at
/// `timeout_ms`. Releases the key in on_finish() on EVERY exit path --
/// success, timeout, or being aborted because something above this in the
/// stack failed -- see Sequence::on_finish()'s contract, which is what makes
/// that unconditional release safe to rely on.
///
/// poll() re-asserts the mask every tick rather than only in on_start():
/// Keypad::press() is a no-op when re-asserting the mask it is already
/// holding (see keypad.h), so this is safe, and it makes the hold
/// self-healing if anything ever released it early without this Sequence's
/// knowledge.
///
/// Default-constructible and reset()-able so one long-lived instance can
/// serve several different holds across a sequence's steps (see
/// FetchDiagnostics, which reuses one for three different holds) rather than
/// needing one HoldUntil member per hold: no dynamic allocation in steady
/// state.
class HoldUntil final : public Sequence {
 public:
  using Predicate = std::function<bool()>;

  HoldUntil() = default;
  HoldUntil(protocol::KeyMask mask, Predicate predicate, uint32_t timeout_ms);

  /// Reconfigures for a fresh hold. Must only be called while this instance
  /// is NOT on the Runner's stack (i.e. before the await() that starts the
  /// new hold) -- reset() does not touch step_/entered_/runner_, those are
  /// set by push_child_() when it is next pushed.
  void reset(protocol::KeyMask mask, Predicate predicate, uint32_t timeout_ms);

  const char *name() const override { return "HoldUntil"; }
  Poll poll() override;
  void on_finish(Poll result) override;

 private:
  protocol::KeyMask mask_{0};
  Predicate predicate_;
  uint32_t timeout_ms_{0};
};

/// Absolute menu positioning by exploiting the hard stop at the top: Up past
/// menu index 0 does nothing, so 5 taps of Up always lands on index 0
/// regardless of where the display started -- this counts taps, it never
/// assumes or tracks the screen's current position, which is why it works
/// correctly right after boot or after some other navigation left the
/// display somewhere unknown. Menu map: 0 status, 1 Set Clock, 2 Summer
/// Mode, 3 Indoor Temp.
class GotoMenu final : public Sequence {
 public:
  GotoMenu() = default;
  explicit GotoMenu(uint8_t index) : index_(index) {}

  /// Reconfigures for a fresh target. Must only be called while this
  /// instance is NOT on the Runner's stack, same rule as HoldUntil::reset()
  /// (see its comment). Lets ReadSettings/WriteSetting reuse ONE long-lived
  /// GotoMenu across several different targets in their own steps (their
  /// own menu entry, then 0 to go home) rather than needing one member per
  /// target -- see "no dynamic allocation in steady state" in this file's
  /// class comment.
  void reset(uint8_t index) { this->index_ = index; }

  const char *name() const override { return "GotoMenu"; }
  Poll poll() override;

  /// Worst case for a run targeting `down_taps` (the menu index, 0-3 on this
  /// unit -- see the class comment's menu map): the fixed 5 Up taps, each
  /// occupying a tap and its key_gap, plus SETTLE_MS; then `down_taps` Down
  /// taps the same way, plus a second SETTLE_MS -- SETTLE_DOWN is unconditional,
  /// even when down_taps is 0 and QUEUE_DOWN/WAIT_DOWN cost nothing (poll()'s
  /// own SETTLE_DOWN case does not skip the wait just because the queue was
  /// already empty). Public so root sequences nesting this (ReadSettings,
  /// WriteSetting, SyncClock) can derive their own timeout_ms() from it.
  static constexpr uint32_t worst_case_ms(uint8_t down_taps) {
    return (5 + down_taps) * (kDefaultTapMs + kDefaultKeyGapMs) + SETTLE_MS + SETTLE_MS;
  }

 private:
  enum Step : uint8_t { QUEUE_UP, WAIT_UP, SETTLE_UP, QUEUE_DOWN, WAIT_DOWN, SETTLE_DOWN };

  // SETTLE_UP/SETTLE_DOWN's own wait, named rather than the bare 500 poll()
  // used before this constant existed -- give the display time to settle on
  // the landed screen before whatever runs next reads it.
  static constexpr uint32_t SETTLE_MS = 500;

  uint8_t index_{0};
};

/// At most ONE Up tap -- never into an open editor -- then, if the display
/// is still showing a menu screen, waits out the unit's own ~2-minute menu
/// timeout rather than pressing again. Mashing Up would corrupt a setting
/// if an editor is still open (observed on the real unit: a second Up
/// silently walked a 14°C setpoint to 19°C). This is deliberate, measured
/// behaviour, not laziness -- do not "helpfully" retry the tap.
///
/// TAP itself checks Display::editor_open() before pressing anything, the
/// same guard Runner::recover() carries (see its own comment). This matters
/// because LeaveMenu can run immediately after a sequence's own commit Set
/// (SyncClock's LEAVE step, following its fourth/COMMIT Set) -- if that
/// commit was dropped, the editor is still open, and Up would adjust the
/// field under the cursor instead of navigating out, the exact
/// 14°C->19°C failure above. So this is at most one Up, and never into an
/// open editor: when the editor is still open, TAP skips straight to
/// WAIT_EXIT and lets the unit's own timeout close it without committing
/// anything.
class LeaveMenu final : public Sequence {
 public:
  const char *name() const override { return "LeaveMenu"; }
  Poll poll() override;

  /// Worst case: one tap-and-its-key_gap (TAP, assuming the editor was
  /// already closed so it is actually taken) plus the full WAIT_EXIT
  /// timeout -- the editor-still-open branch that skips the tap outright is
  /// cheaper, not the worst case. Public so root sequences that nest this
  /// (SyncClock, and via ExitEditChain's own fallback) can derive their own
  /// timeout_ms() from it rather than restating this sum by hand.
  static constexpr uint32_t worst_case_ms() { return kDefaultTapMs + kDefaultKeyGapMs + WAIT_TIMEOUT_MS; }

 private:
  enum Step : uint8_t { TAP, CHECK, WAIT_EXIT };

  // The unit's own menu timeout is ~2 minutes; this is that plus headroom,
  // not a guess.
  static constexpr uint32_t WAIT_TIMEOUT_MS = 130000;
};

// ---------------------------------------------------------- editing model --
// Three primitives behind the unit's editing model: OpenEditor (tap Set,
// confirm it actually opened, retry once), AdjustField (the closed loop
// every field this component writes shares) and ExitEditChain (the
// walk-out, Set only). Ported from the old mhrv_orig/summer_bypass.yaml's
// open_editor/adjust loops/exit_edit_chain scripts -- the comments there
// record real observations on the physical unit, carried into the class
// comments below rather than just the code.

/// Taps Set and confirms an editor actually opened (Display::editor_open()),
/// retrying once before giving up -- the old open_editor script's
/// `repeat: count: 2`. Worth the trouble because a dropped Set is the one
/// failure here with teeth: every primitive that runs after this one
/// (AdjustField in particular) presses Up/Down expecting to adjust a value,
/// and if no editor actually opened those same presses are navigation
/// instead, walking the display back up the menu while the value being
/// "adjusted" never budges -- observed exactly once on the real unit, before
/// gap_ms went up to 400ms.
class OpenEditor final : public Sequence {
 public:
  const char *name() const override { return "OpenEditor"; }
  void on_start() override;
  Poll poll() override;

  /// Worst case: MAX_ATTEMPTS full cycles (a tap-and-its-key_gap, then the
  /// SETTLE wait) before giving up -- the retry that made the old
  /// open_editor script's `repeat: count: 2` worth porting, see the class
  /// comment. Public for the same reason LeaveMenu::worst_case_ms() is.
  static constexpr uint32_t worst_case_ms() {
    return MAX_ATTEMPTS * (kDefaultTapMs + kDefaultKeyGapMs + SETTLE_MS);
  }

 private:
  enum Step : uint8_t { TAP, SETTLE, CHECK };

  // Matches the old open_editor script's `delay: 700ms` -- long enough for
  // the unit to start blinking the value if an editor really opened.
  static constexpr uint32_t SETTLE_MS = 700;
  static constexpr uint8_t MAX_ATTEMPTS = 2;

  uint8_t attempt_{0};
};

/// The closed loop behind every field this component writes, and the
/// clock's day/hour/minute too -- shared rather than copy-pasted per field
/// the way the old YAML's three apply_* scripts were. ValueParser/
/// DirectionFn are what varies per field -- see parse_summer_mode_field(),
/// parse_temp_field() and direction_no_wrap() below, and read_fresh_value()
/// for the helper WriteSetting's VERIFY step and ReadSettings share.
///
/// One iteration, run from poll():
///  1. Bail (FAILED) if the guard is already exhausted -- bounds the loop so
///     a misread, or a value the unit refuses (e.g. outside Outdoor Temp's
///     5-20 C range), cannot become a key-mashing runaway.
///  2. Re-read the target via target_ (TargetFn, see below) -- FAILED,
///     logged, if it is not available right now. For the fixed-target
///     reset() overload this call can never fail; it only matters for the
///     clock fields, where "not available" means no time source at all or a
///     clock that has not synced yet.
///  3. Read and parse line2. A frame that fails to parse is NOT an error and
///     does not count against the guard: an open editor blanks its value on
///     alternate frames, and a blank temperature frame renders "   C", which
///     parse_field correctly rejects -- this just waits for the next frame.
///  4. If the parsed value already equals the (freshly re-read) target, done.
///  5. Otherwise tap Up or Down once -- direction_ decides which.
///  6. Wait -- up to ~900ms -- for line2 to actually change before looping,
///     so a dropped or doubled press self-corrects instead of the next tap
///     firing blind.
class AdjustField final : public Sequence {
 public:
  /// Parses the field currently on line2 into out; false (out left
  /// untouched) for anything that doesn't parse. A stateless function
  /// pointer rather than std::function: every concrete parser this
  /// component has is a free function, and a SettingSpec table of these
  /// (seq_write_setting.cpp) costs nothing at steady state.
  using ValueParser = bool (*)(const std::string &line2, int &out);

  /// True if the field needs to move UP to get from cur towards want, false
  /// for DOWN. Only ever called once cur != want (see step 4 above). A
  /// parameter rather than a hardcoded `cur < want` because it is NOT always
  /// that simple: every settings field is a plain sign comparison (none of
  /// the three wrap, and parser::wrapped_delta would be actively wrong on
  /// all three -- see direction_no_wrap() below), but the
  /// clock fields DO wrap and need a shortest-path version instead
  /// (direction_wrap_24/direction_wrap_60 below), and this is the seam that
  /// lets them reuse this same class.
  using DirectionFn = bool (*)(int cur, int want);

  /// Supplies the field's target, re-read fresh on every CHECK iteration
  /// rather than fixed once at reset() time -- what the clock fields need
  /// and WriteSetting's plain fields do not: the minute field alone can
  /// take up to ~34 taps (each up to ~1.35s -- see guard_limit's callers),
  /// long enough for a real minute (or even hour) rollover to happen
  /// mid-adjustment, and re-reading is how the loop follows it instead of
  /// chasing a target that is already stale by the time it gets there. The
  /// old YAML's own adjust_minute script did exactly this, and said why in
  /// so many words: "The target is re-read from Home Assistant on every
  /// iteration". False means the target is not available right now (no time
  /// source at all, or a clock that has not synced yet) -- see CHECK's
  /// handling of that below.
  ///
  /// A std::function, not a raw function pointer like ValueParser/
  /// DirectionFn: those are stateless free functions known at compile time,
  /// but a live target has to capture something stateful to be live at all
  /// -- here the hub's clock (VentAxiaHub::time_, USE_TIME-guarded, see
  /// seq_sync_clock.cpp) -- and a function pointer cannot carry that.
  /// Assigned at reset()/configure() time, a handful of times per run rather
  /// than every tick, so it does not violate "no dynamic allocation in
  /// steady state"; HoldUntil::Predicate is the same shape for the same
  /// reason.
  using TargetFn = std::function<bool(int &out)>;

  AdjustField() = default;

  /// The fixed-target form every field but the clock's uses (WriteSetting) --
  /// implemented in terms of the live-target overload below via a capturing
  /// lambda that always returns the same value, so there is exactly one
  /// CHECK implementation for both shapes. Must only be called while this
  /// instance is NOT on the Runner's stack, same rule as HoldUntil::reset().
  /// A single long-lived instance serves every field WriteSetting can write
  /// -- no dynamic allocation in steady state.
  void reset(ValueParser parse, DirectionFn direction, int target, int guard_limit);

  /// The live-target form -- see TargetFn's own comment for why SyncClock
  /// needs this and WriteSetting does not. Must only be called
  /// while this instance is NOT on the Runner's stack, same rule as
  /// HoldUntil::reset(). SyncClock reuses ONE long-lived instance across its
  /// day/hour/minute steps, same "no dynamic allocation in steady state"
  /// reasoning as WriteSetting's own use of this class.
  void reset(ValueParser parse, DirectionFn direction, TargetFn target, int guard_limit);

  const char *name() const override { return "AdjustField"; }
  void on_start() override;
  Poll poll() override;

  /// Worst case for a run with `guard_limit` taps available: each iteration
  /// is a tap-and-its-key_gap plus up to the full CHANGE_TIMEOUT_MS wait --
  /// see the class comment's step 6. Public so root sequences configuring
  /// this with a specific field's guard_limit (WriteSetting, SyncClock) can
  /// derive their own timeout_ms() from it.
  static constexpr uint32_t worst_case_ms(int guard_limit) {
    return static_cast<uint32_t>(guard_limit) * (kDefaultTapMs + kDefaultKeyGapMs + CHANGE_TIMEOUT_MS);
  }

 private:
  enum Step : uint8_t { CHECK, WAIT_CHANGE };

  static constexpr uint32_t CHANGE_TIMEOUT_MS = 900;

  ValueParser parse_{nullptr};
  DirectionFn direction_{nullptr};
  TargetFn target_;
  int guard_limit_{0};
  int guard_count_{0};
};

/// The walk-out: leaves any open editor without changing a thing, by
/// committing with Set until the chain falls off its end. Set is the only
/// key that is safe here -- see this file's class comment -- so this NEVER
/// presses Up or Down, unlike every other navigation primitive above.
///
/// Up to 4 times: if Display::editor_open() is true, tap Set and wait 1800ms
/// (deliberately longer than editor_open()'s own ~1200ms default settle
/// window, so the following check means something) before looking again;
/// stops as soon as editor_open() reads false, which is the common case once
/// the chain has actually been walked off its end. If it is STILL open after
/// 4 commits -- more than the documented chain is ever expected to need --
/// this logs it and falls back to waiting out the unit's own ~2-minute
/// timeout (up to 150s), documented to close any editor without
/// committing. Only fails if editor_open() is STILL true once that fallback
/// itself elapses -- same shape as LeaveMenu's own timeout, and for the same
/// reason: a FAILED here cascades straight to the shared Runner::recover()
/// path, which presses Up **at most once**, which is safer than letting a
/// caller's own next step (a plain GotoMenu, say) mash Up five times against
/// a display state this primitive was never able to confirm was safe.
class ExitEditChain final : public Sequence {
 public:
  const char *name() const override { return "ExitEditChain"; }
  void on_start() override;
  Poll poll() override;

  /// Worst case: MAX_COMMITS full CHECK/SETTLE cycles (each a tap-and-its-
  /// key_gap plus the COMMIT_SETTLE_MS wait) before giving up on committing
  /// at all, plus the full WAIT_TIMEOUT fallback -- see the class comment.
  /// Public so root sequences that nest this (ReadSettings, WriteSetting --
  /// once directly and once more inside WriteSetting::read_back_'s own
  /// outdoor hop -- and SyncClock) can derive their own timeout_ms() from it
  /// rather than restating this sum by hand.
  static constexpr uint32_t worst_case_ms() {
    return MAX_COMMITS * (COMMIT_SETTLE_MS + kDefaultTapMs + kDefaultKeyGapMs) + FALLBACK_TIMEOUT_MS;
  }

 private:
  enum Step : uint8_t { CHECK, SETTLE, WAIT_TIMEOUT };

  static constexpr uint8_t MAX_COMMITS = 4;
  static constexpr uint32_t COMMIT_SETTLE_MS = 1800;
  // The old YAML's own number (mhrv_orig/summer_bypass.yaml's
  // exit_edit_chain): comfortably above the unit's own ~2-minute timeout.
  static constexpr uint32_t FALLBACK_TIMEOUT_MS = 150000;

  uint8_t commits_{0};
};

// ------------------------------------------------------- settings fields --
// Shared by ReadSettings and WriteSetting (seq_read_settings.cpp,
// seq_write_setting.cpp): the three bypass fields' value encoding, shaped to
// match AdjustField::ValueParser/DirectionFn so the exact same functions
// serve both a plain read and a WriteSetting SettingSpec table row: one
// class and three table rows, not three near-identical copies.

/// Summer Mode as 0/1 -- AdjustField and the read path both want an int to
/// compare/step, not a bool. See parser::parse_on_off for the actual parse
/// (blank/blinking frames correctly fail, not read as Off).
bool parse_summer_mode_field(const std::string &line2, int &out);

/// Indoor Temp and Outdoor Temp share one 2-digit field at [0,2) --
/// mhrv_orig/summer_bypass.yaml used this exact position for both. See
/// parser::parse_field for blank-vs-zero handling: a blank temperature frame
/// renders "   C", which this correctly rejects rather than reading as 0.
bool parse_temp_field(const std::string &line2, int &out);

/// Plain sign comparison -- correct for Summer Mode, Indoor Temp and Outdoor
/// Temp, none of which wrap. parser::wrapped_delta would be actively wrong
/// on all three; it is for the clock's hour/minute only.
bool direction_no_wrap(int cur, int want);

/// True once line2 has published something NEWER than `since_ms` (the moment
/// navigation to the CURRENT screen started) that also parses. A change
/// strictly after
/// `since_ms` is what proves a reading belongs to the screen just arrived
/// at, rather than being a stale value left over from wherever the display
/// was before. Returns nullopt while still waiting; callers apply their own
/// timeout. Shared because ReadSettings' plain screens and WriteSetting's
/// VERIFY step both need exactly this.
std::optional<int> read_fresh_value(const Display &display, uint32_t since_ms, AdjustField::ValueParser parse);

// ------------------------------------------------------------ clock fields --
// SyncClock's (seq_sync_clock.cpp) Set Clock screen fields, shaped to match
// AdjustField::ValueParser/DirectionFn exactly like
// the settings fields above -- one AdjustField, three table rows in spirit,
// even though the clock's "table" is just three named calls rather than an
// array (there being only ever three rows, unlike SettingSpec's three that
// are expected to grow).

/// Set Clock's day field. Rejects anything parser::clock_rendered() would --
/// a mid-blink frame, e.g. "    23:49" with the day blanked -- rather than
/// partially decoding it; see parser::clock_day's own comment for why that
/// assumption is safe once clock_rendered() has passed.
bool parse_clock_day_field(const std::string &line2, int &out);
bool parse_clock_hour_field(const std::string &line2, int &out);
bool parse_clock_minute_field(const std::string &line2, int &out);

/// Shortest-path direction for a field that wraps at 24 (the hour field) --
/// parser::wrapped_delta > 0 means "up is the fewer signed presses", not
/// merely "up is numerically larger" the way direction_no_wrap reads. This
/// is for hour and minute (direction_wrap_60 below) ONLY -- the day field
/// uses direction_no_wrap, same as every other non-wrapping field on this
/// unit, because it does NOT wrap: Up on Sun does nothing at all (observed
/// on the real unit, mhrv_orig/controls.yaml's own clock-sync comment). That
/// is the one place the three clock fields genuinely differ from each other,
/// so it is worth being explicit here rather than letting SyncClock's table
/// of three calls be the only place it shows up.
bool direction_wrap_24(int cur, int want);

/// Same, at modulus 60, for the minute field.
bool direction_wrap_60(int cur, int want);

}  // namespace vent_axia
}  // namespace esphome
