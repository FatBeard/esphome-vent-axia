#pragma once

// The sequence engine. Every user-visible operation (fetch diagnostics, sync
// clock, write a setting, set airflow mode, reset filter) is a multi-second,
// multi-step state machine that must not block loop(), must be mutually
// exclusive with every other one (they all fight over one display and one
// keypad), and must release the keypad on every exit path. Plain C++17, no
// ESPHome headers -- see README "Portable core".
//
// Two pieces live here, and two more headers build on them:
//  - Poll / Sequence: one unit of work, pumped by whoever is running it.
//  - Runner: a fixed-depth stack of Sequences, pumped once per loop() tick.
//    Only the sequence on top of the stack runs. A single ROOT sequence at a
//    time is what gives mutual exclusion *structurally* -- the old setup's
//    hand-rolled `ui_busy` global (acquired at 5 sites, released at 12, one
//    miss away from deadlocking the device until reboot) does not get
//    reimplemented here, it stops existing: there is nowhere for a second
//    root sequence to run while one is already on the stack.
//  - sequence_primitives.h: the pieces every concrete sequence is built
//    from -- HoldUntil, GotoMenu, LeaveMenu, and the editing model.
//  - sequences.h: the six concrete sequences themselves.
//
// Driven entirely by Runner::loop(uint32_t now_ms), the same discipline as
// Keypad (see keypad.h): a Sequence never calls millis() itself, it asks its
// Runner (see Sequence::elapsed()), which is what keeps this host testable
// with explicit timestamps and no sleeping.

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
#include "status.h"

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
  /// step_ (see goto_step()); WriteSetting below is the model to match: a
  /// flat list of named steps, each one line where possible.
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

  /// The Runner's own log sink -- see Runner::log(). Every sequence reaches
  /// it through here rather than carrying a LogSink of its own, so there is
  /// exactly one place a sink is ever set (VentAxiaHub::setup() wires only
  /// the Runner) and no forwarding step that a child sequence can be left
  /// out of. NOT valid inside a constructor: runner_ is only assigned by
  /// Runner::push_child_(), before on_start() runs -- see this file's class
  /// comment.
  const Keypad::LogSink &log() const;

  /// Queues one tap -- one menu step -- at the Runner's configured
  /// tap_duration_ms() rather than a hardcoded duration, so that raising it
  /// (keypad.h's remedy for a loop()-stall-dropped press) reaches every menu
  /// tap in every sequence and not just the four manual key buttons.
  /// Completes at `next_step` once the keypad is idle again -- including the mandatory key_gap, so a caller chaining
  /// taps needs no delay step of its own. Returns Poll::FAILED if the tap
  /// was refused by the Set interlock (Runner::tap()), same as every
  /// hand-rolled tap-then-wait pair this replaces.
  Poll tap_then_(protocol::KeyMask mask, uint8_t next_step);

  Runner *runner_{nullptr};
  uint8_t step_{0};
  uint32_t entered_{0};

 private:
  friend class Runner;

  // Reserved step value meaning "a tap_then_() call is in flight", handled
  // by pump() below before any derived poll() ever sees step_ take this
  // value -- no sequence in this file uses a Step enum anywhere near 255
  // (verified), so this is safe to reserve unconditionally rather than
  // needing per-sequence coordination.
  static constexpr uint8_t WAIT_TAP_STEP = 255;

  /// What Runner::loop() actually pumps, instead of poll() directly:
  /// handles WAIT_TAP_STEP itself (RUNNING while the keypad is still busy,
  /// otherwise resumes at tap_resume_step_) and delegates everything else
  /// straight to the derived poll(). Kept out of poll() itself so every
  /// sequence's own switch stays a flat, complete list of ONLY its own
  /// named steps -- a derived class never needs a default: case that knows
  /// WAIT_TAP_STEP exists.
  Poll pump();

  uint8_t tap_resume_step_{0};  // where tap_then_() resumes once the keypad goes idle -- see pump()
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
  /// backs YAML's on_sequence_failed. Not fired for a failure that a
  /// parent's await() would have survived, and not fired for a child
  /// failing on its own -- only ever for the whole run.
  using FailureSink = std::function<void(const std::string &name)>;

  /// Keypad is mutated (tap/press/release); Display is only ever read. Both
  /// outlive the Runner -- the hub owns all three as long-lived members.
  Runner(Keypad &keypad, const Display &display) : keypad_(keypad), display_(display) {}

  void set_log_sink(LogSink sink) { this->log_ = std::move(sink); }
  const LogSink &log() const { return this->log_; }
  void set_on_sequence_failed(FailureSink sink) { this->on_failure_ = std::move(sink); }

  /// Fed by the hub every tick from its own link_up computation --
  /// request() refuses to start anything while this is false, same
  /// reasoning as refusing a second root: driving the keypad at a unit that
  /// has stopped talking to us cannot possibly succeed and would just run
  /// out the clock on a timeout instead.
  void set_link_up(bool up) { this->link_up_ = up; }

  /// The duration every sequence's own menu/field/commit taps use (via
  /// Sequence::tap_then_() and the handful of batch taps that queue several
  /// at once -- GotoMenu, SetAirflowMode::APPLY_TAP) -- forwarded here from
  /// VentAxiaHub::set_tap_duration_ms(), which also keeps its own copy for
  /// the four manual key buttons and the vent_axia.tap_key action. Defaults
  /// to 50ms so behaviour is unchanged when YAML does not override
  /// tap_duration -- keypad.h's under_emitting_presses()'s documented remedy
  /// for a loop()-stall-dropped press ("raise tap_duration to 100ms")
  /// previously only reached those four buttons; this is what lets it reach
  /// every sequence tap too, including GotoMenu/LeaveMenu and
  /// Runner::recover()'s own exit tap.
  void set_tap_duration_ms(uint32_t ms) { this->tap_duration_ms_ = ms; }
  uint32_t tap_duration_ms() const { return this->tap_duration_ms_; }

  /// Starts `seq` as a new root sequence. Refuses (logging which sequence is
  /// blocking, or that the link is down) rather than queuing or interrupting
  /// -- a single root at a time is what makes mutual exclusion structural,
  /// see this file's class comment.
  bool request(Sequence &seq);

  /// Pumps whichever Sequence is on top of the stack, exactly once. Also
  /// enforces the per-root timeout (see Sequence::timeout_ms()) and a
  /// mid-run link drop (link_up_ going false while depth_ > 0 -- request()
  /// only ever checks it before STARTING a run, see request()'s own
  /// comment), aborting the whole run and calling recover() on either --
  /// see finish_top_().
  void loop(uint32_t now_ms);

  /// True whenever a root sequence is in progress, whatever depth it has
  /// pushed children to. Backs the hub's BUSY binary sensor alongside
  /// Keypad::busy().
  bool busy() const { return this->depth_ > 0; }

  /// The root sequence's name while busy(), or "" -- for logging and the
  /// hub's dump_config(), not meant to be published as an entity itself.
  const char *running_name() const { return this->busy() ? this->stack_[0].seq->name() : ""; }

  /// True when `seq` is the ROOT of the current run -- pointer identity, so
  /// it cannot be defeated by two sequences sharing a name. Lets a caller
  /// (e.g. VentAxiaHub::publish_airflow_mode_()) special-case "this specific
  /// sequence is running" without the false positive a name comparison
  /// would risk if two distinct Sequence instances ever shared a name()
  /// literal.
  bool is_running(const Sequence *seq) const { return this->busy() && this->stack_[0].seq == seq; }

  uint32_t now_ms() const { return this->now_ms_; }
  const Display &display() const { return this->display_; }

  /// The single choke point every Sequence primitive uses to reach the
  /// keypad -- there is deliberately no plain accessor for the Keypad
  /// itself, so a primitive cannot bypass what these two enforce. Same
  /// interlock VentAxiaHub::tap_key()/hold_key() enforce for every other
  /// caller: refused, and logged, if `mask` includes Set while the display
  /// is currently showing a diagnostic page (page 27, "Reset", writes and
  /// has never been tried). Returns false when refused, so a primitive can
  /// fail fast -- see tap_then_()/HoldUntil::poll() -- rather than wait out
  /// a timeout for a press that was never going to happen. This is also
  /// what makes the interlock host-testable at all: it used to live only in
  /// vent_axia.cpp, which the host test suite cannot compile (see README
  /// "Portable core"), so it was asserted only by convention, not in the
  /// test suite itself.
  bool tap(protocol::KeyMask mask, uint32_t duration_ms);
  bool press(protocol::KeyMask mask);

  /// Never interlocked -- releasing a key is always safe, refusing it never
  /// is (same reasoning as VentAxiaHub::release_keys()).
  void release() { this->keypad_.release(); }
  bool keypad_busy() const { return this->keypad_.busy(); }

  /// The shared abort path, run automatically whenever a root sequence
  /// finishes as FAILED (including via timeout) -- see
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
  /// refuse_if_set_interlocked_() -- moved here so it is enforced for every
  /// path that can reach the keypad, not only the pre-sequence ones
  /// (button.py's key_* buttons, the vent_axia.tap_key/hold_key actions),
  /// which now go through tap()/press() above too rather than duplicating
  /// the check.
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
  // Default matches the hub's own tap_duration_ms_ default (vent_axia.h) so
  // behaviour is unchanged unless YAML overrides tap_duration.
  uint32_t tap_duration_ms_{50};
};

}  // namespace vent_axia
}  // namespace esphome
