#pragma once

#include <array>

#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/uart/uart.h"
#include "esphome/core/component.h"

#include "display.h"
#include "entities.h"
#include "protocol.h"

namespace esphome {
namespace vent_axia {

/// Hub for a Vent-Axia Sentinel Kinetic MVHR on its wired-remote serial port.
///
/// The component impersonates the wired remote: the MVHR pushes a 41-byte
/// display frame roughly every 300 ms, and a keypress is a stream of 8-byte
/// frames repeated for as long as the key is held (there is no key-up
/// frame). This stage wires up RX framing and display decode/publish only;
/// nothing transmits yet except the one-shot alive frame in setup().
///
/// This is the only file (besides the platform .py files) that may include
/// esphome/... headers -- protocol.h, display.h, screens.h and entities.h
/// are the portable core and are compiled into the host test suite, so they
/// stay framework-free. See README, "Portable core".
class VentAxiaHub : public Component, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

  // Runs LATE so that any entity this hub publishes into is already set up.
  float get_setup_priority() const override { return setup_priority::LATE; }

  void set_read_only(bool read_only) { this->read_only_ = read_only; }

  void set_text_sensor(TextKey key, text_sensor::TextSensor *sensor) {
    this->text_sensors_[static_cast<size_t>(key)] = sensor;
  }

 protected:
  void publish_text_(TextKey key, const std::string &value);

  bool read_only_{false};
  protocol::Framer framer_;
  Display display_;
  std::array<text_sensor::TextSensor *, static_cast<size_t>(TextKey::COUNT)> text_sensors_{};
};

}  // namespace vent_axia
}  // namespace esphome
