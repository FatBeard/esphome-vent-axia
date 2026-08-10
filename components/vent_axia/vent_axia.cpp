#include "vent_axia.h"

#include "esphome/core/log.h"

namespace esphome {
namespace vent_axia {

static const char *const TAG = "vent_axia";

void VentAxiaHub::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Vent-Axia hub...");

  // The hub does the publishing (per architecture: Display never touches
  // text_sensor::TextSensor). The callback only fires for lines that
  // actually changed, so a change to line1 alone never re-publishes line2.
  this->display_.set_on_change([this](bool line1_changed, bool line2_changed) {
    if (line1_changed) {
      this->publish_text_(TextKey::DISPLAY_LINE_1, this->display_.line1());
    }
    if (line2_changed) {
      this->publish_text_(TextKey::DISPLAY_LINE_2, this->display_.line2());
    }
  });

  if (this->read_only_) {
    ESP_LOGI(TAG, "read_only is set: not sending the alive frame");
  } else {
    const auto frame = protocol::build_alive_frame();
    this->write_array(frame.data(), frame.size());
    ESP_LOGI(TAG, "Sent alive frame");
  }
}

void VentAxiaHub::loop() {
  // Byte-at-a-time: drains whatever the UART FIFO has this tick, one frame
  // worth of state machine per byte, so nothing is buffered on our own side
  // waiting for a "full frame" read that a mid-stream glitch could stall.
  uint8_t byte;
  protocol::DisplayFrame frame;
  while (this->available() != 0 && this->read_byte(&byte)) {
    if (this->framer_.feed(byte, &frame)) {
      this->display_.update(frame.line1, frame.line2, millis());
    }
  }
}

void VentAxiaHub::dump_config() {
  ESP_LOGCONFIG(TAG, "Vent-Axia:");
  ESP_LOGCONFIG(TAG, "  Read-only: %s", YESNO(this->read_only_));
  ESP_LOGCONFIG(TAG, "  Frames received: %u", this->framer_.frames_received());
  ESP_LOGCONFIG(TAG, "  Frames dropped (bad CRC): %u", this->framer_.frames_dropped());
  this->check_uart_settings(9600, 1, uart::UART_CONFIG_PARITY_NONE, 8);
}

void VentAxiaHub::publish_text_(TextKey key, const std::string &value) {
  text_sensor::TextSensor *sensor = this->text_sensors_[static_cast<size_t>(key)];
  if (sensor != nullptr) {
    sensor->publish_state(value);
  }
}

}  // namespace vent_axia
}  // namespace esphome
