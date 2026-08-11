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
// FetchDiagnostics (the one concrete sequence this stage adds) is declared
// here too, alongside the primitives, rather than in its own header -- see
// entities.h for the same "one growing file, not one file per addition"
// choice. seq_fetch_diagnostics.cpp carries only its method bodies; later
// stages add ReadSettings, WriteSetting, SyncClock, SetAirflowMode,
// ResetFilter and ManualKey the same way, each getting its own seq_*.cpp.
//
// Driven entirely by Runner::loop(uint32_t now_ms), the same discipline as
// Keypad (see keypad.h): a Sequence never calls millis() itself, it asks its
// Runner (see Sequence::elapsed()), which is what keeps this file host
// testable with explicit timestamps and no sleeping.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "display.h"
#include "keypad.h"
#include "protocol.h"

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
  explicit GotoMenu(uint8_t index) : index_(index) {}

  const char *name() const override { return "GotoMenu"; }
  Poll poll() override;

 private:
  enum Step : uint8_t { QUEUE_UP, WAIT_UP, SETTLE_UP, QUEUE_DOWN, WAIT_DOWN, SETTLE_DOWN };

  uint8_t index_;
};

/// Exactly ONE Up tap, then -- if the display is still showing a menu screen
/// -- waits out the unit's own ~2-minute menu timeout rather than pressing
/// again. PLAN.md §3: mashing Up would corrupt a setting if an editor is
/// still open (observed on the real unit: a second Up silently walked a
/// 14°C setpoint to 19°C). This is deliberate, measured behaviour, not
/// laziness -- do not "helpfully" retry the tap.
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

}  // namespace vent_axia
}  // namespace esphome
