#pragma once

#include "esphome/components/uart/uart.h"
#include "esphome/core/component.h"

namespace esphome {
namespace vent_axia {

/// Hub for a Vent-Axia Sentinel Kinetic MVHR on its wired-remote serial port.
///
/// The component impersonates the wired remote: the MVHR pushes a 41-byte
/// display frame roughly every 300 ms, and a keypress is a stream of 8-byte
/// frames repeated for as long as the key is held (there is no key-up frame).
class VentAxiaHub : public Component, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

  // Runs LATE so that any entity this hub publishes into is already set up.
  float get_setup_priority() const override { return setup_priority::LATE; }

  void set_read_only(bool read_only) { this->read_only_ = read_only; }

 protected:
  bool read_only_{false};
  uint32_t bytes_received_{0};
};

}  // namespace vent_axia
}  // namespace esphome
