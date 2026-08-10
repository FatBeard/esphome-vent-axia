#pragma once

#include <array>
#include <optional>

#include "esphome/components/uart/uart.h"
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

#include "display.h"
#include "entities.h"
#include "protocol.h"
#include "status.h"

namespace esphome {
namespace vent_axia {

/// Hub for a Vent-Axia Sentinel Kinetic MVHR on its wired-remote serial port.
///
/// The component impersonates the wired remote: the MVHR pushes a 41-byte
/// display frame roughly every 300 ms, and a keypress is a stream of 8-byte
/// frames repeated for as long as the key is held (there is no key-up
/// frame). This stage adds status-line decode (status::StatusTracker) and
/// link liveness on top of stage 1's RX framing and display decode/publish;
/// nothing transmits yet except the one-shot alive frame in setup().
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
