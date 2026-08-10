#include "vent_axia.h"

#include <cmath>

#include "esphome/core/log.h"

#include "parser.h"

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
      // status_message is a trimmed, friendlier sibling of display_line_1
      // for the status loop specifically -- see text_sensor.py. Gated on
      // screen_kind() rather than published unconditionally so a menu
      // screen's line1 (e.g. "Set Clock") never overwrites it; the entity
      // simply holds its last status-loop value while a sequence is
      // elsewhere, same as every other status-derived entity.
      if (this->display_.screen_kind() == screens::ScreenKind::STATUS) {
        this->publish_text_(TextKey::STATUS_MESSAGE, parser::trim(this->display_.line1()));
      }
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
      const uint32_t now = millis();
      this->have_frame_ = true;
      this->last_frame_at_ms_ = now;
      this->display_.update(frame.line1, frame.line2, now);
      // Fed every frame, not just changed ones: StatusTracker's aging clock
      // (status.h) needs a regular heartbeat to notice time passing, and a
      // repeated identical frame is exactly the case where "nothing changed,
      // still true" has to be confirmed rather than silently skipped.
      this->status_.update(this->display_.line1(), this->display_.line2(),
                            this->display_.screen_kind() == screens::ScreenKind::STATUS, now);
      this->publish_status_();
    }
  }

  // Checked every tick, independently of whether a frame arrived this tick:
  // a dead link produces no frames at all, so link_up must be reevaluated on
  // the clock, not on an event that has stopped happening.
  this->publish_link_up_(millis());
}

void VentAxiaHub::dump_config() {
  ESP_LOGCONFIG(TAG, "Vent-Axia:");
  ESP_LOGCONFIG(TAG, "  Read-only: %s", YESNO(this->read_only_));
  ESP_LOGCONFIG(TAG, "  Frames received: %u", this->framer_.frames_received());
  ESP_LOGCONFIG(TAG, "  Frames dropped (bad CRC): %u", this->framer_.frames_dropped());
  this->check_uart_settings(9600, 1, uart::UART_CONFIG_PARITY_NONE, 8);
}

// Each publish_ helper compiles to a no-op when its platform is absent from
// the build, so the decode path calls them unconditionally and stays free of
// preprocessor noise. The (void) casts keep unused parameters quiet.
void VentAxiaHub::publish_text_(TextKey key, const std::string &value) {
#ifndef USE_TEXT_SENSOR
  (void) key;
  (void) value;
#else
  text_sensor::TextSensor *sensor = this->text_sensors_[static_cast<size_t>(key)];
  if (sensor != nullptr) {
    sensor->publish_state(value);
  }
#endif
}

void VentAxiaHub::publish_sensor_(SensorKey key, std::optional<int> value) {
#ifndef USE_SENSOR
  (void) key;
  (void) value;
#else
  const size_t idx = static_cast<size_t>(key);
  sensor::Sensor *sens = this->sensors_[idx];

  if (!value.has_value()) {
    // Two different situations, and they need different answers. If nothing
    // was ever published, the tracker simply doesn't know yet: publish
    // nothing, so the entity stays unknown rather than becoming a guess.
    //
    // But if a value *was* published and has now gone away, the last one is
    // stale and actively misleading -- boost_time_remaining is the case that
    // matters: when a 30-minute boost ends, the countdown disappears from
    // line2 and Home Assistant would otherwise go on showing "30 min"
    // indefinitely. There is no way to un-publish, so publish NaN, which HA
    // renders as unknown.
    if (this->last_sensor_value_[idx].has_value()) {
      this->last_sensor_value_[idx].reset();
      if (sens != nullptr) {
        sens->publish_state(NAN);
      }
    }
    return;
  }

  if (this->last_sensor_value_[idx].has_value() && *this->last_sensor_value_[idx] == *value) {
    return;  // unchanged since the last publish -- don't spam the API at ~3 Hz
  }
  this->last_sensor_value_[idx] = value;
  if (sens != nullptr) {
    sens->publish_state(static_cast<float>(*value));
  }
#endif
}

void VentAxiaHub::publish_binary_(BinaryKey key, std::optional<bool> value) {
#ifndef USE_BINARY_SENSOR
  (void) key;
  (void) value;
#else
  if (!value.has_value()) {
    return;  // tracker doesn't know yet -- leave the entity unpublished, not a guess
  }
  const size_t idx = static_cast<size_t>(key);
  if (this->last_binary_value_[idx].has_value() && *this->last_binary_value_[idx] == *value) {
    return;  // unchanged since the last publish -- don't spam the API at ~3 Hz
  }
  this->last_binary_value_[idx] = value;
  binary_sensor::BinarySensor *sens = this->binary_sensors_[idx];
  if (sens != nullptr) {
    sens->publish_state(*value);
  }
#endif
}

void VentAxiaHub::publish_status_() {
  this->publish_binary_(BinaryKey::SUMMER_BYPASS, this->status_.summer_bypass());
  this->publish_binary_(BinaryKey::BOOSTING, this->status_.boosting());
  this->publish_binary_(BinaryKey::PURGING, this->status_.purging());
  this->publish_binary_(BinaryKey::DEFROST_ACTIVE, this->status_.defrost_active());
  this->publish_binary_(BinaryKey::DRYOUT_ACTIVE, this->status_.dryout_active());
  this->publish_binary_(BinaryKey::FILTER_CHANGE_DUE, this->status_.filter_change_due());

  this->publish_sensor_(SensorKey::AIRFLOW, this->status_.airflow_percent());
  this->publish_sensor_(SensorKey::BOOST_TIME_REMAINING, this->status_.boost_time_remaining());
}

void VentAxiaHub::publish_link_up_(uint32_t now_ms) {
  // False before the first frame (have_frame_ starts false) and false again
  // once LINK_TIMEOUT_MS has passed without a new one -- replaces the old
  // setup's "infer liveness from line2 having stopped republishing" (PLAN.md
  // §7).
  const bool up = this->have_frame_ && (now_ms - this->last_frame_at_ms_) < LINK_TIMEOUT_MS;
  this->publish_binary_(BinaryKey::LINK_UP, up);
}

}  // namespace vent_axia
}  // namespace esphome
