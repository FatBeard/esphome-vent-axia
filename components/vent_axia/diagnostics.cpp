#include "diagnostics.h"

#include <cstddef>

#include "parser.h"

namespace esphome {
namespace vent_axia {
namespace diagnostics {

namespace {

// ------------------------------------------------------------- the table --
// Roughly 90% of diagnostic pages are "extract an integer at (pos, len) and
// publish it to a key, possibly as a boolean" -- PLAN.md §4. Those live here
// as data. Field positions and this unit's captured values are all from
// mhrv_orig/vent-axia-esphome-project.md's page map, cross-checked against
// mhrv_orig/diagnostic_sensors.yaml, the 14 lambdas this table replaces.

/// How to turn a field's parsed integer into a published value. A field's
/// `kind` selects both which Sink member gets called and which of `key`'s
/// two possible meanings (SensorKey or BinaryKey) applies -- see the
/// sensor_field()/nonzero_field()/inverted_field() builders below, which are
/// the only way a Field is ever constructed, so kind and key never disagree
/// in practice even though the struct itself can't enforce that statically.
/// A tagged union rather than three parallel tables, per the brief: three
/// tables would let a page's fields drift out of (pos, len) order across
/// them, which is exactly the kind of mistake this table exists to make
/// impossible.
enum class FieldKind : uint8_t {
  SENSOR,          // publish the parsed integer as-is
  BINARY_NONZERO,  // publish (value != 0)
  BINARY_ZERO,     // publish (value == 0) -- see inverted_field()
};

struct Field {
  uint8_t pos;
  uint8_t len;
  FieldKind kind;
  uint8_t key;  // static_cast<SensorKey> or static_cast<BinaryKey>, per kind
};

constexpr Field sensor_field(uint8_t pos, uint8_t len, SensorKey key) {
  return Field{pos, len, FieldKind::SENSOR, static_cast<uint8_t>(key)};
}
constexpr Field nonzero_field(uint8_t pos, uint8_t len, BinaryKey key) {
  return Field{pos, len, FieldKind::BINARY_NONZERO, static_cast<uint8_t>(key)};
}
/// The 24V rail field is 1 == ok, 0 == fault -- backwards from every other
/// boolean field on these pages, and easy to get wrong in exactly the
/// direction that would silently hide a real fuse failure. Named
/// differently from nonzero_field() on purpose, so a reader (or a future
/// table entry) has to consciously reach for "inverted" rather than
/// copy-pasting nonzero_field() and missing the difference.
constexpr Field inverted_field(uint8_t pos, uint8_t len, BinaryKey key) {
  return Field{pos, len, FieldKind::BINARY_ZERO, static_cast<uint8_t>(key)};
}

template<typename T, size_t N> constexpr uint8_t array_len(const T (&)[N]) { return static_cast<uint8_t>(N); }

using PageHook = void (*)(const std::string &line2, const Sink &sink);

struct Page {
  uint8_t index;
  const Field *fields;
  uint8_t field_count;
  PageHook hook;  // nullptr for a page fully covered by `fields`
};

// Named hooks for the ~10% of pages that are not a plain field extraction --
// forward-declared here, defined below the table.
void page4_internal_sensor_hook(const std::string &line2, const Sink &sink);
void page23_filter_hook(const std::string &line2, const Sink &sink);
void page24_antifrost_hook(const std::string &line2, const Sink &sink);
void page25_serial_hook(const std::string &line2, const Sink &sink);
void page26_firmware_hook(const std::string &line2, const Sink &sink);

// Page 0/1: "018 029 % 0994  " -- airflow setpoint %, motor PWM %, RPM.
//
// CORRECTION, worth recording because it is easy to get backwards: field 1
// (pos 0, len 3) is the COMMANDED airflow percentage, not a measured flow,
// despite the manufacturer's own column header calling it "flow". Evidence:
// the value is identical on both motors ("018" on both page 0 and page 1)
// while PWM and RPM differ between supply and extract, and it matches the
// status screen's percentage exactly -- "018" here against "18%" on the
// status line. Treating it as a flow rate in l/s would be wrong, hence the
// entity name "Airflow Setpoint (Commanded)" rather than a flow unit.
constexpr Field PAGE_0_FIELDS[] = {
    sensor_field(0, 3, SensorKey::SUPPLY_AIRFLOW_SET),
    sensor_field(4, 3, SensorKey::SUPPLY_MOTOR_PWM),
    sensor_field(10, 4, SensorKey::SUPPLY_FAN_RPM),
};
constexpr Field PAGE_1_FIELDS[] = {
    sensor_field(0, 3, SensorKey::EXTRACT_AIRFLOW_SET),
    sensor_field(4, 3, SensorKey::EXTRACT_MOTOR_PWM),
    sensor_field(10, 4, SensorKey::EXTRACT_FAN_RPM),
};

// Page 2/3: " 16 C 00        " -- temperature, then a fault code.
// T1 (page 2) faults: 1 short-circuit, 4 open-circuit.
// T2 (page 3) faults: 2 short-circuit, 8 open-circuit.
// Only "is it nonzero" is decoded -- which specific code faulted is not
// currently surfaced as its own entity; a problem binary sensor is enough
// to alert on, and the exact code is still visible via raw_diagnostic_page.
constexpr Field PAGE_2_FIELDS[] = {
    sensor_field(0, 3, SensorKey::SUPPLY_AIR_TEMP),
    nonzero_field(6, 2, BinaryKey::SUPPLY_TEMP_FAULT),
};
constexpr Field PAGE_3_FIELDS[] = {
    sensor_field(0, 3, SensorKey::EXTRACT_AIR_TEMP),
    nonzero_field(6, 2, BinaryKey::EXTRACT_TEMP_FAULT),
};

// Page 5: "1 00 05 0 00 030" -- switch-line sampler internals for the
// SWITCHED LIVE boost input (a wired switched live, e.g. a light switch's
// pull-cord, held on for as long as the light is on) -- distinct from the
// SW1/SW2/SW3 wall-switch inputs on pages 6/7/8 below, which are
// byte-identical ("0000 1 0 000 00 ") whether this switch is asserted or
// released, so they have no column to retarget onto it (verified 13 Aug
// 2026) and are left as-is.
//
// Captured live from 192.168.1.200 (firmware V32/05, 13 Aug 2026), all
// exactly 16 chars:
//
//           0123456789012345
// switched  1 00 05 0 00 030    <- boosting, held on by a light-linked switched live (toilet light on)
// switched  1 00 05 0 01 030    <- same episode, cols 10-11 ticking
// commanded 0 00 00 0 00 000    <- boosting on a Boost 30 min commanded from Home Assistant, light off
// commanded 0 00 00 0 05 000    <- same episode, cols 10-11 ticking
// idle      0 00 00 0 00 000    <- not boosting, light off
// idle      0 00 00 0 02 000
// idle      0 00 00 0 04 000
//
// Column 0 is the only field decoded here: it reads 1 only when the
// switched live is asserted, and stayed 0 right through a genuine
// HA-commanded boost (the "commanded" rows above, captured alongside line2
// `48%       30m`, Boost Active ON, airflow_mode Boost 30 min) -- so this is
// a switch-INPUT flag, not a "boosting" flag. Do not confuse it with
// BinaryKey::BOOSTING (the status-line decode, status.cpp), which is true
// for both switched and commanded boosts.
//
// Cols 5-6 ("05" asserted / "00" otherwise) and cols 13-15 ("030" asserted /
// "000" otherwise, presumably the overrun period in minutes the switch keeps
// the fan running after the light goes off) both correlate with the switched
// live too, but are deliberately left UNDECODED, and a third capture that
// evening is why. Sampled across ONE continuous switched-live episode
// (23:1x-23:22, the light on throughout), cols 13-15 read 030, then 029,
// then 030 again. That is neither a constant nor a monotonic countdown, so
// both of the obvious readings are wrong as stated:
//
//   - not a fixed *configured* period, or it could not have shown 029;
//   - not a *remaining* countdown, or it could not have gone back up.
//
// The reading that fits is an overrun timer being continuously RELOADED for
// as long as the switch is held -- 029 being the one sample that landed
// between a tick and its reload -- which would mean the field only becomes a
// meaningful countdown once the switch RELEASES. That is untested: every
// sample so far was taken with the light on, because that is the only state
// in which the field is nonzero at all and the scrape takes ~90s to reach
// this page. Settling it needs a scrape started immediately after the light
// goes off, inside the overrun. Until then a decoded value here would carry
// an observation the evidence does not establish.
//
// Cols 8-9 (always "00" in every capture above) and cols 10-11 (ticking
// 00/01/02/04/05 across ALL three episode types, switched and commanded and
// idle alike) are unexplained and also left undecoded -- cols 10-11 look
// like some general sample/tick counter unrelated to switch state, but with
// only a handful of samples there's nothing safe to name it.
//
// STALE BY CONSTRUCTION, same as every other diagnostic-page entity: this
// whole page reaches the component only through the ~15-minute
// fetch_diagnostics scrape (PLAN.md §4), so a reading here can lag reality
// by up to that long. It is useful as an explanation surfaced in Home
// Assistant ("why is the unit boosting right now") but must never become a
// precondition for refusing or gating a user command -- a stale "not
// asserted" reading is not proof the switch is currently off.
constexpr Field PAGE_5_FIELDS[] = {nonzero_field(0, 1, BinaryKey::SWITCHED_LIVE_BOOST)};

// Page 6/7/8: "0000 1 0 000 00 " -- raw, link state, closed, west %, west
// time for SW1/SW2/SW3. Only "is the contact closed" is decoded.
constexpr Field PAGE_6_FIELDS[] = {nonzero_field(7, 1, BinaryKey::SWITCH_LINE_1)};
constexpr Field PAGE_7_FIELDS[] = {nonzero_field(7, 1, BinaryKey::SWITCH_LINE_2)};
constexpr Field PAGE_8_FIELDS[] = {nonzero_field(7, 1, BinaryKey::SWITCH_LINE_3)};

// Page 11: "16 0 16 0000 000" -- raw, fitted flag, rx nibble count, rx byte,
// timer. Only the fitted flag is decoded.
constexpr Field PAGE_11_FIELDS[] = {nonzero_field(3, 1, BinaryKey::WIRELESS_FITTED)};

// Page 19: "1               " -- the control PCB's 24V rail / fuse FS1.
// INVERTED: 1 == ok, 0 == fault. See inverted_field()'s comment above.
constexpr Field PAGE_19_FIELDS[] = {inverted_field(0, 1, BinaryKey::RAIL_24V_FAULT)};

// The page table itself. Deliberately sparse: a page number with no entry
// here is not an error (see decode_page()) -- this is what lets it have
// nothing to say about page 28, which does not exist on firmware V32/05.
// The old component hardcoded 28 as its scrape terminator and hung for 60s
// waiting for it; this table encodes no assumption about the highest page
// number at all.
constexpr Page PAGES[] = {
    {0, PAGE_0_FIELDS, array_len(PAGE_0_FIELDS), nullptr},
    {1, PAGE_1_FIELDS, array_len(PAGE_1_FIELDS), nullptr},
    {2, PAGE_2_FIELDS, array_len(PAGE_2_FIELDS), nullptr},
    {3, PAGE_3_FIELDS, array_len(PAGE_3_FIELDS), nullptr},
    // Page 4 is entirely hook-driven (the "no sensor fitted" sentinel needs
    // to see all three fields together before publishing any of them) --
    // see page4_internal_sensor_hook below.
    {4, nullptr, 0, page4_internal_sensor_hook},
    // 5: switched-live boost input -- see PAGE_5_FIELDS' comment above for
    // the captures and why only column 0 is decoded.
    {5, PAGE_5_FIELDS, array_len(PAGE_5_FIELDS), nullptr},
    {6, PAGE_6_FIELDS, array_len(PAGE_6_FIELDS), nullptr},
    {7, PAGE_7_FIELDS, array_len(PAGE_7_FIELDS), nullptr},
    {8, PAGE_8_FIELDS, array_len(PAGE_8_FIELDS), nullptr},
    // 9/10: SW4/SW5 -- not wired on this unit's wall-switch inputs; not decoded.
    {11, PAGE_11_FIELDS, array_len(PAGE_11_FIELDS), nullptr},
    // 12-18: wireless T0-T4 timers, the security PIN digits, and the two
    // plug-in sensor sockets -- empty on this unit (no wireless receiver, no
    // plug-in sensors, no PIN set), not decoded.
    {19, PAGE_19_FIELDS, array_len(PAGE_19_FIELDS), nullptr},
    // 20: west/normal link raw state -- not understood, not decoded.
    // 21/22: pressure sensors 1/2 -- not fitted on this unit, not decoded.
    {23, nullptr, 0, page23_filter_hook},
    {24, nullptr, 0, page24_antifrost_hook},
    {25, nullptr, 0, page25_serial_hook},
    {26, nullptr, 0, page26_firmware_hook},
    // 27 is "Reset". The sheet says only "press Set to reset"; what it
    // actually does has never been tried and is not guessed at here. This
    // stage never presses anything -- decode_page() only ever reads line2,
    // and there is deliberately no entry above that would do so for page
    // 27 either, since even reading it earns no benefit worth the risk of a
    // future table edit accidentally growing a write path here. The keypad
    // and sequence stages that come after this one must NOT press Set while
    // the display shows this page -- see PLAN.md §7, which interlocks Set
    // off entirely for the whole diagnostic menu, not just this page.
};

// ------------------------------------------------------------- dispatch --

void apply_field(const Field &f, const std::string &line2, const Sink &sink) {
  int value = 0;
  if (!parser::parse_field(line2, f.pos, f.len, value)) {
    return;  // blank -- see Sink's comment: simply don't call, don't publish 0
  }
  switch (f.kind) {
    case FieldKind::SENSOR:
      if (sink.publish_sensor) {
        sink.publish_sensor(static_cast<SensorKey>(f.key), value);
      }
      break;
    case FieldKind::BINARY_NONZERO:
      if (sink.publish_binary) {
        sink.publish_binary(static_cast<BinaryKey>(f.key), value != 0);
      }
      break;
    case FieldKind::BINARY_ZERO:
      if (sink.publish_binary) {
        sink.publish_binary(static_cast<BinaryKey>(f.key), value == 0);
      }
      break;
  }
}

// ---------------------------------------------------------------- hooks --

// Page 4: "59 % 23 C 59 037" -- RH %, temperature, 5-minute average RH,
// sample timer (the timer field is not decoded -- internal sampler state).
//
// SENTINEL: the manufacturer's sheet notes that RH == 0 AND temperature == 0
// together means no internal sensor is fitted at all, not "it read zero on
// both". That combination must publish NOTHING -- not even the average --
// rather than a pair of plausible-looking but fictitious 0%/0°C readings.
// This is why page 4 has no table entry above: the three fields cannot be
// decided independently, so they cannot be three Field rows.
void page4_internal_sensor_hook(const std::string &line2, const Sink &sink) {
  int rh = 0;
  int temp = 0;
  int avg = 0;
  const bool have_rh = parser::parse_field(line2, 0, 2, rh);
  const bool have_temp = parser::parse_field(line2, 5, 2, temp);
  const bool have_avg = parser::parse_field(line2, 10, 2, avg);

  if (have_rh && have_temp && rh == 0 && temp == 0) {
    return;  // no sensor fitted -- see comment above
  }
  if (have_rh && sink.publish_sensor) {
    sink.publish_sensor(SensorKey::INDOOR_HUMIDITY, rh);
  }
  if (have_temp && sink.publish_sensor) {
    sink.publish_sensor(SensorKey::INDOOR_TEMP, temp);
  }
  if (have_avg && sink.publish_sensor) {
    sink.publish_sensor(SensorKey::INDOOR_HUMIDITY_AVG, avg);
  }
}

// Page 23: "00000           " -- hours left of the configured filter service
// interval.
//
// filter_change_due is derived here (hours == 0) and reported through
// Sink::report_filter_change_due rather than published directly -- see that
// member's comment on why only the hub can reconcile it with the live
// status-line source.
void page23_filter_hook(const std::string &line2, const Sink &sink) {
  int hours = 0;
  if (!parser::parse_field(line2, 0, 5, hours)) {
    return;
  }
  if (sink.publish_sensor) {
    sink.publish_sensor(SensorKey::FILTER_HOURS, hours);
  }
  if (sink.report_filter_change_due) {
    sink.report_filter_change_due(hours == 0);
  }
}

// Page 24: "00 0 000  00    " -- mode, status, countdown minutes, stored
// temperature, pre-heater power. Only mode is decoded: status/countdown/
// stored-temperature are not understood, and the pre-heater power field is
// documented on the manufacturer's sheet but blank on this unit, so it is
// left alone rather than guessed at.
void page24_antifrost_hook(const std::string &line2, const Sink &sink) {
  int mode = 0;
  if (!parser::parse_field(line2, 0, 2, mode)) {
    return;
  }
  if (sink.publish_binary) {
    sink.publish_binary(BinaryKey::ANTIFROST_ACTIVE, mode != 0);
  }
  if (!sink.publish_text) {
    return;
  }
  std::string text;
  switch (mode) {
    case 0:
      text = "Off";
      break;
    case 1:
      text = "Airflow 0% / 115%";
      break;
    case 2:
      text = "Airflow 85% / 115%";
      break;
    case 3:
      text = "Airflow 55% / 115%";
      break;
    case 4:
      text = "Airflow 0% / 100%";
      break;
    case 10:
      text = "Bypass";
      break;
    default:
      text = "Mode " + std::to_string(mode);
      break;
  }
  sink.publish_text(TextKey::ANTIFROST_MODE, text);
}

void page25_serial_hook(const std::string &line2, const Sink &sink) {
  if (sink.publish_text) {
    sink.publish_text(TextKey::SERIAL_NUMBER, parser::trim(line2));
  }
}

void page26_firmware_hook(const std::string &line2, const Sink &sink) {
  if (sink.publish_text) {
    sink.publish_text(TextKey::FIRMWARE_VERSION, parser::trim(line2));
  }
}

}  // namespace

std::string format_raw_page(int page, const std::string &line2) {
  std::string out;
  if (page >= 0 && page < 10) {
    out += '0';
  }
  out += std::to_string(page);
  out += ": ";
  out += line2;
  return out;
}

void decode_page(int page, const std::string &line2, const Sink &sink) {
  for (const Page &p : PAGES) {
    if (p.index != page) {
      continue;
    }
    for (uint8_t i = 0; i < p.field_count; i++) {
      apply_field(p.fields[i], line2, sink);
    }
    if (p.hook != nullptr) {
      p.hook(line2, sink);
    }
    return;
  }
  // Not in the table -- see decode_page()'s header comment. Deliberately
  // nothing here: the raw/trigger path that makes an unknown page visible
  // is the hub's job, not this function's.
}

}  // namespace diagnostics
}  // namespace vent_axia
}  // namespace esphome
