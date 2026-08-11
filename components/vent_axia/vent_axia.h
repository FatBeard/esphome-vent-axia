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
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif

#include "diagnostics.h"
#include "display.h"
#include "entities.h"
#include "protocol.h"
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

/// Hub for a Vent-Axia Sentinel Kinetic MVHR on its wired-remote serial port.
///
/// The component impersonates the wired remote: the MVHR pushes a 41-byte
/// display frame roughly every 300 ms, and a keypress is a stream of 8-byte
/// frames repeated for as long as the key is held (there is no key-up
/// frame). Stage 2 added status-line decode (status::StatusTracker) and
/// link liveness on top of stage 1's RX framing and display decode/publish.
/// This stage adds the diagnostic page/field table (diagnostics.cpp):
/// entirely passive, driven off whatever page the display happens to be
/// showing -- see publish_diagnostic_page_(). Nothing transmits yet except
/// the one-shot alive frame in setup().
///
/// This is the only file (besides the platform .py files) that may include
/// esphome/... headers -- protocol.h, display.h, screens.h, parser.h,
/// status.h and entities.h are the portable core and are compiled into the
/// host test suite, so they stay framework-free. See README, "Portable
/// core".
class VentAxiaHub : public Component, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

  // Runs LATE so that any entity this hub publishes into is already set up.
  float get_setup_priority() const override { return setup_priority::LATE; }

  void set_read_only(bool read_only) { this->read_only_ = read_only; }

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

  bool read_only_{false};
  protocol::Framer framer_;
  Display display_;
  status::StatusTracker status_;
  bool have_frame_{false};
  uint32_t last_frame_at_ms_{0};
  std::vector<DiagnosticPageTrigger *> diagnostic_page_triggers_;

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

}  // namespace vent_axia
}  // namespace esphome
