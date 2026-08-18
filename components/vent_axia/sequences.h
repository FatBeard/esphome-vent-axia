#pragma once

// The six concrete sequences: FetchDiagnostics, ReadSettings, WriteSetting,
// SyncClock, SetAirflowMode and ResetFilter. Declared together in one
// growing file rather than one header apiece -- the same choice entities.h
// makes -- with each one's method bodies in its own seq_*.cpp. Plain C++17,
// no ESPHome headers -- see README "Portable core".
//
// Read sequence.h first for the engine these run on, then
// sequence_primitives.h for the pieces they are built from.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "entities.h"
#include "sequence_primitives.h"
#include "status.h"

namespace esphome {
namespace vent_axia {

/// Up+Main to enter the diagnostic menu, hold Down 8s to auto-scroll
/// through every page, then the verified two-stage exit (release, settle, a
/// FRESH hold of Up -- see seq_fetch_diagnostics.cpp's poll() for why
/// holding straight through never works). Decodes nothing itself: the hub's
/// existing on_change callback publishes every page passed through as it
/// goes by. Tracks the highest page number actually seen
/// rather than waiting for a hardcoded terminator -- firmware V32/05 stops
/// at page 27, and the old component's wait for "Diagnostic  28" hung for
/// 60s and abandoned the display in the menu.
///
/// A long-lived hub member, reused on every scheduled or button-triggered
/// run -- see on_start() for what gets reset between runs.
class FetchDiagnostics final : public Sequence {
 public:
  /// Called once, only after a fully successful run -- the hub uses this to
  /// stamp diagnostics_updated with the current wall-clock time. The
  /// indirection exists because portable core has no notion of wall-clock
  /// time (see README "Portable core"): this file never touches
  /// time::RealTimeClock, the hub does that on the other side of the sink.
  using SuccessSink = std::function<void()>;

  void set_on_success(SuccessSink sink) { this->on_success_ = std::move(sink); }

  const char *name() const override { return "FetchDiagnostics"; }
  void on_start() override;
  Poll poll() override;
  void on_finish(Poll result) override;

  /// Sum of every step's own timeout -- this class has no timeout_ms()
  /// override of its own (Sequence::timeout_ms()'s 180s default only ever
  /// applies when this runs as the ROOT, which nothing in this component
  /// does; see ResetFilter's own comment for "only ever consulted for
  /// whichever Sequence is currently the ROOT"). Public so ResetFilter,
  /// which pushes this as a CHILD and so is bounded by ITS OWN timeout_ms()
  /// instead, can fold this worst case into that derivation.
  static constexpr uint32_t worst_case_ms() {
    return ENTER_TIMEOUT_MS + ENTER_SETTLE_MS + HOLD_DOWN_MS + DOWN_SETTLE_MS + TO_PAGE_00_TIMEOUT_MS +
           PAGE_00_SETTLE_MS + EXIT_TIMEOUT_MS;
  }

 private:
  bool at_page_00_() const;
  void track_page_();
  int count_seen_pages_() const;

  enum Step : uint8_t { ENTER, RELEASE_ENTER, HOLD_DOWN, RELEASE_DOWN, RELEASE_AT_00, EXIT, FINISHED };

  static constexpr uint32_t ENTER_TIMEOUT_MS = 15000;
  static constexpr uint32_t ENTER_SETTLE_MS = 300;
  static constexpr uint32_t HOLD_DOWN_MS = 8000;
  // Matches key_gap: a release is only silence on this protocol, and this is
  // how much of it the unit needs to see one. See RELEASE_DOWN in
  // seq_fetch_diagnostics.cpp for why a hold-to-hold transition needs this
  // spelled out where a tap-to-tap one does not.
  static constexpr uint32_t DOWN_SETTLE_MS = 400;
  static constexpr uint32_t TO_PAGE_00_TIMEOUT_MS = 20000;
  static constexpr uint32_t PAGE_00_SETTLE_MS = 250;
  static constexpr uint32_t EXIT_TIMEOUT_MS = 15000;
  // Page numbers are two ASCII digits (screens::diagnostic_page()), so 0..99
  // covers every representable page with room to spare.
  static constexpr size_t MAX_PAGE_INDEX = 100;

  HoldUntil hold_;  // reused across steps 1, 5 and 7 -- see HoldUntil's class comment
  SuccessSink on_success_;
  int highest_page_seen_{-1};
  std::array<bool, MAX_PAGE_INDEX> seen_pages_{};
};

// -------------------------------------------------------------- settings --
// ReadSettings and WriteSetting both drive the same three screens (Summer
// Mode, Indoor Temp, Outdoor Temp) via the editing-model primitives just
// above.

/// One pass reading Summer Mode, Indoor Temp, then Outdoor Temp via the
/// edit-chain hop -- see this file's class comment for the primitives it is
/// built from. Publishes whatever it manages to read via
/// on_switch()/on_number(); a value that doesn't parse (blank/blinking, or a
/// screen that was never reached) is logged and simply left unpublished,
/// same "never hard-fail a read" philosophy as the rest of this component --
/// one unreadable value is not a reason to withhold the other two.
///
/// Reading Outdoor Temp requires opening Indoor Temp's editor and stepping
/// past it, so a read can leave the display mid-edit whether or not it
/// landed where it meant to: the chain's shape is not guaranteed -- a commit
/// has been observed closing the editor outright rather than advancing. Both
/// the "landed on Outdoor Temp" and "did not" branches of that hop fall
/// through to the SAME EXIT_CHAIN step below -- one funnel every path passes
/// through structurally, not a per-branch call whose indentation had to be
/// right in both an `if` and an `else`
/// (mhrv_orig/summer_bypass.yaml's read_summer_settings).
///
/// Finishes by returning the display to the status screen (GotoMenu(0)).
/// A long-lived hub member, reused on every button press -- see on_start()
/// for what resets between runs.
class ReadSettings final : public Sequence {
 public:
  using SwitchSink = std::function<void(SwitchKey, bool)>;
  using NumberSink = std::function<void(NumberKey, int)>;

  void set_on_switch(SwitchSink sink) { this->on_switch_ = std::move(sink); }
  void set_on_number(NumberSink sink) { this->on_number_ = std::move(sink); }

  const char *name() const override { return "ReadSettings"; }
  void on_start() override;
  Poll poll() override;
  void on_finish(Poll result) override;

  // The outdoor hop nests ExitEditChain's own up-to-~159s fallback wait --
  // see Sequence::timeout_ms()'s comment on why anything nesting a wait like
  // that needs a root budget with real headroom above it. TIMEOUT_BUDGET_MS
  // is checked against WORST_CASE_MS below (static_assert), not trusted by
  // eye.
  uint32_t timeout_ms() const override { return TIMEOUT_BUDGET_MS; }

  /// WriteSetting nests a whole ReadSettings run as its own READ_BACK step
  /// (confirming what actually landed) -- public so WriteSetting's own
  /// worst-case derivation can fold this one in rather than restating it.
  static constexpr uint32_t worst_case_ms() { return WORST_CASE_MS; }

 private:
  enum Step : uint8_t {
    NAV_SUMMER,
    WAIT_SUMMER,
    NAV_INDOOR,
    WAIT_INDOOR,
    OPEN_OUTDOOR,
    HOP_COMMIT,
    WAIT_OUTDOOR_SCREEN,
    WAIT_OUTDOOR_VALUE,
    EXIT_CHAIN,
    HOME,
    FINISHED,
  };

  // mhrv_orig/summer_bypass.yaml's own wait_until timeouts for these same
  // four reads.
  static constexpr uint32_t SUMMER_TIMEOUT_MS = 2000;
  static constexpr uint32_t INDOOR_TIMEOUT_MS = 3000;
  static constexpr uint32_t OUTDOOR_SCREEN_TIMEOUT_MS = 3000;
  static constexpr uint32_t OUTDOOR_VALUE_TIMEOUT_MS = 2000;

  // Worst case is dominated by EXIT_CHAIN's ExitEditChain fallback (~159s).
  // One term per step in poll() (seq_read_settings.cpp), expressed as a
  // compiler-checked sum rather than a hand-summed comment, so it cannot go
  // stale the way a prose estimate could.
  static constexpr uint32_t WORST_CASE_MS = GotoMenu::worst_case_ms(2) + SUMMER_TIMEOUT_MS +
                                             GotoMenu::worst_case_ms(3) + INDOOR_TIMEOUT_MS +
                                             OpenEditor::worst_case_ms() + (kDefaultTapMs + kDefaultKeyGapMs) +
                                             OUTDOOR_SCREEN_TIMEOUT_MS + OUTDOOR_VALUE_TIMEOUT_MS +
                                             ExitEditChain::worst_case_ms() + GotoMenu::worst_case_ms(0);

  static constexpr uint32_t TIMEOUT_BUDGET_MS = 240000;
  static_assert(TIMEOUT_BUDGET_MS > WORST_CASE_MS,
                "ReadSettings' root budget must exceed its own worst-case sum, or a legitimate run (e.g. the "
                "outdoor hop's ExitEditChain genuinely waiting out its own fallback) can be killed mid-wait by the "
                "root timeout instead of being allowed to finish on its own");

  GotoMenu goto_menu_;      // reused for NAV_SUMMER, NAV_INDOOR and HOME -- reset() before each
  OpenEditor open_editor_;  // the outdoor hop's opening move
  ExitEditChain exit_chain_;

  uint32_t nav_started_ms_{0};
  // Gates the outdoor hop: opening an editor on a screen that could not be
  // identified is exactly the situation WriteSetting aborts on, so a read
  // that never saw a clean Indoor Temp value does not attempt it either --
  // mhrv_orig/summer_bypass.yaml's own guard on enter_outdoor_editor.
  bool indoor_read_ok_{false};

  SwitchSink on_switch_;
  NumberSink on_number_;
};

/// Which of the three settings a WriteSetting instance targets -- selects a
/// row of the SettingSpec table in seq_write_setting.cpp. Public because the
/// hub names one when configuring the shared, long-lived WriteSetting
/// instance before request()ing it.
enum class SettingId : uint8_t { SUMMER_MODE, INDOOR_TEMP, OUTDOOR_TEMP };

// Kept out of this header (defined in seq_write_setting.cpp) -- WriteSetting
// only ever needs a pointer to one, never a complete type.
struct SettingSpec;

/// Maps a writable entity key onto the setting WriteSetting knows how to
/// write, or nullopt for a key with no mapping.
///
/// These live in the portable core, not in the hub's write_switch()/
/// write_number(), for one specific reason: this side of the line is
/// compiled by the host test suite, and tests/CMakeLists.txt builds it with
/// -Werror. A switch on the enum TYPE there turns a forgotten mapping into
/// a hard compile error, which is the enforcement the enum-keyed design
/// wants ("adding an entity means an enum member, a dict entry and a
/// publish call").
///
/// The firmware build does NOT use -Werror -- an unhandled enumerator there
/// is only a -Wswitch warning in a long build log, which is why keeping
/// these switches in vent_axia.cpp would not have bought the guarantee it
/// looks like it buys.
std::optional<SettingId> setting_for(SwitchKey key);
std::optional<SettingId> setting_for(NumberKey key);


/// Writes one of the three bypass settings, then reads all three back as
/// confirmation. configure() selects the row (SettingId) and target value
/// before this instance is request()ed; the
/// STEPS below are the same for all three -- see seq_write_setting.cpp's
/// SettingSpec table for what actually differs between them. This is the
/// first sequence in this component that presses Set: see this file's header
/// comment for why every step past OPEN below only ever does so through
/// OpenEditor/AdjustField/ExitEditChain.
///
/// Outdoor Temp is reached through Indoor Temp's editor rather than by
/// direct navigation, and that hop can leave an editor open whether or not
/// it landed on the right screen -- so, exactly like ReadSettings, EXIT_CHAIN
/// is a single funnel every path (hop landed or not) falls through to, never
/// a per-branch call. ok_ carries whether the write actually happened
/// through that funnel to FINISHED, where it decides DONE vs FAILED.
///
/// A long-lived hub member, reused for every write -- configure() before
/// each request(), see on_start() for what else resets.
class WriteSetting final : public Sequence {
 public:
  void set_on_switch(ReadSettings::SwitchSink sink) { this->read_back_.set_on_switch(std::move(sink)); }
  void set_on_number(ReadSettings::NumberSink sink) { this->read_back_.set_on_number(std::move(sink)); }

  /// Selects which row to write and the value to write it to -- call before
  /// request()ing this instance. target uses the same 0/1 encoding
  /// parse_summer_mode_field() does for Summer Mode, plain whole degrees C
  /// for the two temperatures.
  void configure(SettingId id, int target);

  const char *name() const override { return "WriteSetting"; }
  void on_start() override;
  Poll poll() override;
  void on_finish(Poll result) override;

  // Nests ExitEditChain's own fallback wait potentially TWICE -- once
  // directly (EXIT_CHAIN below) and once more inside read_back_'s own
  // outdoor hop (READ_BACK) -- on top of AdjustField's own worst case and
  // the rest of the navigation/verify/settle steps. TIMEOUT_BUDGET_MS is
  // checked against WORST_CASE_MS below (static_assert): a root budget that
  // expires WHILE ExitEditChain is patiently waiting out a genuinely
  // still-open editor would abandon the run at the worst possible moment,
  // same reasoning as Sequence::timeout_ms()'s own comment on LeaveMenu's
  // 130s wait.
  uint32_t timeout_ms() const override { return TIMEOUT_BUDGET_MS; }

 private:
  enum Step : uint8_t {
    NAVIGATE,
    VERIFY,
    OPEN,
    HOP_COMMIT,
    WAIT_HOP_SCREEN,
    ADJUST,
    COMMIT,
    SETTLE,
    EXIT_CHAIN,
    HOME,
    READ_BACK,
    FINISHED,
  };

  static constexpr uint32_t VERIFY_TIMEOUT_MS = 3000;
  static constexpr uint32_t HOP_SCREEN_TIMEOUT_MS = 3000;  // matches ReadSettings' own outdoor hop
  // Must exceed Display::editor_open()'s own 1200ms staleness window: a
  // SUCCESSFUL commit repaints line2 too, so a shorter settle cannot tell a
  // closed editor from an open one.
  static constexpr uint32_t SETTLE_MS = 1800;

  // The worst-case menu_index and guard_limit across every row this shared,
  // reused instance might be configure()d for (seq_write_setting.cpp's
  // SettingSpec table): Indoor/Outdoor Temp's menu_index 3 (vs. Summer
  // Mode's 2) and guard_limit 40 (vs. Summer Mode's 3) are both the binding
  // case, and Outdoor Temp alone exercises HOP_COMMIT/WAIT_HOP_SCREEN -- so
  // the derivation below assumes all three at once, which no single actual
  // run does, making it a genuine worst case rather than an average one.
  static constexpr uint8_t MAX_MENU_INDEX = 3;
  static constexpr int MAX_GUARD_LIMIT = 40;

  // One term per step in poll() (seq_write_setting.cpp); READ_BACK's nested
  // ReadSettings run carries its own ExitEditChain fallback a SECOND time
  // (see this override's own comment). A compiler-checked sum rather than a
  // hand-summed comment, so it cannot go stale the way a prose estimate
  // could.
  static constexpr uint32_t WORST_CASE_MS =
      GotoMenu::worst_case_ms(MAX_MENU_INDEX) + VERIFY_TIMEOUT_MS + OpenEditor::worst_case_ms() +
      (kDefaultTapMs + kDefaultKeyGapMs) + HOP_SCREEN_TIMEOUT_MS + AdjustField::worst_case_ms(MAX_GUARD_LIMIT) +
      (kDefaultTapMs + kDefaultKeyGapMs) + SETTLE_MS + ExitEditChain::worst_case_ms() + GotoMenu::worst_case_ms(0) +
      ReadSettings::worst_case_ms();

  static constexpr uint32_t TIMEOUT_BUDGET_MS = 480000;
  static_assert(TIMEOUT_BUDGET_MS > WORST_CASE_MS,
                "WriteSetting's root budget must exceed its own worst-case sum, or a legitimate run (ExitEditChain "
                "genuinely waiting out its own fallback, potentially twice -- once directly, once inside the "
                "READ_BACK confirmation pass) can be killed mid-wait by the root timeout instead of being allowed "
                "to finish on its own");

  SettingId id_{SettingId::SUMMER_MODE};
  const SettingSpec *spec_{nullptr};
  int target_{0};
  uint32_t nav_started_ms_{0};  // for VERIFY's read_fresh_value() -- see read_fresh_value()'s own comment
  // False only if the Outdoor Temp hop does not land -- see class comment.
  // Every other failure in this sequence (VERIFY, OPEN, AdjustField's guard,
  // ExitEditChain itself) returns Poll::FAILED directly instead: at those
  // points nothing has been committed and/or the failure already cascades
  // correctly, so there is nothing this flag needs to remember for them.
  bool ok_{true};

  GotoMenu goto_menu_;        // reused for NAVIGATE and HOME -- reset() before each
  OpenEditor open_editor_;
  AdjustField adjust_field_;
  ExitEditChain exit_chain_;
  ReadSettings read_back_;
};

// ---------------------------------------------------------------- SyncClock --
// The Set Clock screen. Ported from mhrv_orig/controls.yaml's sync_clock/
// sync_clock_run/adjust_day/adjust_hour/adjust_minute scripts -- see this
// class's own comment and seq_sync_clock.cpp for where each observation on
// the physical unit landed.

/// Corrects the unit's own clock against wall-clock time, one field at a
/// time through the Set Clock editor. How the editor behaves, measured on
/// the real unit (mhrv_orig/controls.yaml): Set enters on the day field;
/// each further Set accepts the current field and advances to the next; a
/// FOURTH Set commits and drops out. The field being edited blinks (blanks
/// on alternate frames -- Display::editor_open()'s signal), which is both
/// how the unit shows which field is active and why parse_clock_*_field()
/// above only trusts a fully rendered frame. Day does not wrap (Up on Sun
/// does nothing); hour and minute do, and take the shortest path
/// (direction_wrap_24/60 above). Main is never pressed anywhere here: on
/// this unit Main is Boost.
///
/// If anything is not where it is expected -- wrong screen, unreadable
/// clock, no time source -- this bails out and leaves the display alone
/// (VERIFY/CHECK_TIME below); the unit returns to its normal screen on its
/// own two-minute timeout, as with every sequence that fails before
/// committing.
///
/// The fourth (COMMIT) Set is not itself retried or verified the way
/// OpenEditor verifies the first: SETTLE and EXIT_CHAIN stand in for that.
/// If COMMIT's Set was dropped, the editor is still open once SETTLE has
/// waited long enough to tell (SETTLE_MS is deliberately longer than
/// editor_open()'s own window), and EXIT_CHAIN walks it closed with more
/// Sets before LEAVE ever taps Up. LeaveMenu's TAP step also refuses to tap
/// into an open editor as a structural backstop, but EXIT_CHAIN is what
/// actually recovers the run.
///
/// A long-lived hub member, reused on every scheduled or button-triggered
/// run: no dynamic allocation in steady state. There is no per-run state to
/// reset in on_start() -- nav_started_ms_ is written fresh at NAVIGATE
/// (always step 0), and every AdjustField child resets its own guard_count_
/// in ITS on_start() each time await() pushes it.
class SyncClock final : public Sequence {
 public:
  /// Wall-clock time to sync the unit's own clock to, injected the same way
  /// FetchDiagnostics::SuccessSink lets the hub touch wall-clock time from
  /// the other side of the portable-core boundary (README "Portable core"):
  /// this file never includes esphome/components/time/real_time_clock.h.
  /// Mon=0..Sun=6 (parser::dow_to_display's convention), hour 0-23, minute
  /// 0-59. False when there is no time source at all (no `time_id`
  /// configured, or USE_TIME undefined -- see vent_axia.h ~line 303) or the
  /// clock has not synced yet (ESPTime::is_valid() false) -- a sync against
  /// an unsynced clock would write a WRONG time to the unit, which is worse
  /// than not syncing at all, so both count as "unavailable" the same way.
  using TimeSource = std::function<bool(int &dow_display, int &hour, int &minute)>;

  void set_time_source(TimeSource source) { this->time_source_ = std::move(source); }

  const char *name() const override { return "SyncClock"; }
  Poll poll() override;
  void on_finish(Poll result) override;

  // Worst case: the three guard limits (DAY_GUARD/HOUR_GUARD/MINUTE_GUARD --
  // the old script's own numbers) each drive an AdjustField that can run up
  // to its own worst case, plus NAVIGATE's GotoMenu(1), VERIFY's budget,
  // OPEN's own worst case, three plain Set taps advancing/committing the
  // editor (SET_DAY/SET_HOUR/COMMIT), SETTLE's wait, EXIT_CHAIN's own worst
  // case if the commit Set was dropped and the fourth Set never actually
  // landed, and LEAVE's own worst case if the editor still somehow has not
  // closed by then. TIMEOUT_BUDGET_MS is checked against WORST_CASE_MS below
  // (static_assert) rather than trusted by eye -- same "comfortable, not
  // cutting it close" reasoning as WriteSetting's own timeout_ms().
  uint32_t timeout_ms() const override { return TIMEOUT_BUDGET_MS; }

 private:
  enum Step : uint8_t {
    NAVIGATE,
    VERIFY,
    CHECK_TIME,
    OPEN,
    ADJUST_DAY,
    SET_DAY,
    ADJUST_HOUR,
    SET_HOUR,
    ADJUST_MINUTE,
    COMMIT,
    SETTLE,
    EXIT_CHAIN,
    LEAVE,
    FINISHED,
  };

  static constexpr uint32_t VERIFY_TIMEOUT_MS = 3000;  // matches WriteSetting::VERIFY's own budget
  // NOT the old script's 700ms: that delay only ever gated a log line, but
  // this one also gates EXIT_CHAIN's decision of whether the commit Set
  // actually landed (Display::editor_open() below) -- so, unlike the old
  // delay, it has to outlast editor_open()'s own ~1200ms default settle
  // window for that check to mean anything, the same reason
  // ExitEditChain::COMMIT_SETTLE_MS is 1800ms rather than reusing
  // OpenEditor::SETTLE_MS's 700ms (see that constant's own comment).
  // Settling on the SAME 1800ms here rather than inventing a third number.
  static constexpr uint32_t SETTLE_MS = 1800;

  // The old script's own guard numbers (mhrv_orig/controls.yaml's
  // adjust_day/adjust_hour/adjust_minute comments): the day field needs at
  // most 6 presses, the hour 12, the minute 30 -- each with headroom, same
  // "bound the loop, don't trust luck" reasoning as every other guard_limit
  // in this component.
  static constexpr int DAY_GUARD = 8;
  static constexpr int HOUR_GUARD = 14;
  static constexpr int MINUTE_GUARD = 34;

  // One term per step in poll() (seq_sync_clock.cpp) -- CHECK_TIME adds
  // nothing, since it never waits, only fails fast or falls through. A
  // compiler-checked sum rather than a hand-summed comment, so it cannot go
  // stale the way a prose estimate could.
  static constexpr uint32_t WORST_CASE_MS =
      GotoMenu::worst_case_ms(1) + VERIFY_TIMEOUT_MS + OpenEditor::worst_case_ms() +
      AdjustField::worst_case_ms(DAY_GUARD) + (kDefaultTapMs + kDefaultKeyGapMs) +
      AdjustField::worst_case_ms(HOUR_GUARD) + (kDefaultTapMs + kDefaultKeyGapMs) +
      AdjustField::worst_case_ms(MINUTE_GUARD) + (kDefaultTapMs + kDefaultKeyGapMs) + SETTLE_MS +
      ExitEditChain::worst_case_ms() + LeaveMenu::worst_case_ms();

  static constexpr uint32_t TIMEOUT_BUDGET_MS = 450000;
  static_assert(TIMEOUT_BUDGET_MS > WORST_CASE_MS,
                "SyncClock's root budget must exceed its own worst-case sum, or a legitimate run (ExitEditChain "
                "genuinely waiting out a dropped commit Set, then LeaveMenu waiting out the unit's own menu "
                "timeout on top of that) can be killed mid-wait by the root timeout instead of being allowed to "
                "finish on its own");

  bool day_target_(int &out) const;
  bool hour_target_(int &out) const;
  bool minute_target_(int &out) const;
  /// Shared by CHECK_TIME's initial log and SETTLE's before/after one --
  /// "Ddd HH:MM" from a TimeSource reading, or "unknown" if none is
  /// available right now (SETTLE's own read is best-effort logging only, not
  /// a condition anything branches on -- the sync already happened).
  std::string describe_target_() const;

  GotoMenu goto_menu_;      // NAVIGATE only, but reset()-able the same as every other sequence's copy
  OpenEditor open_editor_;
  AdjustField adjust_field_;  // reused for ADJUST_DAY, ADJUST_HOUR and ADJUST_MINUTE -- reset() before each
  ExitEditChain exit_chain_;  // EXIT_CHAIN only -- runs only if the commit Set was dropped, see class comment
  LeaveMenu leave_menu_;

  uint32_t nav_started_ms_{0};
  TimeSource time_source_;
};

// ------------------------------------------------------------ SetAirflowMode --
// Ported from mhrv_orig/controls.yaml's boost_probe/boost_normalise/
// boost_set, extended for Purge -- see SetAirflowMode's own class comment
// (seq_set_airflow_mode.cpp) for the full step-by-step reasoning.

/// The five targets `airflow_mode` (select.py) can be set to -- deliberately
/// ordered to match select.py's AIRFLOW_MODE_OPTIONS list index-for-index,
/// so VentAxiaSelect::control(size_t index) (vent_axia.h) can hand the raw
/// index straight to SetAirflowMode::configure() with no separate lookup
/// table. THE ORDER IS LOAD-BEARING: vent_axia.cpp's write_select() casts
/// the select's raw option index straight to this enum, so
/// BOOST_CONTINUOUS must sit between BOOST_60 and PURGE in BOTH this enum
/// and select.py's list, or the two silently drift out of sync. Reopened
/// 13 Aug 2026 against live evidence from 192.168.1.200 (see the plan this
/// shipped under): continuous boost was excluded here on the grounds it had
/// no reliable evidence on the display; status::StatusTracker::
/// continuous_boost() now decodes it from the same "Boost Airflow" line1
/// signal boosting() already trusts, confirmed only after
/// CONTINUOUS_CONFIRM_MS to rule out a timed boost's own trailing sticky
/// window (status.h). Normalising below may still pass THROUGH it
/// transiently on the way back to Normal when the TARGET is something else
/// -- that remains harmless and deliberate, see SetAirflowMode's own
/// comment.
enum class AirflowTarget : uint8_t { NORMAL, BOOST_30, BOOST_60, BOOST_CONTINUOUS, PURGE };

/// Same reasoning as setting_for() above, for the one select: this lives in
/// the portable core so the host suite's -Werror catches a forgotten
/// mapping. `index` is the raw option index, which select.py's
/// AIRFLOW_MODE_OPTIONS deliberately orders to match AirflowTarget
/// index-for-index. nullopt for an unmapped key or an out-of-range index --
/// the latter cannot happen through select::SelectCall, which validates
/// against traits.get_options() first, but this is the boundary where that
/// assumption stops being someone else's to keep.
std::optional<AirflowTarget> airflow_target_for(SelectKey key, size_t index);

/// Sets the Main-key boost/purge state to an ABSOLUTE target. Main is a
/// cumulative press counter with no usable timeout -- 1 press = 30 min
/// boost, 2 = 60 min, 3 = continuous, 4 = back to Normal
/// (mhrv_orig/controls.yaml's measured table: "Presses 250ms apart are
/// counted individually and correctly") -- so "Boost 60" only means the same
/// thing every time if the counter is first normalised back to Normal and
/// then advanced by exactly the target's own press count. Purge is a
/// separate axis: a 5.5s Main hold TOGGLES it regardless of where the tap
/// counter sits, so it gets its own branch rather than being folded into the
/// counter.
///
/// Steps (full reasoning in seq_set_airflow_mode.cpp):
///  1. CHECK_CURRENT: no keys pressed. First PROVES the display is on the
///     status loop -- have_frame() true, screens::is_menu_screen() false --
///     and FAILS immediately if not, with no retry window, because a menu
///     screen does not clear itself within a sequence's patience. Being
///     PARKED on a menu is a reachable start state, not a hypothetical one:
///     Runner::recover()'s exit tap does not wait for the unit to actually
///     leave, the unit's own ~2-minute timeout may still be running, and a
///     human at the physical keypad can park it there at any moment. From a
///     menu screen classify_line()/purging() can never see Purge or Boost,
///     so the probe below would conclude "Normal" on zero evidence and then
///     tap Main against a counter whose position was never read. Second,
///     reads purge from the injected StatusTracker's STICKY purging(), not a
///     single frame (see set_status()). Branches to the Purge hold (direct
///     -- the manual describes Purge as reachable from any state), or for a
///     non-Purge target to CANCEL_PURGE first if purging, else to the probe.
///  2. CANCEL_PURGE / OPEN_PURGE: a fixed 5500ms Main hold via
///     press()+elapsed() rather than HoldUntil, because its success
///     condition is "the timer ran out" and not a predicate -- same
///     reasoning as FetchDiagnostics::HOLD_DOWN's fixed 8s hold.
///     CANCEL_PURGE is followed by an explicit ~400ms settle before the next
///     tap: a release is silence, so a hold-to-tap transition needs a gap or
///     the unit reads one unbroken press.
///  3. PROBE_CHECK / PROBE_WAIT (mhrv_orig's boost_probe): sample line1 for
///     "Boost Airflow" now; if absent, wait up to 8s for a Boost frame.
///     line1 alternates every ~3.2-3.5s, so one unlucky sample can catch the
///     other half of the cycle and the probe needs ~8s to be conclusive.
///  4. If boosting: tap Main once (bounded by a 4-tap guard), let the tap
///     and its key_gap clear, wait a further 1s (mhrv_orig's figure -- long
///     enough for the unit's counter and display to catch up before judging
///     it), then re-probe. Guard-exhausted-and-still-boosting FAILS rather
///     than layering the target's presses on top of an unknown count, which
///     is what mhrv_orig's boost_set silently did instead.
///  5. APPLY_TAP / APPLY_WAIT: once confirmed Normal, tap Main 0/1/2/3 times
///     for Normal/Boost30/Boost60/Continuous, queued as a batch and drained
///     through Keypad's queue exactly as GotoMenu does.
///
/// Accepted edge, chosen deliberately: purging()'s sticky
/// ALTERNATION_TIMEOUT_MS window (~12s, status.h) means a purge that ended
/// within the last ~12s still reads as purging, so a PURGE target in that
/// window no-ops rather than re-opening it. That is the safe direction --
/// trusting a stale "not purging" instead risks a wrong-direction 5.5s hold
/// CANCELLING a purge that is actually still running, in an occupied house.
/// PROBE_CHECK deliberately does NOT use this sticky reading; see its own
/// comment for the mirror-image reasoning.
///
/// NOT optimistic, same as WriteSetting: this sequence only presses keys and
/// never publishes. HA's airflow_mode comes entirely from the hub's PASSIVE
/// status-line decode (VentAxiaHub::publish_airflow_mode_()), off the same
/// boosting()/purging()/boost_time_remaining() the generic sensors already
/// use, so a press at the unit's physical keypad reaches HA exactly as a
/// command from HA does. That decode goes SILENT while a SetAirflowMode run
/// is the active root: normalising walks the unit through boost states
/// nobody chose (Boost 30 -> 60 -> continuous -> Normal is one lap of the
/// counter), and HA is meant to keep showing the OLD value for the ~25-30s a
/// transition takes, with `busy` surfacing that a change is in flight.
///
/// A long-lived hub member, reused for every write -- configure() before
/// each request(), see on_start() for what resets between runs.
class SetAirflowMode final : public Sequence {
 public:
  /// The alternation-aware, STICKY purging() this sequence needs at
  /// CHECK_CURRENT to know "is the unit currently purging" reliably. A
  /// single decoded frame is not enough -- the status loop alternates,
  /// which is exactly why StatusTracker models purging_ as a Flag with its
  /// own ALTERNATION_TIMEOUT_MS rather than trusting one frame (status.h's
  /// own class comment), and the purge layout being otherwise unresolved
  /// makes a single-frame miss MORE likely, not less. Must be set before
  /// request()ing this instance --
  /// VentAxiaHub::setup() wires it to &this->status_, the exact tracker
  /// every other status-derived entity already reads from, so this is not
  /// a second, separate purge decode. A null pointer is treated the same
  /// as "not yet known" at CHECK_CURRENT -- see its own comment for why
  /// that FAILS rather than being read as "not purging".
  void set_status(const status::StatusTracker *status) { this->status_ = status; }

  /// Selects the target -- call before request()ing this instance.
  void configure(AirflowTarget target) { this->target_ = target; }

  const char *name() const override { return "SetAirflowMode"; }
  void on_start() override;
  Poll poll() override;
  void on_finish(Poll result) override;

  // Worst case: the binding branch is target != PURGE while the unit IS
  // currently purging (CANCEL_PURGE runs, unlike the direct entry path) and
  // the target is BOOST_CONTINUOUS (APPLY's 3 taps, the most of any target).
  // TIMEOUT_BUDGET_MS is checked against WORST_CASE_MS below (static_assert)
  // rather than trusted by eye -- same "don't cut it close" reasoning as
  // every other timeout_ms() override in this file, and in practice
  // unreachable, since every RUNNING state in this sequence is already
  // internally bounded (see the class comment), unlike e.g. WriteSetting's
  // VERIFY/OPEN steps which wait on a specific screen that might never
  // arrive.
  uint32_t timeout_ms() const override { return TIMEOUT_BUDGET_MS; }

 private:
  enum Step : uint8_t {
    CHECK_CURRENT,
    CANCEL_PURGE,
    CANCEL_SETTLE,
    OPEN_PURGE,
    PROBE_CHECK,
    PROBE_WAIT,
    NORMALISE_WAIT,
    NORMALISE_SETTLE,
    APPLY_TAP,
    APPLY_WAIT,
    FINISHED,
  };

  // The manual's own figure for both starting and cancelling Purge
  // (mhrv_orig/mhrv.yaml's purge_ms) -- same gesture, same duration, either
  // direction.
  static constexpr uint32_t PURGE_HOLD_MS = 5500;
  // Matches FetchDiagnostics::DOWN_SETTLE_MS -- see this class's own comment
  // on why a hold's release needs an explicit gap before the next tap.
  static constexpr uint32_t CANCEL_SETTLE_MS = 400;
  static constexpr uint32_t PROBE_TIMEOUT_MS = 8000;      // mhrv_orig's boost_probe
  static constexpr uint32_t NORMALISE_SETTLE_MS = 1000;   // mhrv_orig's boost_normalise `delay: 1s`
  static constexpr uint8_t NORMALISE_GUARD = 4;           // one full lap of the unit's own boost counter

  // Reopened 13 Aug 2026 against live evidence from 192.168.1.200: a toilet
  // boost held on by a light-linked SWITCHED LIVE (the wired wall/toilet
  // switch stays asserted for as long as the light is on) accepts every
  // Main tap electrically -- line1 kept alternating "Boost Airflow"/"Summer
  // Bypass On" throughout -- but the switched live is itself holding the
  // unit's own boost input, so the tap counter this sequence is trying to
  // walk back to Normal never actually moves: line2's percentage AND
  // countdown sat dead still (48% forever, 30m/60m never once appearing)
  // across all 4 guard taps, and the run still failed at guard exhaustion
  // with a generic message that gave the person driving it no way to tell
  // "the unit ignored me" from "I don't know what state this is in". TWO
  // consecutive taps producing zero movement in (airflow_percent,
  // countdown_minutes) is that signature -- a SINGLE unmoving sample is not
  // enough to conclude it (a probe frame landing mid-alternation could
  // plausibly repeat one value even on a healthy unit), so this deliberately
  // waits for two in a row before bailing, same "don't trust one frame"
  // caution PROBE_WAIT's own alternation timeout already carries. This is an
  // EARLIER exit alongside NORMALISE_GUARD above, not a replacement for it --
  // NORMALISE_GUARD stays the outer bound for every other way normalising
  // can fail to converge (e.g. a genuinely moving but never-settling
  // counter), and deliberately is not lowered to match: 4 taps is one full
  // lap of the unit's own Normal->30->60->continuous counter, which is what
  // makes an exhausted guard state-neutral (after_probe_()'s own comment on
  // NORMALISE_GUARD) -- collapsing the two constants together would give up
  // that property for taps that ARE moving the counter, just slowly.
  static constexpr uint8_t STUCK_TAP_LIMIT = 2;

  // Worst case for the binding branch: currently purging (so
  // CANCEL_PURGE/CANCEL_SETTLE run) and target == BOOST_CONTINUOUS (3
  // presses_for_(), the most of any target). The normalise loop
  // (PROBE_CHECK/PROBE_WAIT -> after_probe_() -> NORMALISE_WAIT/
  // NORMALISE_SETTLE, seq_set_airflow_mode.cpp) runs one probe BEFORE each
  // of up to NORMALISE_GUARD taps, plus one FINAL probe after the last tap
  // to learn whether it can stop -- NORMALISE_GUARD+1 probes total, each
  // bounded by PROBE_TIMEOUT_MS regardless of how it resolves (a Boost frame
  // reappearing, or the timeout itself). The +1 for that final post-tap
  // probe is deliberate, not slack -- it is easy to drop when re-deriving
  // this sum, and the budget's headroom would hide the error.
  static constexpr uint8_t NORMALISE_PROBES = NORMALISE_GUARD + 1;
  static constexpr uint8_t MAX_APPLY_TAPS = 3;  // BOOST_CONTINUOUS -- presses_for_()'s own largest case

  static constexpr uint32_t WORST_CASE_MS = PURGE_HOLD_MS + CANCEL_SETTLE_MS +
                                             NORMALISE_PROBES * PROBE_TIMEOUT_MS +
                                             NORMALISE_GUARD * (kDefaultTapMs + kDefaultKeyGapMs) +
                                             NORMALISE_GUARD * NORMALISE_SETTLE_MS +
                                             MAX_APPLY_TAPS * (kDefaultTapMs + kDefaultKeyGapMs);

  static constexpr uint32_t TIMEOUT_BUDGET_MS = 90000;
  static_assert(TIMEOUT_BUDGET_MS > WORST_CASE_MS,
                "SetAirflowMode's root budget must exceed its own worst-case sum (cancelling an in-progress purge, "
                "then normalising back to Normal against the full NORMALISE_GUARD, then applying "
                "BOOST_CONTINUOUS's 3 taps), or a legitimate run can be killed mid-wait by the root timeout "
                "instead of being allowed to finish on its own");

  /// Shared by CANCEL_PURGE and OPEN_PURGE: holds Main for the fixed
  /// PURGE_HOLD_MS, then releases and moves to `next_step` -- see the class
  /// comment for why this is press()+elapsed(), not HoldUntil.
  Poll hold_main_(uint8_t next_step);

  /// Shared by PROBE_CHECK and PROBE_WAIT once either has an answer: decides
  /// whether to tap again (guard/stuck-detection permitting), fail outright,
  /// or move on to APPLY_TAP/FINISHED -- see the class comment, steps 3-5,
  /// and STUCK_TAP_LIMIT's own comment for the switched-live early exit.
  Poll after_probe_(bool boosting_now);

  /// 0/1/2/3 Main taps from Normal to reach `target` -- PURGE never reaches
  /// here, see CHECK_CURRENT/hold_main_ above.
  static uint8_t presses_for_(AirflowTarget target);

  AirflowTarget target_{AirflowTarget::NORMAL};
  uint8_t guard_{0};
  const status::StatusTracker *status_{nullptr};  // see set_status(), CHECK_CURRENT

  // Per-run stuck-tap tracking (STUCK_TAP_LIMIT above) -- reset in on_start(),
  // NOT here, because this Sequence is a long-lived hub member reused across
  // runs (this file's own Runner class comment): a value left over from a
  // PREVIOUS run's last probe would let that run's history leak into this
  // one's first comparison. last_airflow_percent_/last_countdown_minutes_
  // hold the most recent after_probe_(true) sample (status::parse_line_values(),
  // the same instantaneous read PROBE_CHECK's own defensive re-check uses --
  // see PROBE_CHECK's asymmetry comment for why this is deliberately NOT
  // status_'s sticky tracker); have_stuck_sample_ is false
  // until the first sample exists, so the very first probe never has anything
  // to compare against and can never itself count as "unmoving".
  std::optional<int> last_airflow_percent_;
  std::optional<int> last_countdown_minutes_;
  bool have_stuck_sample_{false};
  uint8_t stuck_taps_{0};
};

// ------------------------------------------------------------- ResetFilter --
// The ONE operation with no way back from software, which is why it was
// built last. Ported from mhrv_orig/controls.yaml's reset_filter script -- see that file's own
// comment for how the gesture itself (a 5s Up+Down hold on the status
// screen) was actually established: the manual originally looked
// undocumented because its button glyphs are embedded images, not a symbol
// font, and did not survive naive PDF text extraction.

/// Clears the "Check Filter" reminder and restarts the service countdown
/// (6/12/18 months, unit-dependent) by holding Up+Down together for a fixed
/// 5.5s on the status screen -- the protocol is a bitmask, so holding two
/// keys at once is fine, the same fact FetchDiagnostics' Up+Main entry combo
/// relies on (mhrv_orig/controls.yaml). This writes to the unit and restarts
/// the countdown at the full interval; there is no read-back that undoes it.
///
/// Steps (full reasoning in seq_reset_filter.cpp):
///  1. CHECK_STATUS: refuses, with no retry, unless the display has a frame
///     and is on the status loop rather than a menu or diagnostic screen.
///     This is the ONE guard between a stray press and an unrecoverable
///     write, so a wrong screen means refuse outright, not wait-and-see: a
///     menu screen does not clear itself within a sequence's patience.
///  2. HOLD/RELEASE_SETTLE: asserts Up+Down for a fixed 5500ms via
///     press()+elapsed() rather than HoldUntil (success is "the timer ran
///     out", not a predicate), then releases and settles. The settle is long
///     enough for the release-is-silence gap before FETCH's own Up+Main
///     hold, and long enough to show whatever the unit answers with -- which
///     is LOGGED and never acted on. This model's manual describes no
///     confirmation prompt, but other Vent-Axia models answer the same
///     gesture with a "Reset Filter?" prompt needing a second keypress,
///     untested here either way. Firing a guessed confirm press at whatever
///     is on screen would be worse than leaving an unconfirmed reset alone;
///     if a prompt ever appears, this log line is where a human sees it.
///  3. FETCH/VERIFY: chains a FetchDiagnostics run on its OWN dedicated
///     instance (see diagnostics_scan_ for why not the hub's shared one) to
///     re-read every page including 23, then reads filter_hours_source_ for
///     four distinguishable outcomes, each logged differently -- collapsing
///     any of them into "pass"/"fail" would hide followup a human needs:
///       - no reading at all -- cannot confirm either way, WARN.
///       - exactly 0 -- the reset did not take, WARN.
///       - above 0 but EQUAL to hours_before_ (the pre-hold snapshot) --
///         most likely page 23 was not re-read by this run rather than the
///         reset failing; logged with both numbers at WARN, not INFO. See
///         hours_before_ for the one case this cannot tell apart from a
///         genuine no-op reset.
///       - above 0 and DIFFERENT from hours_before_ -- confirmed, INFO.
///     VERIFY never returns FAILED on any of the four: the irreversible hold
///     already happened in step 2, so there is nothing left to protect by
///     failing here, only information to log. This sequence CAN still fail,
///     but only through FETCH's await() cascading when the chained
///     FetchDiagnostics could not complete at all -- a distinct situation
///     from "reached page 23 and didn't like what it saw".
///
/// A long-lived hub member, reused on every button press: no dynamic
/// allocation in steady state. on_start() resets hours_before_ to the
/// CURRENT filter_hours_source_ reading (see that member); diagnostics_scan_
/// resets its own per-run state in its own on_start() when FETCH await()s it.
class ResetFilter final : public Sequence {
 public:
  /// Wired to diagnostics_scan_'s own SuccessSink -- a genuine full
  /// diagnostic scrape happens inside this sequence exactly as it does for
  /// the button/schedule path, so diagnostics_updated should be stamped the
  /// same way. See VentAxiaHub::stamp_diagnostics_updated_().
  void set_on_diagnostics_success(FetchDiagnostics::SuccessSink sink) {
    this->diagnostics_scan_.set_on_success(std::move(sink));
  }

  /// Read-only view of the last known filter-hours reading (diagnostic page
  /// 23, SensorKey::FILTER_HOURS), injected the same way SetAirflowMode::
  /// set_status() and SyncClock::set_time_source() are, so this file never
  /// includes esphome/components/sensor/sensor.h (README "Portable core").
  /// It cannot be read off the diagnostics_scan_ child directly because
  /// diagnostic decoding is entirely passive: it is driven by the hub's
  /// on_change callback whenever line1/line2 change while a diagnostic page
  /// shows, not by whichever sequence happens to be scrolling through it.
  ///
  /// nullopt means "never published", mirroring sensor::Sensor::has_state()
  /// being false -- the same case mhrv_orig/controls.yaml's reset_filter
  /// checked with `!id(filter_hours).has_state()`. This deliberately carries
  /// forward that script's limitation: it is the LAST known reading, not
  /// necessarily one from THIS run's scan. If diagnostics_scan_ skipped page
  /// 23 (a dropped frame mid-scroll) but an earlier run had published a
  /// nonzero value, a bare read here would see the stale value rather than
  /// "no reading". A scan-scoped signal would need new per-run tracking on
  /// the hub side, since the decode is shared and passive.
  ///
  /// Most of that gap closes without touching the hub: on_start() snapshots
  /// this source into hours_before_ BEFORE the hold and VERIFY compares
  /// against it, so a stale-nonzero reading -- the dangerous direction, see
  /// hours_before_ -- can no longer be reported as a fresh confirmation.
  /// What it does not buy: it cannot prove page 23 WAS re-read, only that
  /// the value moved, so a scan missing page 23 both before AND after the
  /// hold still reads as "unchanged" rather than "no reading".
  using FilterHoursSource = std::function<std::optional<int>()>;
  void set_filter_hours_source(FilterHoursSource source) { this->filter_hours_source_ = std::move(source); }

  const char *name() const override { return "ResetFilter"; }
  void on_start() override;
  Poll poll() override;
  void on_finish(Poll result) override;

  // Worst case: HOLD's fixed HOLD_MS + RELEASE_SETTLE's fixed
  // RELEASE_SETTLE_MS + diagnostics_scan_'s own worst case AS A CHILD --
  // which is what actually bounds this, not FetchDiagnostics' own
  // (inherited, unused-as-a-child) timeout_ms(): Sequence::timeout_ms()'s
  // own comment is explicit that it is "only ever consulted for whichever
  // Sequence is currently the ROOT", so only THIS override matters once
  // diagnostics_scan_ is pushed as a child below (CHECK_STATUS and VERIFY
  // add nothing -- both are synchronous checks with no wait of their own).
  // TIMEOUT_BUDGET_MS is checked against WORST_CASE_MS below
  // (static_assert), so a legitimate full scan -- even one that ultimately
  // fails on ITS OWN internal timeout rather than succeeding -- always gets
  // to finish deciding that for itself instead of being cut off mid-wait by
  // this sequence's own budget expiring first.
  uint32_t timeout_ms() const override { return TIMEOUT_BUDGET_MS; }

 private:
  enum Step : uint8_t { CHECK_STATUS, HOLD, RELEASE_SETTLE, FETCH, VERIFY, FINISHED };

  // mhrv_orig/controls.yaml's own filter_reset_ms -- the manual's figure for
  // this gesture, same as PURGE_HOLD_MS is for Main (SetAirflowMode above),
  // just a different key combination.
  static constexpr uint32_t HOLD_MS = 5500;
  // mhrv_orig's own `delay: 1s` after the hold -- longer than
  // SetAirflowMode::CANCEL_SETTLE_MS/FetchDiagnostics::DOWN_SETTLE_MS's
  // 400ms because this settle does double duty: it is both the mandatory
  // hold-to-hold gap before FETCH's own Up+Main entry hold (CLAUDE.md's
  // "release is silence... needs an explicit gap") AND long enough for the
  // display to have shown whatever the unit answers the gesture with,
  // logged the moment this settle ends -- see the class comment. Neither
  // purpose is served by rushing this down to 400ms.
  static constexpr uint32_t RELEASE_SETTLE_MS = 1000;

  static constexpr uint32_t WORST_CASE_MS = HOLD_MS + RELEASE_SETTLE_MS + FetchDiagnostics::worst_case_ms();

  static constexpr uint32_t TIMEOUT_BUDGET_MS = 120000;
  static_assert(TIMEOUT_BUDGET_MS > WORST_CASE_MS,
                "ResetFilter's root budget must exceed its own worst-case sum, or a legitimate full diagnostics "
                "scan (diagnostics_scan_, pushed as a child) can be killed mid-wait by the root timeout instead of "
                "being allowed to finish -- or fail -- on its own");

  // A DEDICATED FetchDiagnostics instance, not the hub's shared
  // button/schedule one (VentAxiaHub::fetch_diagnostics_) -- same reasoning
  // as WriteSetting::read_back_ being its own ReadSettings rather than
  // reusing the hub's read_settings_ (vent_axia.h's own read_settings()
  // comment: "so a manual 'refresh' button press can never collide with a
  // write's own confirmation pass"). Runner's single-root-at-a-time
  // exclusivity (this file's own class comment) already makes that literal
  // collision structurally impossible even with a SHARED instance --
  // request() refuses a second root while one is running, and this
  // sequence's own await() below only ever pushes its child while THIS
  // sequence is that one root -- so a Sequence ending up on the stack twice
  // is not actually reachable either way. A private instance is still the
  // safer choice: it keeps this sequence's own per-run scan state
  // (highest_page_seen_/seen_pages_) from ever being shared with a wholly
  // unrelated logical operation (a manual "refresh diagnostics" press),
  // which is one fewer thing that has to stay correct under future changes
  // rather than one thing that merely happens to be safe today.
  FetchDiagnostics diagnostics_scan_;

  FilterHoursSource filter_hours_source_;
  // filter_hours_source_'s answer taken BEFORE the
  // hold, in on_start() -- the one piece of real per-run state this
  // sequence carries. Exists to catch the outcome that matters most: this
  // button is usually pressed at hours == 0, where a stale reading is also
  // 0 and VERIFY already warns "did not take" -- the safe direction. But
  // someone cleaning the filters EARLY presses this at a NONZERO reading,
  // and a scan that misses page 23 would then leave filter_hours_source_
  // still answering that same old nonzero value -- which, read bare in
  // VERIFY, is indistinguishable from a fresh, successful reset and would
  // report "confirmed" for a reset that may never have taken. That is the
  // one outcome here that must not be allowed to lie: it is what a human
  // reads before deciding whether to press this irreversible button again.
  // Comparing against this snapshot turns that case into "unchanged since
  // before the hold" instead -- WARN, not INFO. One case remains genuinely
  // unresolvable and is documented at VERIFY, not papered over here: a
  // reset pressed while the timer is already sitting at the full interval
  // (i.e. a reset with nothing to gain) leaves the reading unchanged even
  // though the hold may have worked, and reports the same "unchanged" WARN
  // as a scan that simply missed page 23 -- the two are not distinguishable
  // from this sequence's own evidence.
  std::optional<int> hours_before_;
};

}  // namespace vent_axia
}  // namespace esphome
