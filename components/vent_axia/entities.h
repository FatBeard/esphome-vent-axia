#pragma once

// Enum-keyed identifiers for the entities this component can expose, one
// enum per platform. Each enum is the single source of truth for "which
// entities exist": the platform .py files build their CONFIG_SCHEMA dict
// from the same names (see PLAN.md §5), and the hub indexes a
// std::array<..., COUNT> rather than growing a hand-written setter or a
// switch per entity. COUNT is a sentinel, not a real key -- it only exists
// so arrays can be sized as `std::array<T, count(Key::COUNT)>`.
//
// Stage 1 only needs TextKey, for the two raw display-line text sensors.
// Later stages extend SensorKey (diagnostics table, status line) and
// BinaryKey (diagnostics table) and add further TextKey members (clock,
// serial number, firmware version, ...) as more of the protocol is decoded;
// this file is expected to keep growing rather than being considered done.

#include <cstdint>

namespace esphome {
namespace vent_axia {

enum class SensorKey : uint8_t {
  COUNT = 0,
};

enum class BinaryKey : uint8_t {
  COUNT = 0,
};

enum class TextKey : uint8_t {
  DISPLAY_LINE_1,
  DISPLAY_LINE_2,
  COUNT,
};

}  // namespace vent_axia
}  // namespace esphome
