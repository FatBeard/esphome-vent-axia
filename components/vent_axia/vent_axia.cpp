#include "vent_axia.h"

#include "esphome/core/log.h"

namespace esphome {
namespace vent_axia {

static const char *const TAG = "vent_axia";

void VentAxiaHub::setup() { ESP_LOGCONFIG(TAG, "Setting up Vent-Axia hub..."); }

void VentAxiaHub::loop() {
  // Placeholder until the framer lands: drain the UART so the FIFO cannot
  // overrun, and count what arrives so dump_config can show the link is alive.
  uint8_t byte;
  while (this->available() != 0 && this->read_byte(&byte)) {
    this->bytes_received_++;
  }
}

void VentAxiaHub::dump_config() {
  ESP_LOGCONFIG(TAG, "Vent-Axia:");
  ESP_LOGCONFIG(TAG, "  Read-only: %s", YESNO(this->read_only_));
  ESP_LOGCONFIG(TAG, "  Bytes received: %u", this->bytes_received_);
  this->check_uart_settings(9600, 1, uart::UART_CONFIG_PARITY_NONE, 8);
}

}  // namespace vent_axia
}  // namespace esphome
