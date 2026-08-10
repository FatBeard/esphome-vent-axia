#pragma once

// Enum-keyed identifiers for the entities this component can expose, one
// enum per platform. Each enum is the single source of truth for "which
// entities exist": the platform .py files build their CONFIG_SCHEMA dict
// from the same names (see PLAN.md §5), and the hub indexes a
// std::array<..., COUNT> rather than growing a hand-written setter or a
// switch per entity. COUNT is a sentinel, not a real key -- it only exists
// so arrays can be sized as `std::array<T, count(Key::COUNT)>`.
//
// Stage 1 needed only TextKey, for the two raw display-line text sensors.
// Stage 2 (status line decode) adds SensorKey and BinaryKey members and one
// more TextKey. Stage 3 (diagnostics table) will add many more of both, and
// later stages keep going (clock, serial number, firmware version, ...);
// this file is expected to keep growing rather than being considered done.

#include <cstdint>

namespace esphome {
namespace vent_axia {

enum class SensorKey : uint8_t {
  AIRFLOW,               // status line2, %
  BOOST_TIME_REMAINING,  // status line2 countdown, minutes; unpublished outside a timed boost
  COUNT,
};

enum class BinaryKey : uint8_t {
  SUMMER_BYPASS,
  BOOSTING,
  PURGING,
  DEFROST_ACTIVE,
  DRYOUT_ACTIVE,
  // Sourced from the status line1 "Check Filter" message in this stage.
  // Stage 3 gives it a second, independent source: diagnostic page 23's
  // filter-hours field reaching zero. The two are expected to agree
  // (PLAN.md risk 7) -- if they ever don't, that disagreement is itself
  // worth surfacing rather than silently letting one source win.
  FILTER_CHANGE_DUE,
  // Link liveness, tracked by the hub from CRC-valid frame timing -- not
  // part of the status-screen decode, so it stays true/known even while a
  // sequence has parked the display in a menu.
  LINK_UP,
  COUNT,
};

enum class TextKey : uint8_t {
  DISPLAY_LINE_1,
  DISPLAY_LINE_2,
  STATUS_MESSAGE,
  COUNT,
};

}  // namespace vent_axia
}  // namespace esphome
