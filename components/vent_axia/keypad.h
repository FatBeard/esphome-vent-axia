#pragma once

// Key mask assertion, TX cadence, the tap/hold queue and the release
// watchdog (PLAN.md §2 "Keypad"). Plain C++17, no ESPHome headers -- see
// README "Portable core". This is the first stage that transmits anything
// to the real unit: every frame the firmware sends from here on passes
// through this class, and the constants below were paid for with debugging
// on the live MVHR (PLAN.md §2's timing table) -- treat them as spec.
//
// Driven entirely by loop(uint32_t now_ms), never by calling millis()
// itself: press()/release()/tap() only record *what* was asked for, and the
// *when* is always resolved against the now_ms given to the next loop()
// call. That is what makes the whole state machine host-testable with
// explicit timestamps and no sleeping.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "protocol.h"

namespace esphome {
namespace vent_axia {

/// Asserts/holds/taps a protocol::KeyMask and retransmits it while it is
/// asserted, exactly as the genuine wired remote does (a keypress is the
/// 8-byte key frame repeated for as long as the key is held; a release is
/// silence, there is no key-up frame).
class Keypad {
 public:
  /// Where frames go. The hub points this at a lambda that writes to the
  /// UART; the host tests point it at a lambda that records what would have
  /// been sent. Never called when read_only() -- see set_read_only().
  using FrameSink = std::function<void(const uint8_t *data, size_t len)>;

  /// Portable logging hook, same reasoning as FrameSink: this file must not
  /// depend on ESPHome's logger, but several events here (PLAN.md risk 1,
  /// §2's watchdog, §7's read_only) are meant to be loud, not silent. Each
  /// member is optional -- an unset one is simply skipped, which is
  /// convenient for tests that only care about one severity, or none. The
  /// hub wires these to ESP_LOGI/ESP_LOGW/ESP_LOGE.
  ///
  /// debug added for frame_logger.h's FrameLogger, the only portable-core
  /// user of this severity: its lines are deliberately chatty (a raw frame
  /// arrives ~3.3 times a second) and were ESP_LOGD before that class moved
  /// out of vent_axia.cpp. Wiring them to warn or error instead would
  /// promote debug spam to a severity mhrv.yaml's log level shows by
  /// default; this keeps them at DEBUG, which mhrv.yaml does run at, so
  /// behaviour is unchanged.
  struct LogSink {
    std::function<void(const std::string &)> info;
    std::function<void(const std::string &)> warn;
    std::function<void(const std::string &)> error;
    std::function<void(const std::string &)> debug;
  };

  /// Ring-buffer size for queued taps, not a std::vector: a std::vector can
  /// grow unboundedly if some stuck automation calls tap() in a loop, and
  /// nothing here should be able to do that. 8 is plenty for any real
  /// sequence (the old YAML config's equivalent, `max_runs`, used 10) -- see
  /// tap()'s comment for what happens when it fills up.
  static constexpr uint8_t QUEUE_CAPACITY = 8;

  void set_frame_sink(FrameSink sink) { this->frame_sink_ = std::move(sink); }
  void set_log_sink(LogSink sink) { this->log_ = std::move(sink); }

  /// Mutes transmission without changing anything else about how this class
  /// behaves: the state machine still advances on the configured timing,
  /// busy() still reports it, the under-emit counter below still counts --
  /// only the actual FrameSink call is skipped. This is what lets the
  /// production firmware be soak-tested against the live unit with the
  /// keypad muted (PLAN.md §7, §8 stage 1): the exact code path runs, it
  /// just never reaches the wire. Each suppressed press is logged (at least
  /// once, not once per retransmit) so a soak test can confirm silence at
  /// the unit rather than just trusting the flag.
  void set_read_only(bool read_only) { this->read_only_ = read_only; }
  bool read_only() const { return this->read_only_; }

  /// Retransmit cadence while a mask is asserted. Default 20ms -- the old
  /// implementation used a hardware timer ISR at 26/28ms; PLAN.md §2.1
  /// explains why that is gone in favour of a loop()-driven comparison.
  void set_tx_interval_ms(uint32_t ms) { this->tx_interval_ms_ = ms; }
  uint32_t tx_interval_ms() const { return this->tx_interval_ms_; }

  /// Enforced silence after every tap, before the next queued tap (or a
  /// fresh hold) may start. Default 400ms. DO NOT LOWER: at 250ms roughly
  /// one press in ten was dropped on the real unit, and a dropped Set fails
  /// to open an editor -- the Up presses that follow then walk *back up*
  /// the menu instead of adjusting a value. At 50ms the unit sees the
  /// release but does not count the next press at all.
  void set_key_gap_ms(uint32_t ms) { this->key_gap_ms_ = ms; }
  uint32_t key_gap_ms() const { return this->key_gap_ms_; }

  /// Hard release of any asserted mask, no matter what any caller believes,
  /// after this many continuous milliseconds. Default 30s. The old setup
  /// needed four separate YAML watchdog scripts because a restored-on
  /// switch could hold a key forever; this replaces all four with one
  /// unconditional check that runs regardless of state (see loop()).
  void set_key_watchdog_ms(uint32_t ms) { this->key_watchdog_ms_ = ms; }
  uint32_t key_watchdog_ms() const { return this->key_watchdog_ms_; }

  /// Asserts `mask` and holds it until release() or the watchdog fires --
  /// what the (later) sequence engine uses for long holds: 8s Down to
  /// scroll diagnostics, 5.5s Main for purge, 5.5s Up+Down for filter
  /// reset. Pre-empts anything queued or already in progress: a fresh hold
  /// request means "whatever was happening before no longer matters",
  /// exactly like release() (see hard_release_()).
  void press(protocol::KeyMask mask);

  /// Hard stop: the asserted mask goes to zero (transmission simply stops --
  /// there is no key-up frame to send) and anything queued is dropped. This
  /// is an escape hatch for "stop everything now", not a graceful
  /// finish-what's-queued-first primitive; the watchdog reaches for the
  /// same logic internally when it fires.
  void release();

  /// Queues a tap: assert `mask` for `duration_ms`, then go silent for
  /// key_gap_ms() before the next queued tap (or a fresh hold) can start.
  /// Queued rather than applied synchronously so a caller issuing several
  /// taps back to back (e.g. a sequence stepping a menu) needs no timer of
  /// its own -- see busy().
  ///
  /// Ring-buffered at QUEUE_CAPACITY rather than grown on demand (see the
  /// class comment). If the queue is already full when this is called, the
  /// new tap is the one dropped, and it is logged as a warning: a caller
  /// stuck enough to overflow an 8-deep queue should lose its newest ask,
  /// not have older, already-committed-to taps bumped out from under it.
  /// This is a deliberate, documented choice, not a silent cap.
  void tap(protocol::KeyMask mask, uint32_t duration_ms);

  /// True while a tap (including its trailing gap) or a hold is in
  /// progress, or a hold/tap has been requested but loop() has not yet run
  /// to start it. Callers wait on this rather than running a timer of their
  /// own -- see the class comment on why the *when* only exists inside
  /// loop().
  bool busy() const;

  /// Drives every timing decision in this class off `now_ms`. Must be
  /// called often (every ESPHome loop() tick) with the real clock -- never
  /// with a value this class invents itself, which is what keeps it host
  /// testable.
  void loop(uint32_t now_ms);

  /// Presses that emitted fewer than 2 frames before their tap duration
  /// elapsed -- PLAN.md risk 1's diagnostic for a loop() stall landing
  /// inside a short tap. Not prevented, just made visible: dump_config()
  /// reports this, and if it ever climbs in practice the documented remedy
  /// is raising tap_duration to 100ms (the unit's own repeat threshold is
  /// comfortably above 260ms, so there is headroom). Only evaluated for
  /// taps that run to their natural end, not one cut short by press() or
  /// release() pre-empting it.
  uint32_t under_emitting_presses() const { return this->under_emitting_presses_; }

  /// Times the watchdog has force-released a stuck mask. Expected to stay
  /// at 0 in normal operation -- anything else means some caller left a key
  /// asserted for 30s, which the watchdog treats as a bug, not routine
  /// operation.
  uint32_t watchdog_releases() const { return this->watchdog_releases_; }

  /// Taps dropped for arriving while the queue was already full -- see
  /// tap()'s comment.
  uint32_t dropped_taps() const { return this->dropped_taps_; }

 private:
  enum class State : uint8_t {
    IDLE,
    HOLD,       // press() is in effect; stays until release() or the watchdog
    TAP_PRESS,  // a queued tap is asserted, counting down to its duration
    TAP_GAP,    // the mandatory silence after a tap, counting down to key_gap_ms_
  };

  struct QueuedTap {
    protocol::KeyMask mask{0};
    uint32_t duration_ms{0};
  };

  /// Drops back to IDLE with the mask cleared and the queue emptied. Shared
  /// by release(), the watchdog and a fresh press() pre-empting whatever
  /// came before -- all three mean "stop everything, right now".
  /// Stops whatever is currently asserted without touching the tap queue.
  void stop_assertion_();
  void clear_queue_();
  void hard_release_();

  QueuedTap pop_queued_tap_();

  /// Sends a frame if the asserted mask requires one this tick: immediately
  /// on any 0->nonzero or value change (PLAN.md risk 1 -- see the class
  /// comment), otherwise every tx_interval_ms_ while it stays asserted, and
  /// never while asserted_mask_ == 0. Silence *is* the release.
  void maybe_transmit_(uint32_t now_ms);

  FrameSink frame_sink_;
  LogSink log_;
  bool read_only_{false};

  uint32_t tx_interval_ms_{20};
  uint32_t key_gap_ms_{400};
  uint32_t key_watchdog_ms_{30000};

  State state_{State::IDLE};
  protocol::KeyMask asserted_mask_{0};
  uint32_t phase_started_ms_{0};       // start of the current TAP_PRESS/TAP_GAP phase
  uint32_t current_tap_duration_ms_{0};
  uint32_t press_started_ms_{0};       // start of the current *continuous* assertion, for the watchdog
  uint32_t frames_this_episode_{0};    // frames sent since asserted_mask_ last went 0->nonzero

  // Transmission bookkeeping, separate from the state machine above: what
  // was last actually (or, in read_only mode, notionally) sent.
  bool have_tx_mask_{false};
  protocol::KeyMask last_tx_mask_{0};
  uint32_t last_tx_ms_{0};

  // press()/release() only ever set these flags; loop() is the only place
  // that turns a request into a timestamped state change -- see the class
  // comment.
  bool pending_release_{false};
  bool have_pending_hold_{false};
  protocol::KeyMask pending_hold_mask_{0};

  std::array<QueuedTap, QUEUE_CAPACITY> queue_{};
  uint8_t queue_head_{0};
  uint8_t queue_count_{0};

  uint32_t under_emitting_presses_{0};
  uint32_t watchdog_releases_{0};
  uint32_t dropped_taps_{0};
};

}  // namespace vent_axia
}  // namespace esphome
