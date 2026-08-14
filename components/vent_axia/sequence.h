#pragma once

// The sequence engine (PLAN.md §2). Every user-visible operation (fetch
// diagnostics, sync clock, write a setting, set airflow mode, reset filter)
// is a multi-second, multi-step state machine that must not block loop(),
// must be mutually exclusive with every other one (they all fight over one
// display and one keypad), and must release the keypad on every exit path.
// Plain C++17, no ESPHome headers -- see README "Portable core".
//
// Three pieces, in this file:
//  - Poll / Sequence: one unit of work, pumped by whoever is running it.
//  - Runner: a fixed-depth stack of Sequences, pumped once per loop() tick.
//    Only the sequence on top of the stack runs. A single ROOT sequence at a
//    time is what gives mutual exclusion *structurally* -- the old setup's
//    hand-rolled `ui_busy` global (acquired at 5 sites, released at 12, one
//    miss away from deadlocking the device until reboot) does not get
//    reimplemented here, it stops existing: there is nowhere for a second
//    root sequence to run while one is already on the stack.
//  - The primitives every later sequence is built from: Tap, HoldUntil,
//    GotoMenu, LeaveMenu.
//
// FetchDiagnostics (stage 5) and ReadSettings/WriteSetting (stage 6) are
// declared here too, alongside the primitives, rather than in their own
// headers -- see entities.h for the same "one growing file, not one file per
// addition" choice. Each gets only its method bodies in its own seq_*.cpp
// (seq_fetch_diagnostics.cpp, seq_read_settings.cpp, seq_write_setting.cpp);
// stage 7 adds SyncClock, SetAirflowMode, ResetFilter and ManualKey the same
// way.
//
// Stage 6 also adds three primitives on top of stage 5's Tap/HoldUntil/
// GotoMenu/LeaveMenu: OpenEditor, AdjustField and ExitEditChain -- the three
// pieces PLAN.md's "editing model" section is about. Set is the only key
// that is safe once an editor is open (walking Up out of one silently took a
// 14C setpoint to 19C on the real unit), so these three primitives are the
// only things in this component that are allowed to open one, adjust a value
// inside one, or walk one closed.
//
// Driven entirely by Runner::loop(uint32_t now_ms), the same discipline as
// Keypad (see keypad.h): a Sequence never calls millis() itself, it asks its
// Runner (see Sequence::elapsed()), which is what keeps this file host
// testable with explicit timestamps and no sleeping.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "display.h"
#include "entities.h"
#include "keypad.h"
#include "protocol.h"
#include "status.h"  // SetAirflowMode::set_status() -- status.h is portable core, see its own header comment

namespace esphome {
namespace vent_axia {

enum class Poll : uint8_t { RUNNING, DONE, FAILED };

class Runner;

/// One unit of work: a single key hold, a menu navigation, or a whole
/// user-visible operation composed from smaller Sequences via await(). See
/// this file's class comment for the shape.
class Sequence {
 public:
  virtual ~Sequence() = default;

  /// Short, human name for logging, the BUSY-adjacent "what's running"
  /// surface (Runner::running_name()) and the on_sequence_failed trigger
  /// payload -- e.g. "FetchDiagnostics".
  virtual const char *name() const = 0;

  /// Pumped once per loop() tick while this Sequence is on top of the
  /// Runner's stack -- never otherwise. Implementations read as a switch on
  /// step_ (see goto_step()); PLAN.md §2's WriteSetting is the model to
  /// match: a flat list of named steps, each one line where possible.
  virtual Poll poll() = 0;

  /// Runs once, the same tick this Sequence is first pushed -- as a root via
  /// Runner::request(), or as a child via await() -- before its first
  /// poll().
  virtual void on_start() {}

  /// ALWAYS runs when this Sequence leaves the Runner's stack, however it
  /// got there: it completed, a child it was await()ing failed (propagating
  /// up without this Sequence ever being polled again), or the Runner's own
  /// timeout fired. This is THE release site, replacing the old setup's 12
  /// scattered ones: anything this Sequence itself asserted on the keypad
  /// must be released here, unconditionally, not only on the success path.
  virtual void on_finish(Poll result) { (void) result; }

  /// Backstop for the whole *root* run -- see Runner::loop(). Individual
  /// steps are expected to carry their own, usually much shorter, timeouts
  /// (HoldUntil takes one explicitly); this is the "something is wrong and
  /// nothing else caught it" net, not a per-step budget, and it is only ever
  /// consulted for whichever Sequence is currently the ROOT of the stack.
  ///
  /// The default MUST stay above LeaveMenu::WAIT_TIMEOUT_MS (130s), which is
  /// itself above the unit's own ~2-minute menu timeout. Any sequence nesting
  /// LeaveMenu -- every settings write in stages 6-7 does, to get out of an
  /// editor that will not close any other way -- can legitimately sit there
  /// for the full 130s. A root budget below that would kill the run mid-wait
  /// and report a correctly-handled stuck editor as a failure. Raising an
  /// individual sequence's timeout_ms() above this is fine; lowering this
  /// default is not.
  virtual uint32_t timeout_ms() const { return 180000; }

 protected:
  /// Moves to step `s` and restarts elapsed() from now -- the only
  /// sanctioned way to change step_, so entered_ can never drift out of sync
  /// with it. Always returns Poll::RUNNING, so a case can end with
  /// `return goto_step(NEXT);`.
  Poll goto_step(uint8_t s);

  /// Milliseconds since the current step was entered (goto_step(), or
  /// on_start() for step 0) -- what a step's own timeout is measured
  /// against.
  uint32_t elapsed() const;

  /// Runs `child` to completion, then resumes THIS Sequence at step `on_ok`
  /// -- but only if child succeeds. A child that fails is not survivable:
  /// this Sequence fails too, without ever being polled again, propagating
  /// up however many await() levels are currently nested (see Runner's
  /// class comment). Also fails (synchronously, this call) if the Runner's
  /// stack is already at its fixed depth -- see Runner::MAX_DEPTH.
  Poll await(Sequence &child, uint8_t on_ok);

  Runner *runner_{nullptr};
  uint8_t step_{0};
  uint32_t entered_{0};

 private:
  friend class Runner;
};

/// Owns the stack, the clock, the per-root timeout and the shared recovery
/// path. One Runner per hub.
class Runner {
 public:
  /// root -> GotoMenu -> HoldUntil is 3; 4 leaves headroom without inviting
  /// deep nesting. Fixed rather than a std::vector deliberately -- see the
  /// class comment on no dynamic allocation in steady state. Overflowing it
  /// is a bug (a sequence nesting this deep is a design smell to fix, not a
  /// depth to grow for), and is handled as a synchronous await() failure
  /// rather than corrupting the stack -- see push_child_().
  static constexpr uint8_t MAX_DEPTH = 4;

  /// Same shape as Keypad::LogSink, reused rather than redeclared -- both
  /// exist for the same reason (this file must stay ESPHome-free but some
  /// events here are meant to be loud, see keypad.h's comment).
  using LogSink = Keypad::LogSink;

  /// Fired once a ROOT sequence finishes as FAILED, with its name -- what
  /// backs YAML's on_sequence_failed (PLAN.md §5). Not fired for a failure
  /// that a parent's await() would have survived (there isn't one at this
  /// stage), and not fired for a child failing on its own -- only ever for
  /// the whole run.
  using FailureSink = std::function<void(const std::string &name)>;

  /// Keypad is mutated (tap/press/release); Display is only ever read. Both
  /// outlive the Runner -- the hub owns all three as long-lived members.
  Runner(Keypad &keypad, const Display &display) : keypad_(keypad), display_(display) {}

  void set_log_sink(LogSink sink) { this->log_ = std::move(sink); }
  void set_on_sequence_failed(FailureSink sink) { this->on_failure_ = std::move(sink); }

  /// Fed by the hub every tick from its own link_up computation (PLAN.md
  /// §7's "Link loss") -- request() refuses to start anything while this is
  /// false, same reasoning as refusing a second root: driving the keypad at
  /// a unit that has stopped talking to us cannot possibly succeed and would
  /// just run out the clock on a timeout instead.
  void set_link_up(bool up) { this->link_up_ = up; }

  /// Starts `seq` as a new root sequence. Refuses (logging which sequence is
  /// blocking, or that the link is down) rather than queuing or interrupting
  /// -- a single root at a time is what makes mutual exclusion structural,
  /// see this file's class comment.
  bool request(Sequence &seq);

  /// Pumps whichever Sequence is on top of the stack, exactly once. Also
  /// enforces the per-root timeout (see Sequence::timeout_ms()) and, on
  /// exhausting it, aborts the whole run and calls recover() -- see
  /// finish_top_().
  void loop(uint32_t now_ms);

  /// True whenever a root sequence is in progress, whatever depth it has
  /// pushed children to. Backs the hub's BUSY binary sensor alongside
  /// Keypad::busy().
  bool busy() const { return this->depth_ > 0; }

  /// The root sequence's name while busy(), or "" -- for logging and the
  /// hub's dump_config(), not meant to be published as an entity itself.
  const char *running_name() const { return this->busy() ? this->stack_[0].seq->name() : ""; }

  uint32_t now_ms() const { return this->now_ms_; }
  const Display &display() const { return this->display_; }

  /// The single choke point every Sequence primitive uses to reach the
  /// keypad -- there is deliberately no plain accessor for the Keypad
  /// itself, so a primitive cannot bypass what these two enforce. Same
  /// interlock VentAxiaHub::tap_key()/hold_key() enforce for every other
  /// caller (PLAN.md §7): refused, and logged, if `mask` includes Set while
  /// the display is currently showing a diagnostic page (page 27, "Reset",
  /// writes and has never been tried). Returns false when refused, so a
  /// primitive can fail fast -- see Tap::poll()/HoldUntil::poll() -- rather
  /// than wait out a timeout for a press that was never going to happen.
  /// This is also what makes the interlock host-testable at all: it used to
  /// live only in vent_axia.cpp, which the host test suite cannot compile
  /// (see README "Portable core") -- see PLAN.md §7's "asserted globally in
  /// the test suite, not just avoided by convention".
  bool tap(protocol::KeyMask mask, uint32_t duration_ms);
  bool press(protocol::KeyMask mask);

  /// Never interlocked -- releasing a key is always safe, refusing it never
  /// is (same reasoning as VentAxiaHub::release_keys()).
  void release() { this->keypad_.release(); }
  bool keypad_busy() const { return this->keypad_.busy(); }

  /// The shared abort path (PLAN.md §2), run automatically whenever a root
  /// sequence finishes as FAILED (including via timeout) -- see
  /// finish_top_(). Public so a test can also drive it directly. Releases
  /// every key and, if the display is parked on a menu screen, issues the
  /// single verified exit tap -- see sequence.cpp for why this is not a
  /// blocking wait for the unit to actually leave the menu.
  void recover();

 private:
  friend class Sequence;

  struct Frame {
    Sequence *seq;
    uint8_t resume_step;  // what to resume the FRAME BELOW at, once this one succeeds
  };

  /// Pushes `child` and runs its on_start(). Fails (false, nothing pushed)
  /// only on stack overflow -- see MAX_DEPTH. Deliberately synchronous and
  /// side-effect-free on failure: Sequence::await() turns a false here
  /// straight into Poll::FAILED for its caller rather than this function
  /// trying to unwind a stack it never touched.
  bool push_child_(Sequence &child, uint8_t resume_step);

  /// Pops the top frame and runs its on_finish(result) -- unconditionally,
  /// the one release site (see Sequence::on_finish()'s comment). Then:
  ///  - if the stack is now empty, this was the root: on FAILED, fires
  ///    on_failure_ and calls recover().
  ///  - on FAILED with a parent still on the stack, cascades: the parent
  ///    never gets polled again, its own on_finish(FAILED) fires
  ///    immediately via a recursive call here (bounded by MAX_DEPTH, so this
  ///    never runs away).
  ///  - on DONE with a parent still on the stack, resumes the parent at the
  ///    step recorded when it awaited this child.
  void finish_top_(Poll result);

  /// Ported near-verbatim from what was vent_axia.cpp's
  /// refuse_if_set_interlocked_() -- moved here (stage 5) so it is enforced
  /// for every path that can reach the keypad, not only the pre-sequence
  /// ones (button.py's key_* buttons, the vent_axia.tap_key/hold_key
  /// actions), which now go through tap()/press() above too rather than
  /// duplicating the check.
  bool refuse_if_set_interlocked_(protocol::KeyMask mask) const;

  Keypad &keypad_;
  const Display &display_;
  LogSink log_;
  FailureSink on_failure_;
  bool link_up_{false};

  std::array<Frame, MAX_DEPTH> stack_{};
  uint8_t depth_{0};
  uint32_t now_ms_{0};
  uint32_t root_started_at_ms_{0};
};

// --------------------------------------------------------------- primitives --
// Needed by this stage's FetchDiagnostics and, per PLAN.md §3, by every
// sequence that comes after it.

/// Issues runner_->tap(mask, duration_ms) once (in on_start(), never
/// repeated -- tap() enqueues, it is not idempotent like press()) and
/// completes once !runner_->keypad_busy(), i.e. once the mandatory key_gap
/// has elapsed too, not just the tap itself -- so a caller chaining Tap
/// after Tap via await() never needs a delay step of its own.
class Tap final : public Sequence {
 public:
  Tap(protocol::KeyMask mask, uint32_t duration_ms) : mask_(mask), duration_ms_(duration_ms) {}

  const char *name() const override { return "Tap"; }
  void on_start() override;
  Poll poll() override;

 private:
  protocol::KeyMask mask_;
  uint32_t duration_ms_;
  // Whether on_start()'s tap() call was actually queued, or refused by the
  // Set interlock (Runner::tap()) -- see poll()'s fast-fail on false.
  bool sent_{false};
};

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
/// needing one HoldUntil member per hold -- see PLAN.md's "no dynamic
/// allocation in steady state".
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
/// regardless of where the display started (PLAN.md §2/§3) -- this counts
/// taps, it never assumes or tracks the screen's current position, which is
/// why it works correctly right after boot or after some other navigation
/// left the display somewhere unknown. Menu map (PLAN.md §2): 0 status, 1 Set
/// Clock, 2 Summer Mode, 3 Indoor Temp.
class GotoMenu final : public Sequence {
 public:
  GotoMenu() = default;
  explicit GotoMenu(uint8_t index) : index_(index) {}

  /// Reconfigures for a fresh target. Must only be called while this
  /// instance is NOT on the Runner's stack, same rule as HoldUntil::reset()
  /// (see its comment). Added in stage 6 so ReadSettings/WriteSetting can
  /// reuse ONE long-lived GotoMenu across several different targets in their
  /// own steps (their own menu entry, then 0 to go home) rather than needing
  /// one member per target -- see PLAN.md's "no dynamic allocation in steady
  /// state".
  void reset(uint8_t index) { this->index_ = index; }

  const char *name() const override { return "GotoMenu"; }
  Poll poll() override;

 private:
  enum Step : uint8_t { QUEUE_UP, WAIT_UP, SETTLE_UP, QUEUE_DOWN, WAIT_DOWN, SETTLE_DOWN };

  uint8_t index_{0};
};

/// At most ONE Up tap -- never into an open editor -- then, if the display
/// is still showing a menu screen, waits out the unit's own ~2-minute menu
/// timeout rather than pressing again. PLAN.md §3: mashing Up would corrupt
/// a setting if an editor is still open (observed on the real unit: a
/// second Up silently walked a 14°C setpoint to 19°C). This is deliberate,
/// measured behaviour, not laziness -- do not "helpfully" retry the tap.
///
/// Stage 7a widened the guard: TAP itself now checks Display::editor_open()
/// before pressing anything, the same guard Runner::recover() has carried
/// since stage 6 (see its own comment). This matters because LeaveMenu can
/// run immediately after a sequence's own commit Set (SyncClock's LEAVE
/// step, following its fourth/COMMIT Set) -- if that commit was dropped,
/// the editor is still open, and Up would adjust the field under the cursor
/// instead of navigating out, the exact 14°C->19°C failure above. So
/// "exactly one Up" (the old contract) is now "at most one Up, and never
/// into an open editor": when the editor is still open, TAP skips straight
/// to WAIT_EXIT and lets the unit's own timeout close it without committing
/// anything.
class LeaveMenu final : public Sequence {
 public:
  const char *name() const override { return "LeaveMenu"; }
  Poll poll() override;

 private:
  enum Step : uint8_t { TAP, WAIT_TAP, CHECK, WAIT_EXIT };

  // The unit's own menu timeout is ~2 minutes; this is that plus headroom,
  // not a guess -- PLAN.md §3.
  static constexpr uint32_t WAIT_TIMEOUT_MS = 130000;
};

// ---------------------------------------------------------- editing model --
// Stage 6's three new primitives, per PLAN.md's "The unit's editing model":
// OpenEditor (tap Set, confirm it actually opened, retry once), AdjustField
// (the closed loop every field this component writes shares) and
// ExitEditChain (the walk-out, Set only). Ported from the old
// mhrv_orig/summer_bypass.yaml's open_editor/adjust loops/exit_edit_chain
// scripts -- the comments there record real observations on the physical
// unit, carried into the class comments below rather than just the code.

/// Taps Set and confirms an editor actually opened (Display::editor_open()),
/// retrying once before giving up -- the old open_editor script's
/// `repeat: count: 2`. Worth the trouble because a dropped Set is the one
/// failure here with teeth: every primitive that runs after this one
/// (AdjustField in particular) presses Up/Down expecting to adjust a value,
/// and if no editor actually opened those same presses are navigation
/// instead, walking the display back up the menu while the value being
/// "adjusted" never budges -- observed exactly once on the real unit, before
/// gap_ms went up to 400ms (PLAN.md §3).
class OpenEditor final : public Sequence {
 public:
  const char *name() const override { return "OpenEditor"; }
  void on_start() override;
  Poll poll() override;

 private:
  enum Step : uint8_t { TAP, WAIT_TAP, SETTLE, CHECK };

  // Matches the old open_editor script's `delay: 700ms` -- long enough for
  // the unit to start blinking the value if an editor really opened.
  static constexpr uint32_t SETTLE_MS = 700;
  static constexpr uint8_t MAX_ATTEMPTS = 2;

  uint8_t attempt_{0};
};

/// The closed loop behind every field this component writes, and (stage 7)
/// the clock's day/hour/minute too -- shared rather than copy-pasted per
/// field the way the old YAML's three apply_* scripts were (PLAN.md §2/§3).
/// ValueParser/DirectionFn are what varies per field; see
/// parse_summer_mode_field()/parse_temp_field()/direction_no_wrap() below for
/// the concrete ones this stage uses, and read_fresh_value() for the sibling
/// helper WriteSetting's VERIFY step and ReadSettings share with this class.
///
/// One iteration, run from poll():
///  1. Bail (FAILED) if the guard is already exhausted -- bounds the loop so
///     a misread, or a value the unit refuses (e.g. outside Outdoor Temp's
///     5-20 C range, PLAN.md risk 6), cannot become a key-mashing runaway.
///  2. Re-read the target via target_ (TargetFn, see below) -- FAILED,
///     logged, if it is not available right now. For the fixed-target
///     reset() overload this call can never fail; it only matters for stage
///     7's clock fields, where "not available" means no time source at all
///     or a clock that has not synced yet.
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
  /// that simple: every field this stage writes is a plain sign comparison
  /// (none of the three wrap -- PLAN.md is explicit that parser::wrapped_delta
  /// would be actively wrong on all three, see direction_no_wrap() below),
  /// but stage 7's clock fields DO wrap and need a shortest-path version
  /// instead (direction_wrap_24/direction_wrap_60 below), and this is the
  /// seam that lets them reuse this same class.
  using DirectionFn = bool (*)(int cur, int want);

  /// Supplies the field's target, re-read fresh on every CHECK iteration
  /// rather than fixed once at reset() time -- what stage 7's clock fields
  /// need and WriteSetting's plain fields do not: the minute field alone can
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
  /// DirectionFn above: those two are always one of a handful of stateless
  /// free functions known at compile time, but a live target has to capture
  /// something stateful to be live at all -- here, the hub's own clock
  /// (VentAxiaHub::time_, guarded by USE_TIME -- see seq_sync_clock.cpp) --
  /// and a bare function pointer cannot carry that capture. Assigned only at
  /// reset()/configure() time, a handful of times per run rather than every
  /// tick, so this is not the "no dynamic allocation in steady state" the
  /// class comment elsewhere on this file cares about -- HoldUntil::Predicate
  /// is the same shape for the same reason.
  using TargetFn = std::function<bool(int &out)>;

  using LogSink = Keypad::LogSink;
  void set_log_sink(LogSink sink) { this->log_ = std::move(sink); }

  AdjustField() = default;

  /// The fixed-target form every field but the clock's uses (WriteSetting) --
  /// implemented in terms of the live-target overload below via a capturing
  /// lambda that always returns the same value, so there is exactly one
  /// CHECK implementation for both shapes. Must only be called while this
  /// instance is NOT on the Runner's stack, same rule as HoldUntil::reset().
  /// A single long-lived instance serves every field WriteSetting can write
  /// -- see PLAN.md's "no dynamic allocation in steady state".
  void reset(ValueParser parse, DirectionFn direction, int target, int guard_limit);

  /// Stage 7's live-target form -- see TargetFn's own comment for why
  /// SyncClock needs this and WriteSetting does not. Must only be called
  /// while this instance is NOT on the Runner's stack, same rule as
  /// HoldUntil::reset(). SyncClock reuses ONE long-lived instance across its
  /// day/hour/minute steps, same "no dynamic allocation in steady state"
  /// reasoning as WriteSetting's own use of this class.
  void reset(ValueParser parse, DirectionFn direction, TargetFn target, int guard_limit);

  const char *name() const override { return "AdjustField"; }
  void on_start() override;
  Poll poll() override;

 private:
  enum Step : uint8_t { CHECK, WAIT_CHANGE };

  static constexpr uint32_t CHANGE_TIMEOUT_MS = 900;

  ValueParser parse_{nullptr};
  DirectionFn direction_{nullptr};
  TargetFn target_;
  int guard_limit_{0};
  int guard_count_{0};
  LogSink log_;
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
/// timeout (up to 150s), documented to close any editor without committing
/// (PLAN.md). Only fails if editor_open() is STILL true once that fallback
/// itself elapses -- same shape as LeaveMenu's own timeout, and for the same
/// reason: a FAILED here cascades straight to the shared Runner::recover()
/// path, which presses Up **at most once**, which is safer than letting a
/// caller's own next step (a plain GotoMenu, say) mash Up five times against
/// a display state this primitive was never able to confirm was safe.
class ExitEditChain final : public Sequence {
 public:
  using LogSink = Keypad::LogSink;
  void set_log_sink(LogSink sink) { this->log_ = std::move(sink); }

  const char *name() const override { return "ExitEditChain"; }
  void on_start() override;
  Poll poll() override;

 private:
  enum Step : uint8_t { CHECK, WAIT_TAP, SETTLE, WAIT_TIMEOUT };

  static constexpr uint8_t MAX_COMMITS = 4;
  static constexpr uint32_t COMMIT_SETTLE_MS = 1800;
  // The old YAML's own number (mhrv_orig/summer_bypass.yaml's
  // exit_edit_chain): comfortably above the unit's own ~2-minute timeout.
  static constexpr uint32_t FALLBACK_TIMEOUT_MS = 150000;

  uint8_t commits_{0};
  LogSink log_;
};

// ------------------------------------------------------- settings fields --
// Shared by ReadSettings and WriteSetting (seq_read_settings.cpp,
// seq_write_setting.cpp): the three bypass fields' value encoding, shaped to
// match AdjustField::ValueParser/DirectionFn so the exact same functions
// serve both a plain read and a WriteSetting SettingSpec table row -- see
// PLAN.md §2's "one class... three table rows, not three near-identical
// copies".

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
/// Temp, none of which wrap (PLAN.md is explicit that parser::wrapped_delta
/// would be actively wrong on all three; it is for the clock's hour/minute
/// only, stage 7).
bool direction_no_wrap(int cur, int want);

/// True once line2 has published something NEWER than `since_ms` (the moment
/// navigation to the CURRENT screen started) that also parses -- see
/// PLAN.md "Reading a value off the screen": a change strictly after
/// `since_ms` is what proves a reading belongs to the screen just arrived
/// at, rather than being a stale value left over from wherever the display
/// was before. Returns nullopt while still waiting; callers apply their own
/// timeout. Shared because ReadSettings' plain screens and WriteSetting's
/// VERIFY step both need exactly this.
std::optional<int> read_fresh_value(const Display &display, uint32_t since_ms, AdjustField::ValueParser parse);

// ------------------------------------------------------------ clock fields --
// Stage 7's SyncClock (seq_sync_clock.cpp): the Set Clock screen's three
// fields, shaped to match AdjustField::ValueParser/DirectionFn exactly like
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

/// PLAN.md §3: Up+Main to enter the diagnostic menu, hold Down 8s to auto-
/// scroll through every page, then the verified two-stage exit (release,
/// settle, a FRESH hold of Up -- see seq_fetch_diagnostics.cpp's poll() for
/// why holding straight through never works). Decodes nothing itself: the
/// hub's existing on_change callback (stage 3) publishes every page passed
/// through as it goes by. Tracks the highest page number actually seen
/// rather than waiting for a hardcoded terminator -- firmware V32/05 stops
/// at page 27, and the old component's wait for "Diagnostic  28" hung for
/// 60s and abandoned the display in the menu.
///
/// A long-lived hub member, reused on every scheduled or button-triggered
/// run -- see on_start() for what gets reset between runs.
class FetchDiagnostics final : public Sequence {
 public:
  using LogSink = Keypad::LogSink;
  /// Called once, only after a fully successful run -- the hub uses this to
  /// stamp diagnostics_updated with the current wall-clock time. The
  /// indirection exists because portable core has no notion of wall-clock
  /// time (see README "Portable core"): this file never touches
  /// time::RealTimeClock, the hub does that on the other side of the sink.
  using SuccessSink = std::function<void()>;

  void set_log_sink(LogSink sink) { this->log_ = std::move(sink); }
  void set_on_success(SuccessSink sink) { this->on_success_ = std::move(sink); }

  const char *name() const override { return "FetchDiagnostics"; }
  void on_start() override;
  Poll poll() override;
  void on_finish(Poll result) override;

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
  LogSink log_;
  SuccessSink on_success_;
  int highest_page_seen_{-1};
  std::array<bool, MAX_PAGE_INDEX> seen_pages_{};
};

// -------------------------------------------------------------- settings --
// Stage 6: ReadSettings and WriteSetting, PLAN.md §3's remaining two
// sequences that were not FetchDiagnostics. Both drive the same three
// screens (Summer Mode, Indoor Temp, Outdoor Temp) via the editing-model
// primitives just above.

/// One pass reading Summer Mode, Indoor Temp, then Outdoor Temp via the
/// edit-chain hop (PLAN.md §3/§6) -- see this file's class comment for the
/// primitives it is built from. Publishes whatever it manages to read via
/// on_switch()/on_number(); a value that doesn't parse (blank/blinking, or a
/// screen that was never reached) is logged and simply left unpublished,
/// same "never hard-fail a read" philosophy as the rest of this component --
/// one unreadable value is not a reason to withhold the other two.
///
/// Reading Outdoor Temp requires opening Indoor Temp's editor and stepping
/// past it, so a read can leave the display mid-edit whether or not it
/// landed where it meant to (PLAN.md: the chain's shape is not guaranteed --
/// a commit has been observed closing the editor outright rather than
/// advancing). Both the "landed on Outdoor Temp" and "did not" branches of
/// that hop fall through to the SAME EXIT_CHAIN step below -- a single
/// funnel every path passes through structurally, not a per-branch call the
/// old YAML's indentation had to be trusted to get right in both an `if` and
/// an `else` (mhrv_orig/summer_bypass.yaml's read_summer_settings).
///
/// Finishes by returning the display to the status screen (GotoMenu(0)).
/// A long-lived hub member, reused on every button press -- see on_start()
/// for what resets between runs.
class ReadSettings final : public Sequence {
 public:
  using LogSink = Keypad::LogSink;
  using SwitchSink = std::function<void(SwitchKey, bool)>;
  using NumberSink = std::function<void(NumberKey, int)>;

  void set_log_sink(LogSink sink) {
    this->log_ = sink;
    // Forwarded once, here, rather than every run -- ExitEditChain is a
    // private member (see below) the hub cannot reach directly.
    this->exit_chain_.set_log_sink(std::move(sink));
  }
  void set_on_switch(SwitchSink sink) { this->on_switch_ = std::move(sink); }
  void set_on_number(NumberSink sink) { this->on_number_ = std::move(sink); }

  const char *name() const override { return "ReadSettings"; }
  void on_start() override;
  Poll poll() override;
  void on_finish(Poll result) override;

  // The outdoor hop nests ExitEditChain's own up-to-~157s fallback wait (4
  // commits, ~7.2s, plus up to 150s waiting out the unit's own timeout) --
  // see Sequence::timeout_ms()'s comment on why anything nesting a wait like
  // that needs a root budget with real headroom above it. The default 180s
  // leaves only ~23s for two GotoMenus, three value-waits and the hop's own
  // navigation on top of that -- not comfortable, so this is raised.
  uint32_t timeout_ms() const override { return 240000; }

 private:
  enum Step : uint8_t {
    NAV_SUMMER,
    WAIT_SUMMER,
    NAV_INDOOR,
    WAIT_INDOOR,
    OPEN_OUTDOOR,
    HOP_COMMIT,
    WAIT_HOP_TAP,
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

  GotoMenu goto_menu_;      // reused for NAV_SUMMER, NAV_INDOOR and HOME -- reset() before each
  OpenEditor open_editor_;  // the outdoor hop's opening move
  ExitEditChain exit_chain_;

  uint32_t nav_started_ms_{0};
  // Gates the outdoor hop: opening an editor on a screen that could not be
  // identified is exactly the situation WriteSetting aborts on, so a read
  // that never saw a clean Indoor Temp value does not attempt it either --
  // mhrv_orig/summer_bypass.yaml's own guard on enter_outdoor_editor.
  bool indoor_read_ok_{false};

  LogSink log_;
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

/// Writes one of the three bypass settings, then reads all three back as
/// confirmation (PLAN.md §2's WriteSetting body). configure() selects the
/// row (SettingId) and target value before this instance is request()ed; the
/// STEPS below are the same for all three -- see seq_write_setting.cpp's
/// SettingSpec table for what actually differs between them. This is the
/// first sequence in this component that presses Set: see this file's class
/// comment, and PLAN.md's editing-model section, for why every step past
/// OPEN below only ever does so through OpenEditor/AdjustField/ExitEditChain.
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
  using LogSink = Keypad::LogSink;

  void set_log_sink(LogSink sink) {
    this->log_ = sink;
    // Forwarded once, here -- exit_chain_, adjust_field_ and read_back_ are
    // private members the hub cannot reach directly. read_back_ is a full
    // ReadSettings, so this also reaches ITS OWN exit_chain_ (see
    // ReadSettings::set_log_sink). adjust_field_ forwarding added stage 7a --
    // it previously had a log_ member and an unavailable-target error with
    // nothing wired up to receive it, so that failure mode logged nothing.
    this->exit_chain_.set_log_sink(sink);
    this->adjust_field_.set_log_sink(sink);
    this->read_back_.set_log_sink(std::move(sink));
  }
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

  // Nests ExitEditChain's own up-to-~157s fallback wait potentially TWICE --
  // once directly (EXIT_CHAIN below) and once more inside read_back_'s own
  // outdoor hop (READ_BACK) -- on top of AdjustField's own worst case
  // (guard_limit 40 taps for a temperature, each up to ~1.35s, so up to
  // ~54s) and the rest of the navigation/verify/settle steps. ~400s is the
  // realistic worst-case sum; this leaves comfortable headroom above it
  // rather than cutting it close, for the same reason Sequence::timeout_ms()
  // gives LeaveMenu's 130s wait headroom: a root budget that expires WHILE
  // ExitEditChain is patiently waiting out a genuinely still-open editor
  // would abandon the run at the worst possible moment.
  uint32_t timeout_ms() const override { return 480000; }

 private:
  enum Step : uint8_t {
    NAVIGATE,
    VERIFY,
    OPEN,
    HOP_COMMIT,
    WAIT_HOP_TAP,
    WAIT_HOP_SCREEN,
    ADJUST,
    COMMIT,
    WAIT_COMMIT_TAP,
    SETTLE,
    EXIT_CHAIN,
    HOME,
    READ_BACK,
    FINISHED,
  };

  static constexpr uint32_t VERIFY_TIMEOUT_MS = 3000;      // PLAN.md §2's WriteSetting body
  static constexpr uint32_t HOP_SCREEN_TIMEOUT_MS = 3000;  // matches ReadSettings' own outdoor hop
  static constexpr uint32_t SETTLE_MS = 1800;              // PLAN.md §2's WriteSetting body

  SettingId id_{SettingId::SUMMER_MODE};
  const SettingSpec *spec_{nullptr};
  int target_{0};
  uint32_t nav_started_ms_{0};  // for VERIFY's read_fresh_value() -- see PLAN.md "Reading a value off the screen"
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

  LogSink log_;
};

// ---------------------------------------------------------------- SyncClock --
// Stage 7: the Set Clock screen, PLAN.md §3's remaining stage-7 sequence that
// touches this file (SetAirflowMode/ResetFilter/ManualKey are separate
// deliverables). Ported from mhrv_orig/controls.yaml's sync_clock/
// sync_clock_run/adjust_day/adjust_hour/adjust_minute scripts -- see this
// class's own comment and seq_sync_clock.cpp for where each observation on
// the physical unit landed.

/// Corrects the unit's own clock against wall-clock time, one field at a
/// time through the Set Clock editor. How the editor behaves, measured on
/// the real unit (mhrv_orig/controls.yaml's own comment, carried forward
/// here): Set enters on the day field; each further Set accepts the current
/// field and advances to the next; a FOURTH Set commits and drops out of the
/// editor. The field being edited blinks (blanks on alternate frames --
/// Display::editor_open()'s own signal), which is both how the unit shows
/// which field is active and why parse_clock_*_field() above only ever
/// trusts a fully rendered frame. Day does not wrap (Up on Sun does
/// nothing); hour and minute do, and take the shortest path
/// (direction_wrap_24/60 above). Main is never pressed anywhere in this
/// sequence: on this unit Main is Boost, and the old script says so in as
/// many words.
///
/// If anything is not where it is expected -- wrong screen, an unreadable
/// clock, no time source -- this bails out and leaves the display alone
/// (see VERIFY/CHECK_TIME below); the unit returns to its normal screen on
/// its own two-minute timeout, same as every other sequence in this
/// component that fails before committing anything.
///
/// The fourth (COMMIT) Set is not itself retried or verified the way
/// OpenEditor verifies the first -- SETTLE and EXIT_CHAIN below stand in for
/// that. If COMMIT's Set was dropped, the editor is still open once SETTLE
/// has waited long enough to tell (Display::editor_open(), gated on a
/// window deliberately longer than editor_open()'s own settle window --
/// see SETTLE_MS's own comment), and EXIT_CHAIN runs ExitEditChain to walk
/// it closed with more Sets before LEAVE ever taps Up. LeaveMenu's own TAP
/// step also refuses to tap into an open editor, as a structural backstop
/// -- but EXIT_CHAIN is what actually recovers the run; LeaveMenu merely
/// keeps a recovery failure from making things worse.
///
/// A long-lived hub member, reused on every scheduled or button-triggered
/// run -- see PLAN.md's "no dynamic allocation in steady state". There is no
/// real per-run state to reset in on_start(): nav_started_ms_ is written
/// fresh at NAVIGATE (always step 0 of a new run), and every AdjustField
/// child resets its own guard_count_ in ITS on_start(), called automatically
/// each time this pushes it via await().
class SyncClock final : public Sequence {
 public:
  using LogSink = Keypad::LogSink;

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

  void set_log_sink(LogSink sink) {
    this->log_ = sink;
    // Forwarded, here, rather than every run -- adjust_field_ and
    // exit_chain_ are private members the hub cannot reach directly, same
    // pattern as ReadSettings::set_log_sink/WriteSetting::set_log_sink.
    // adjust_field_ forwarding added stage 7a -- see WriteSetting's own
    // set_log_sink comment for why that one mattered.
    this->adjust_field_.set_log_sink(sink);
    this->exit_chain_.set_log_sink(std::move(sink));
  }
  void set_time_source(TimeSource source) { this->time_source_ = std::move(source); }

  const char *name() const override { return "SyncClock"; }
  Poll poll() override;
  void on_finish(Poll result) override;

  // Worst case: the three guard limits (8+14+34 = 56 taps -- the old
  // script's own numbers, carried into DAY_GUARD/HOUR_GUARD/MINUTE_GUARD
  // below) each up to ~1.35s (AdjustField's own worst case: WAIT_CHANGE's
  // 900ms plus a tap and its mandatory key_gap, ~450ms) -- 56 * 1.35s =~
  // 76s -- plus NAVIGATE's GotoMenu(1) (a handful of seconds, ~4s), VERIFY's
  // 3s budget, OPEN's own worst case (one retry: two Set-tap-plus-700ms-
  // settle cycles, ~2.3s), three plain Set taps advancing/committing the
  // editor (~1.35s), SETTLE's now-1800ms wait (~1.8s -- raised stage 7a from
  // the old script's 700ms; see SETTLE_MS's own comment for why), EXIT_
  // CHAIN's own worst case if the commit Set was dropped and the fourth Set
  // never actually landed (~157s -- ExitEditChain's own 4-commits-then-150s-
  // fallback shape, the same figure ReadSettings'/WriteSetting's own
  // timeout_ms() comments use), and LEAVE's own worst case (a tap plus
  // LeaveMenu::WAIT_TIMEOUT_MS, ~130.5s) if the editor still somehow has not
  // closed by then. Roughly 76 + 4 + 3 + 2.3 + 1.35 + 1.8 + 157 + 130.5 =~
  // 376s -- the old 300s budget no longer has headroom above that, so this
  // is raised to 450s (~74s of headroom), same "comfortable, not cutting it
  // close" reasoning as WriteSetting's own timeout_ms().
  uint32_t timeout_ms() const override { return 450000; }

 private:
  enum Step : uint8_t {
    NAVIGATE,
    VERIFY,
    CHECK_TIME,
    OPEN,
    ADJUST_DAY,
    SET_DAY,
    WAIT_SET_DAY,
    ADJUST_HOUR,
    SET_HOUR,
    WAIT_SET_HOUR,
    ADJUST_MINUTE,
    COMMIT,
    WAIT_COMMIT,
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
  // in this component (PLAN.md risk 6).
  static constexpr int DAY_GUARD = 8;
  static constexpr int HOUR_GUARD = 14;
  static constexpr int MINUTE_GUARD = 34;

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
  LogSink log_;
  TimeSource time_source_;
};

// ------------------------------------------------------------ SetAirflowMode --
// Stage 7's other deliverable (PLAN.md §3's SetAirflowMode row, §6's
// airflow_mode select). Ported from mhrv_orig/controls.yaml's boost_probe/
// boost_normalise/boost_set, extended for Purge per PLAN.md risk 3 -- see
// SetAirflowMode's own class comment (seq_set_airflow_mode.cpp) for the full
// step-by-step reasoning.

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

/// Sets the Main-key boost/purge state to an ABSOLUTE target. Main is a
/// cumulative press counter with no usable timeout (1 press = 30 min boost,
/// 2 = 60 min, 3 = continuous, 4 = back to Normal --
/// mhrv_orig/controls.yaml's own measured table, "Presses 250ms apart are
/// counted individually and correctly"), so "Boost 60" only means the same
/// thing every time if the counter is first normalised back to a known
/// state (Normal) and then advanced by exactly the target's own press count
/// -- CLAUDE.md's "Main is never a menu key... its press counter is
/// cumulative... absolute targeting must normalise first". Purge is a
/// separate axis: a 5.5s Main hold TOGGLES it on or off regardless of where
/// the tap counter sits, so it is handled as its own branch rather than
/// folded into the same counter.
///
/// See seq_set_airflow_mode.cpp for the full step-by-step reasoning behind
/// each state below; in outline:
///  1. CHECK_CURRENT: no keys pressed. First, PROVES the display is
///     actually on the status loop -- have_frame() true and
///     screens::is_menu_screen() false -- and FAILS immediately (no retry
///     window; a menu screen does not clear itself within a sequence's
///     patience) if not. This matters because CLAUDE.md's "Main is never a
///     menu key" applies to being PARKED on a menu/diagnostic screen just
///     as much as to pressing one on purpose: Runner::recover()'s own exit
///     tap does not wait for the unit to actually leave the menu, the
///     unit's own ~2-minute timeout may still be running, and a human at
///     the unit's own keypad can park it on one at any moment -- all
///     reachable start states, not hypothetical ones. Without this check,
///     classify_line()/purging() can never see Purge or Boost from a menu
///     screen, so the probe below would conclude "Normal" with zero
///     evidence and go on to tap Main against a boost counter whose
///     position was never actually read. Second, is the unit currently
///     showing Purge -- via the injected StatusTracker's sticky purging(),
///     not a single frame; see set_status()'s own comment for why. Branches
///     to the Purge hold (direct, no normalising needed -- the manual
///     describes Purge as reachable from any state) or, for a non-Purge
///     target, to CANCEL_PURGE first if currently purging, else straight to
///     the boost probe.
///  2. CANCEL_PURGE / OPEN_PURGE: a fixed 5500ms Main hold (PLAN.md §3),
///     via press()+elapsed() rather than HoldUntil -- its SUCCESS condition
///     is "the timer ran out", not a predicate, the same reasoning
///     FetchDiagnostics::HOLD_DOWN documents for its own fixed 8s hold.
///     CANCEL_PURGE is followed by an explicit ~400ms settle before the
///     first subsequent tap -- CLAUDE.md's "release is silence... a
///     hold-to-hold [or hold-to-tap] transition needs an explicit gap".
///  3. PROBE_CHECK / PROBE_WAIT: mhrv_orig's boost_probe -- sample line1 for
///     "Boost Airflow" right now; if not seen, wait up to 8s for a Boost
///     frame (line1 alternates every ~3.2-3.5s, so a single unlucky sample
///     can catch the other half of the cycle -- PLAN.md §3's "the probe
///     needs up to ~8s to be conclusive").
///  4. If boosting: tap Main once (bounded by a 4-tap guard, PLAN.md §3),
///     wait for the tap and its key_gap to clear, wait a further 1s
///     (mhrv_orig's own figure -- long enough for the unit's own counter
///     and display to have caught up before judging it), then re-probe.
///     Guard-exhausted-and-still-boosting FAILS outright rather than
///     layering the target's own presses on top of an unknown press count
///     -- mirrors mhrv_orig's own boost_set, which silently skipped
///     applying anything in exactly this situation.
///  5. APPLY_TAP / APPLY_WAIT: once confirmed Normal, tap Main exactly as
///     many times as the target needs from there -- 0/1/2/3 for
///     Normal/Boost30/Boost60/Continuous -- queued as a batch and drained
///     via Keypad's own queue, the same batching GotoMenu uses for its own
///     taps (sequence.cpp).
///
/// Accepted edge, chosen deliberately rather than left implicit: purging()'s
/// sticky ALTERNATION_TIMEOUT_MS window (~12s, status.h) means a purge that
/// genuinely ended within the last ~12s still reads as "purging" here, so a
/// PURGE target in that window lands on CHECK_CURRENT's no-op branch and
/// does nothing rather than re-opening it. That is the SAFE direction of
/// this trade -- the alternative (trusting a stale "not purging" instead)
/// risks a wrong-direction 5.5s hold that CANCELS a purge actually still
/// running, in an occupied house. See CHECK_CURRENT's own comment
/// (seq_set_airflow_mode.cpp) for the mirror-image reasoning at PROBE_CHECK,
/// which deliberately does NOT use this same sticky reading.
///
/// NOT optimistic (PLAN.md §6) -- same "the unit's own keypad is also a
/// valid input device" reasoning as WriteSetting: this sequence only ever
/// presses keys, it never publishes anything itself. What Home Assistant's
/// airflow_mode shows comes entirely from the hub's own PASSIVE status-line
/// decode (VentAxiaHub::publish_airflow_mode_(), vent_axia.cpp) -- the same
/// boosting()/purging()/boost_time_remaining() the generic binary
/// sensors/sensors already publish from (status.h) -- so a press at the
/// unit's own physical keypad shows up in HA exactly the way a command from
/// HA does, with no read-back step of this sequence's own needed to make
/// that true. That decode also DELIBERATELY GOES SILENT while a
/// SetAirflowMode run is the active root sequence (VentAxiaHub::
/// publish_airflow_mode_()'s own comment) -- normalising walks the unit
/// through intermediate boost states nobody chose (Boost 30 -> Boost 60 ->
/// continuous -> Normal is one lap of the counter), and PLAN.md risk 3 is
/// explicit that HA is expected to keep showing the OLD value for the
/// ~25-30s a transition can take, with `busy` surfacing that a change is in
/// flight, rather than visibly walking through every intermediate mode.
///
/// A long-lived hub member, reused for every write -- configure() before
/// each request(), see on_start() for what resets between runs.
class SetAirflowMode final : public Sequence {
 public:
  using LogSink = Keypad::LogSink;
  void set_log_sink(LogSink sink) { this->log_ = std::move(sink); }

  /// Finding 2 (Opus review of this stage): the alternation-aware, STICKY
  /// purging() this sequence needs at CHECK_CURRENT to know "is the unit
  /// currently purging" reliably. A single decoded frame is not enough --
  /// the status loop alternates, which is exactly why StatusTracker models
  /// purging_ as a Flag with its own ALTERNATION_TIMEOUT_MS rather than
  /// trusting one frame (status.h's own class comment), and PLAN.md risk 4
  /// (the purge layout is unresolved) makes a single-frame miss MORE
  /// likely, not less. Must be set before request()ing this instance --
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

  // Worst case: CANCEL_PURGE/OPEN_PURGE (5.5s) + CANCEL_SETTLE (0.4s) + the
  // normalise loop's own worst case (4 guard iterations, each up to
  // ~8s probe + ~0.45s tap+key_gap + 1s settle =~ 9.45s -- 4*9.45 =~ 37.8s)
  // + APPLY's own worst case (3 taps, for BOOST_CONTINUOUS, ~1.35s) -- about
  // 45s. Comfortable headroom above that, same "don't cut it close"
  // reasoning as every other
  // timeout_ms() override in this file -- and in practice unreachable, since
  // every RUNNING state in this sequence is already internally bounded (see
  // the class comment), unlike e.g. WriteSetting's VERIFY/OPEN steps which
  // wait on a specific screen that might never arrive.
  uint32_t timeout_ms() const override { return 90000; }

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
  static constexpr uint8_t NORMALISE_GUARD = 4;           // PLAN.md §3

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
  LogSink log_;
  const status::StatusTracker *status_{nullptr};  // Finding 2 -- set_status(), CHECK_CURRENT

  // Per-run stuck-tap tracking (STUCK_TAP_LIMIT above) -- reset in on_start(),
  // NOT here, because this Sequence is a long-lived hub member reused across
  // runs (this file's own Runner class comment): a value left over from a
  // PREVIOUS run's last probe would let that run's history leak into this
  // one's first comparison. last_airflow_percent_/last_countdown_minutes_
  // hold the most recent after_probe_(true) sample (status::parse_line_values(),
  // the same instantaneous read PROBE_CHECK's own defensive re-check uses --
  // see seq_set_airflow_mode.cpp:186-201's asymmetry comment for why this is
  // deliberately NOT status_'s sticky tracker); have_stuck_sample_ is false
  // until the first sample exists, so the very first probe never has anything
  // to compare against and can never itself count as "unmoving".
  std::optional<int> last_airflow_percent_;
  std::optional<int> last_countdown_minutes_;
  bool have_stuck_sample_{false};
  uint8_t stuck_taps_{0};
};

// ------------------------------------------------------------- ResetFilter --
// Stage 7's remaining deliverable, and the last thing built in this
// component by design (PLAN.md §8 puts it last "for exactly that reason"):
// the ONE operation with no way back from software. Ported from
// mhrv_orig/controls.yaml's reset_filter script -- see that file's own
// comment for how the gesture itself (a 5s Up+Down hold on the status
// screen) was actually established: the manual originally looked
// undocumented because its button glyphs are embedded images, not a symbol
// font, and did not survive naive PDF text extraction.

/// Clears the "Check Filter" reminder and restarts the service countdown
/// (6/12/18 months, unit-dependent) by holding Up+Down together for a fixed
/// 5.5s on the status screen -- the same "the protocol is a bitmask, holding
/// two keys at once is fine" fact FetchDiagnostics' own Up+Main entry combo
/// already relies on (mhrv_orig/controls.yaml's own comment, carried forward
/// here). This writes to the unit and restarts the countdown at the full
/// interval; there is no read-back that could undo it.
///
/// Three steps, in outline (see seq_reset_filter.cpp for the full
/// step-by-step reasoning):
///  1. CHECK_STATUS: refuses (no retry -- same reasoning as SetAirflowMode's
///     own CHECK_CURRENT, seq_set_airflow_mode.cpp) unless the display has a
///     frame and is showing the status loop, never a menu or diagnostic
///     screen -- CLAUDE.md's device invariants name this sequence
///     specifically ("Filter reset verifies the status screen first").
///     This is the ONE guard standing between a stray press and an
///     unrecoverable write, so a wrong screen here means refuse outright,
///     not wait-and-see: a menu screen does not clear itself within a
///     sequence's patience.
///  2. HOLD/RELEASE_SETTLE: asserts Up+Down for a fixed 5500ms
///     (press()+elapsed(), not HoldUntil -- same reasoning as
///     SetAirflowMode::hold_main_(): "the timer ran out" is the whole
///     success condition, there is no predicate to watch for), then
///     releases and settles -- long enough to satisfy the "release is
///     silence... hold-to-hold transition needs an explicit gap" invariant
///     before FETCH's own Up+Main entry hold, and to let the display show
///     whatever the unit answers the gesture with, which gets LOGGED and
///     never acted on: this model's manual describes no confirmation
///     prompt, but some other Vent-Axia models are known to answer this
///     same gesture with a "Reset Filter?" prompt needing a second
///     keypress, untested on this unit either way. Firing a guessed confirm
///     press at whatever is on screen would be a worse failure than leaving
///     an unconfirmed reset alone -- if a prompt ever does show up, this log
///     line is where a human first sees it, not a code path that reacts to
///     it.
///  3. FETCH/VERIFY: chains a FetchDiagnostics run (its OWN dedicated
///     instance -- see diagnostics_scan_'s own comment below for why this
///     is NOT the hub's shared button/schedule instance) to re-read every
///     diagnostic page including 23, then checks filter_hours_source_ for
///     FOUR distinguishable outcomes, each logged differently -- extending
///     mhrv_orig's own three-way log with one more case Opus review found
///     it needed (see hours_before_'s own comment for why), because
///     collapsing any of these into "pass" or "fail" would hide followup
///     a human needs:
///       - no reading at all (filter_hours_source_ unset, or answers
///         nullopt) -- cannot confirm either way, WARN.
///       - a reading of exactly 0 -- the reset did not take, WARN.
///       - a reading above 0 but EQUAL to hours_before_ (the pre-hold
///         snapshot) -- unchanged since before the hold, so page 23 was
///         most likely not re-read by THIS run's scan rather than the reset
///         having failed; logged with both numbers, WARN, not INFO --
///         see hours_before_'s own comment for the one case this still
///         cannot tell apart from a genuine no-op reset.
///       - a reading above 0 and DIFFERENT from hours_before_ -- confirmed,
///         the hours logged at INFO.
///     VERIFY never itself returns FAILED on any of these four outcomes:
///     the irreversible hold already happened in step 2 by the time VERIFY
///     runs, so there is nothing left to protect by failing the sequence
///     here -- only information to log, the same "never hard-fail a read"
///     philosophy ReadSettings' own class comment describes. A FAILED
///     result IS still possible for this sequence, but only via FETCH's
///     await() cascading if the chained FetchDiagnostics itself could not
///     complete (e.g. never even reached the diagnostic menu) -- a
///     distinct situation from "reached page 23 and didn't like what it
///     saw".
///
/// A long-lived hub member, reused on every button press -- see PLAN.md's
/// "no dynamic allocation in steady state". on_start() resets hours_before_
/// to the CURRENT filter_hours_source_ reading -- see that member's own
/// comment for why this run needs to remember it -- and step_/entered_
/// reset via goto_step()/push_child_() as always; diagnostics_scan_'s own
/// on_start() (run automatically when FETCH await()s it) resets ITS
/// per-run state (highest_page_seen_/seen_pages_) the same way it does for
/// the button's own instance.
class ResetFilter final : public Sequence {
 public:
  using LogSink = Keypad::LogSink;

  void set_log_sink(LogSink sink) {
    this->log_ = sink;
    // Forwarded once, here -- diagnostics_scan_ is a private member the hub
    // cannot reach directly, same pattern as every other sequence in this
    // file that owns a child (ReadSettings::set_log_sink,
    // WriteSetting::set_log_sink, SyncClock::set_log_sink).
    this->diagnostics_scan_.set_log_sink(std::move(sink));
  }

  /// Wired to diagnostics_scan_'s own SuccessSink -- a genuine full
  /// diagnostic scrape happens inside this sequence exactly as it does for
  /// the button/schedule path, so diagnostics_updated should be stamped the
  /// same way. See VentAxiaHub::stamp_diagnostics_updated_().
  void set_on_diagnostics_success(FetchDiagnostics::SuccessSink sink) {
    this->diagnostics_scan_.set_on_success(std::move(sink));
  }

  /// Read-only view of the last known filter-hours reading (diagnostic page
  /// 23, SensorKey::FILTER_HOURS) -- injected the same way SetAirflowMode::
  /// set_status() and SyncClock::set_time_source() let this file reach
  /// state that only exists on the other side of the portable-core boundary
  /// (README "Portable core"): this file never includes
  /// esphome/components/sensor/sensor.h. The value has to arrive this way
  /// rather than being read off the diagnostics_scan_ child directly
  /// because diagnostic decoding is entirely passive -- driven by the hub's
  /// own on_change callback whenever line1/line2 change while a diagnostic
  /// page is showing, not by whichever sequence (if any) happens to be
  /// scrolling the display through it at the time (FetchDiagnostics' own
  /// class comment: "Decodes nothing itself").
  ///
  /// nullopt means "never published" -- mirrors sensor::Sensor::has_state()
  /// being false, the same "no reading from page 23 at all" case
  /// mhrv_orig/controls.yaml's own reset_filter script checked for
  /// (`!id(filter_hours).has_state()`). Deliberately carries forward that
  /// script's one limitation rather than fixing it here: this is the LAST
  /// known reading, not necessarily one from THIS run's own scan -- if
  /// diagnostics_scan_ genuinely skipped page 23 (a dropped frame
  /// mid-scroll) but an earlier run had already published a nonzero value,
  /// a bare read of this source in VERIFY would see that stale value rather
  /// than "no reading". A scan-scoped signal would need new per-run
  /// tracking state on the hub side, since the decode is shared and passive
  /// rather than owned by whichever sequence is driving the scroll -- a
  /// larger change than this stage's least-invasive route calls for, and
  /// the same tradeoff the reference implementation already made.
  ///
  /// Opus review of this stage (Finding 1) closed most of that gap WITHOUT
  /// touching the hub at all: on_start() snapshots this source's answer
  /// into hours_before_ BEFORE the hold, and VERIFY compares against it --
  /// see that member's own comment. What the snapshot buys: a stale-nonzero
  /// reading (the dangerous direction -- see hours_before_) can no longer
  /// be reported as a fresh confirmation, because "unchanged from before
  /// the hold" is now distinguishable from "genuinely re-read and still the
  /// same". What it does NOT buy: it cannot prove page 23 WAS re-read this
  /// run, only that the value moved (or didn't) -- so a scan that misses
  /// page 23 twice in a row (before AND after the hold) would still read as
  /// "unchanged", not "no reading". Still strictly better than a bare read
  /// of this source, and cheap: one extra int stored per run, no new hub
  /// state.
  using FilterHoursSource = std::function<std::optional<int>()>;
  void set_filter_hours_source(FilterHoursSource source) { this->filter_hours_source_ = std::move(source); }

  const char *name() const override { return "ResetFilter"; }
  void on_start() override;
  Poll poll() override;
  void on_finish(Poll result) override;

  // Worst case: HOLD's fixed 5500ms + RELEASE_SETTLE's fixed 1000ms +
  // diagnostics_scan_'s own worst case AS A CHILD -- which is what actually
  // bounds this, not FetchDiagnostics' own (inherited, unused-as-a-child)
  // timeout_ms(): Sequence::timeout_ms()'s own comment is explicit that it
  // is "only ever consulted for whichever Sequence is currently the ROOT",
  // so only THIS override matters once diagnostics_scan_ is pushed as a
  // child below. FetchDiagnostics' own step timeouts sum to
  // ENTER_TIMEOUT_MS(15000) + ENTER_SETTLE_MS(300) + HOLD_DOWN_MS(8000) +
  // DOWN_SETTLE_MS(400) + TO_PAGE_00_TIMEOUT_MS(20000) +
  // PAGE_00_SETTLE_MS(250) + EXIT_TIMEOUT_MS(15000) = 58950ms (see
  // seq_fetch_diagnostics.cpp). 5500 + 1000 + 58950 =~ 65.5s. Comfortable
  // headroom above that, same "don't cut it close" reasoning as every other
  // timeout_ms() override in this file, so a legitimate full scan -- even
  // one that ultimately fails on ITS OWN internal timeout rather than
  // succeeding -- always gets to finish deciding that for itself instead of
  // being cut off mid-wait by this sequence's own budget expiring first.
  uint32_t timeout_ms() const override { return 120000; }

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

  LogSink log_;
  FilterHoursSource filter_hours_source_;
  // Opus review (Finding 1): filter_hours_source_'s answer taken BEFORE the
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
