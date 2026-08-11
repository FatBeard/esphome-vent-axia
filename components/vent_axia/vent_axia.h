#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "esphome/components/uart/uart.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"

// Every entity platform is optional: a config may ask for sensors and no text
// sensors, or none at all. ESPHome only compiles a platform's sources when
// some config actually uses it, so including its header unconditionally
// breaks any build that leaves the platform out -- which is exactly how the
// ESP-IDF example (deliberately minimal, no entities) caught this. The
// storage and the setters are guarded here; the publish_* helpers below
// become no-ops when a platform is absent, so the decode path that calls
// them needs no guards of its own.
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_BUTTON
#include "esphome/components/button/button.h"
#endif
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
// Optional exactly like the entity platforms above, same reasoning: USE_TIME
// is only defined when a config actually has a `time:` block. time_id itself
// is likewise optional on the hub schema (__init__.py) -- see
// stamp_diagnostics_updated_()'s comment for why a missing one is not an
// error, just a skipped stamp.
#ifdef USE_TIME
#include "esphome/components/time/real_time_clock.h"
#endif

#include "diagnostics.h"
#include "display.h"
#include "entities.h"
#include "keypad.h"
#include "protocol.h"
#include "sequence.h"
#include "status.h"

namespace esphome {
namespace vent_axia {

/// Fires once per diagnostic page the display passes through -- decoded by
/// diagnostics.cpp's table or not -- with the page number and raw line2, so
/// a YAML lambda can act on a page nobody has taught the component to
/// decode yet without a component change. Just the Trigger<> base with no
/// extra behaviour: the hub calls ->trigger(page, line2) directly (see
/// publish_diagnostic_page_() below), the same as any other ESPHome trigger.
class DiagnosticPageTrigger : public Trigger<uint8_t, std::string> {};

/// Fires once a ROOT sequence (Runner) finishes as FAILED, with its name --
/// PLAN.md §5's on_sequence_failed. Wired from Runner::FailureSink in
/// setup(), the same one-directional adapter shape as DiagnosticPageTrigger
/// above.
class SequenceFailedTrigger : public Trigger<std::string> {};

/// Hub for a Vent-Axia Sentinel Kinetic MVHR on its wired-remote serial port.
///
/// The component impersonates the wired remote: the MVHR pushes a 41-byte
/// display frame roughly every 300 ms, and a keypress is a stream of 8-byte
/// frames repeated for as long as the key is held (there is no key-up
/// frame). Stage 2 added status-line decode (status::StatusTracker) and
/// link liveness on top of stage 1's RX framing and display decode/publish.
/// Stage 3 added the diagnostic page/field table (diagnostics.cpp): entirely
/// passive, driven off whatever page the display happens to be showing --
/// see publish_diagnostic_page_(). Stage 4 added Keypad and was the first
/// that transmitted anything beyond the one-shot alive frame in setup():
/// tap_key()/hold_key()/release_keys() are the primitive the sequence
/// engine drives. This stage (5) adds that engine (sequence.h's Runner) and
/// its first concrete sequence, FetchDiagnostics -- fetch_diagnostics()
/// below is the only new entry point a caller (button.py, the
/// vent_axia.fetch_diagnostics action) needs. Every caller of Set still
/// goes through the same interlock, now enforced in Runner::tap()/press()
/// (sequence.h) rather than a hub-private method, so the sequence engine's
/// own primitives are not exempt from it either -- see Runner::tap()'s
/// comment.
///
/// This is the only file (besides the platform .py files) that may include
/// esphome/... headers -- protocol.h, display.h, screens.h, parser.h,
/// status.h, keypad.h, sequence.h and entities.h are the portable core and
/// are compiled into the host test suite, so they stay framework-free. See
/// README, "Portable core".
class VentAxiaHub : public Component, public uart::UARTDevice {
 public:
  // Runner holds references to keypad_ and display_, which is why this hub
  // needs an explicit constructor at all -- see their declarations below for
  // why the ordering there matters.
  VentAxiaHub() : runner_(this->keypad_, this->display_) {}

  void setup() override;
  void loop() override;
  void dump_config() override;

  // Runs LATE so that any entity this hub publishes into is already set up.
  float get_setup_priority() const override { return setup_priority::LATE; }

  // Also mutes the keypad -- see setup(), which wires read_only_ into
  // keypad_ via Keypad::set_read_only(). One flag, one meaning: "this
  // firmware never transmits", covering both the alive frame and every key
  // press.
  void set_read_only(bool read_only) { this->read_only_ = read_only; }

  // Timing constants (PLAN.md §2's table), all forwarded straight into
  // keypad_ except tap_duration_ms_, which Keypad has no notion of -- tap()
  // takes an explicit duration per call, so this hub-level default is what
  // the key_* buttons and an omitted `duration` on vent_axia.tap_key fall
  // back to.
  void set_tap_duration_ms(uint32_t ms) { this->tap_duration_ms_ = ms; }
  uint32_t tap_duration_ms() const { return this->tap_duration_ms_; }
  void set_tx_interval_ms(uint32_t ms) { this->keypad_.set_tx_interval_ms(ms); }
  void set_key_gap_ms(uint32_t ms) { this->keypad_.set_key_gap_ms(ms); }
  void set_key_watchdog_ms(uint32_t ms) { this->keypad_.set_key_watchdog_ms(ms); }

  /// Queues a tap through the keypad. Delegates straight to
  /// runner_.tap(), which is now the single choke point for the Set
  /// interlock (PLAN.md §7) -- see Runner::tap()'s comment; this stage moved
  /// the check there from a hub-private method so it applies to the
  /// sequence engine's own primitives too, not only pre-sequence callers.
  /// Used by button.py's KeypadButton and the vent_axia.tap_key action.
  void tap_key(protocol::KeyMask mask, uint32_t duration_ms);

  /// Asserts and holds a mask through the keypad, same interlock and same
  /// reasoning as tap_key(). Used by the vent_axia.hold_key action.
  void hold_key(protocol::KeyMask mask);

  /// Releases whatever the keypad is currently asserting. Never
  /// interlocked -- releasing a key is always safe, refusing it never is.
  void release_keys() { this->keypad_.release(); }

  /// Starts FetchDiagnostics as a root sequence, or refuses (logged inside
  /// Runner::request() -- see sequence.h) if one is already running or the
  /// link is down. Used by button.py's FetchDiagnosticsButton and the
  /// vent_axia.fetch_diagnostics action; mhrv.yaml schedules it daily at
  /// 04:30 (PLAN.md §6).
  void fetch_diagnostics() { this->runner_.request(this->fetch_diagnostics_); }

#ifdef USE_TIME
  /// Optional: without a time_id, stamp_diagnostics_updated_() simply skips
  /// the stamp rather than failing -- see its comment.
  void set_time_id(time::RealTimeClock *rtc) { this->time_ = rtc; }
#endif

  void register_sequence_failed_trigger(SequenceFailedTrigger *trig) {
    this->sequence_failed_triggers_.push_back(trig);
  }

#ifdef USE_TEXT_SENSOR
  void set_text_sensor(TextKey key, text_sensor::TextSensor *sensor) {
    this->text_sensors_[static_cast<size_t>(key)] = sensor;
  }
#endif
#ifdef USE_SENSOR
  void set_sensor(SensorKey key, sensor::Sensor *sensor) { this->sensors_[static_cast<size_t>(key)] = sensor; }
#endif
#ifdef USE_BINARY_SENSOR
  void set_binary_sensor(BinaryKey key, binary_sensor::BinarySensor *sensor) {
    this->binary_sensors_[static_cast<size_t>(key)] = sensor;
  }
#endif

  void register_diagnostic_page_trigger(DiagnosticPageTrigger *trig) {
    this->diagnostic_page_triggers_.push_back(trig);
  }

 protected:
  void publish_text_(TextKey key, const std::string &value);
  /// No-op (and does not update the "last published" cache) when `value` is
  /// nullopt: the tracker not knowing the answer yet must not be published
  /// as a guess. Also skips a republish when the value hasn't changed, since
  /// frames arrive at ~3 Hz and most of them carry no new information for
  /// any given entity.
  void publish_sensor_(SensorKey key, std::optional<int> value);
  void publish_binary_(BinaryKey key, std::optional<bool> value);

  /// Feeds every status-screen-derived entity from the current state of
  /// display_ and status_. Called once per decoded frame.
  void publish_status_();

  /// Passive diagnostic decode (PLAN.md §4): called whenever line1 or line2
  /// changes while display_.screen_kind() == DIAGNOSTIC, with the page
  /// number screens::diagnostic_page() read off line1 and the current
  /// line2. Publishes the raw "NN: <line2>" text sensor and fires
  /// on_diagnostic_page unconditionally -- for every page, decoded by the
  /// table or not -- then runs diagnostics::decode_page() for whatever the
  /// table does understand. Gated on a line actually changing (the same
  /// dedup Display already does for display_line_1/2) rather than firing on
  /// every ~300ms frame regardless of content, so sitting still on one page
  /// does not spam identical publishes and trigger firings.
  void publish_diagnostic_page_(uint8_t page, const std::string &line2);

  /// Page 23's filter_change_due, reconciled against the live status-line
  /// source -- see diagnostics::Sink::report_filter_change_due's comment.
  /// "Live wins by recency": published from the diagnostic reading only
  /// when the live tracker has no opinion yet; otherwise this purely
  /// cross-checks and logs a disagreement (PLAN.md risk 7), it never
  /// overrides a value the live status line is already publishing.
  void reconcile_filter_change_due_(bool due_from_page23);

  /// link_up ages out after LINK_TIMEOUT_MS with no CRC-valid frame -- see
  /// PLAN.md §7 "Link loss". Unlike the status-screen entities this is
  /// always a known value (false before the first frame, not "unknown"), so
  /// it is checked every loop() tick rather than only when a frame lands,
  /// or a dead link would simply never be reevaluated.
  static constexpr uint32_t LINK_TIMEOUT_MS = 30000;
  void publish_link_up_(uint32_t now_ms);

  /// Called from FetchDiagnostics' SuccessSink after a successful run. No-op
  /// (not an error) when time_ is null -- PLAN.md §5 explicitly wants
  /// time_id optional, so a config that leaves it out simply never gets a
  /// timestamp rather than failing validation or logging a complaint every
  /// run.
  void stamp_diagnostics_updated_();

  bool read_only_{false};
  protocol::Framer framer_;
  Display display_;
  status::StatusTracker status_;
  bool have_frame_{false};
  uint32_t last_frame_at_ms_{0};
  bool link_up_{false};  // mirrors what publish_link_up_ just published -- see loop()
  std::vector<DiagnosticPageTrigger *> diagnostic_page_triggers_;
  std::vector<SequenceFailedTrigger *> sequence_failed_triggers_;

  // Declaration order matters here: runner_'s constructor (VentAxiaHub's own,
  // see above) binds references to keypad_ and display_, which must already
  // be constructed -- C++ initialises members in declaration order regardless
  // of initializer-list order, so both must stay ABOVE runner_.
  Keypad keypad_;
  uint32_t tap_duration_ms_{50};  // PLAN.md §2's table: "one tap = one menu step"
  Runner runner_;
  FetchDiagnostics fetch_diagnostics_;  // long-lived -- no dynamic allocation in steady state, PLAN.md §2

#ifdef USE_TIME
  time::RealTimeClock *time_{nullptr};
#endif

#ifdef USE_TEXT_SENSOR
  std::array<text_sensor::TextSensor *, static_cast<size_t>(TextKey::COUNT)> text_sensors_{};
#endif
#ifdef USE_SENSOR
  std::array<sensor::Sensor *, static_cast<size_t>(SensorKey::COUNT)> sensors_{};
#endif
#ifdef USE_BINARY_SENSOR
  std::array<binary_sensor::BinarySensor *, static_cast<size_t>(BinaryKey::COUNT)> binary_sensors_{};
#endif

  // "Last published" caches so publish_sensor_/publish_binary_ can skip a
  // republish when nothing actually changed -- see their comments. Separate
  // from status_'s own state because status_ is the portable core and must
  // not know about ESPHome entities or publish cadence.
  std::array<std::optional<int>, static_cast<size_t>(SensorKey::COUNT)> last_sensor_value_{};
  std::array<std::optional<bool>, static_cast<size_t>(BinaryKey::COUNT)> last_binary_value_{};
};

#ifdef USE_BUTTON
/// One of the four raw key buttons (button.py's key_up/key_down/key_set/
/// key_main): pressing it queues a tap_duration_ms tap of a fixed mask
/// through the hub's tap_key(), the same arbitration point (including the
/// Set interlock) as everything else that presses a key.
///
/// Deliberately a momentary button, not the old setup's hold-switch -- see
/// PLAN.md §6: a switch restored on at boot (ESPHome's default restore
/// behaviour for a switch) could come back holding a key down forever after
/// a power cycle. A button's press_action() only ever queues one bounded
/// tap, so there is nothing for a reboot to leave stuck.
class KeypadButton final : public button::Button, public Parented<VentAxiaHub> {
 public:
  void set_mask(protocol::KeyMask mask) { this->mask_ = mask; }

 protected:
  void press_action() override { this->parent_->tap_key(this->mask_, this->parent_->tap_duration_ms()); }

  protocol::KeyMask mask_{0};
};

/// Starts a Runner root sequence rather than tapping a raw key --
/// press_action() ends up at VentAxiaHub::fetch_diagnostics(), which refuses
/// (and logs why) if another sequence is already running or the link is
/// down, same as every other way of starting one. Only fetch_diagnostics
/// uses this shape so far; ReadSettings/SyncClock/ResetFilter (stages 6-7)
/// are expected to reuse it rather than growing a parallel pattern.
class FetchDiagnosticsButton final : public button::Button, public Parented<VentAxiaHub> {
 protected:
  void press_action() override { this->parent_->fetch_diagnostics(); }
};
#endif

/// vent_axia.tap_key / vent_axia.hold_key / vent_axia.release_keys (PLAN.md
/// §5 "Actions for YAML"). Deliberately plain fields resolved once at
/// codegen time rather than TEMPLATABLE_VALUE: the mask (and, for tap_key,
/// the duration) come straight out of the YAML action config the same way
/// button.py's KeypadButton::mask_ does, and there is no use case yet for a
/// lambda-computed key combination -- TEMPLATABLE_VALUE's extra plumbing on
/// the Python side would be ceremony without payoff for arguments this
/// static.
template<typename... Ts> class TapKeyAction final : public Action<Ts...> {
 public:
  TapKeyAction(VentAxiaHub *parent, protocol::KeyMask mask, uint32_t duration_ms)
      : parent_(parent), mask_(mask), duration_ms_(duration_ms) {}

  void play(const Ts &.../*unused*/) override {
    // duration_ms_ == 0 means the YAML action left `duration` out -- fall
    // back to the hub's own tap_duration, same as the key_* buttons.
    const uint32_t duration = this->duration_ms_ != 0 ? this->duration_ms_ : this->parent_->tap_duration_ms();
    this->parent_->tap_key(this->mask_, duration);
  }

 protected:
  VentAxiaHub *parent_;
  protocol::KeyMask mask_;
  uint32_t duration_ms_;
};

template<typename... Ts> class HoldKeyAction final : public Action<Ts...> {
 public:
  HoldKeyAction(VentAxiaHub *parent, protocol::KeyMask mask) : parent_(parent), mask_(mask) {}
  void play(const Ts &.../*unused*/) override { this->parent_->hold_key(this->mask_); }

 protected:
  VentAxiaHub *parent_;
  protocol::KeyMask mask_;
};

template<typename... Ts> class ReleaseKeysAction final : public Action<Ts...> {
 public:
  explicit ReleaseKeysAction(VentAxiaHub *parent) : parent_(parent) {}
  void play(const Ts &.../*unused*/) override { this->parent_->release_keys(); }

 protected:
  VentAxiaHub *parent_;
};

/// vent_axia.fetch_diagnostics -- same shape as ReleaseKeysAction above, no
/// arguments beyond the hub id. See VentAxiaHub::fetch_diagnostics().
template<typename... Ts> class FetchDiagnosticsAction final : public Action<Ts...> {
 public:
  explicit FetchDiagnosticsAction(VentAxiaHub *parent) : parent_(parent) {}
  void play(const Ts &.../*unused*/) override { this->parent_->fetch_diagnostics(); }

 protected:
  VentAxiaHub *parent_;
};

}  // namespace vent_axia
}  // namespace esphome
