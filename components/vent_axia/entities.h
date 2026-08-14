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
// Stage 2 (status line decode) added SensorKey and BinaryKey members and one
// more TextKey. Stage 3 (diagnostics table) adds the bulk of both -- one
// member per page/field the table decodes, see diagnostics.cpp. Stage 6
// (settings read/write, sequence.h's ReadSettings/WriteSetting) adds
// SwitchKey and NumberKey, the first two enums here that back a WRITABLE
// entity rather than a read-only one. Stage 7 adds SelectKey (airflow_mode)
// the same way, plus the unit clock and diagnostics-updated timestamp
// TextKeys; this file is expected to keep growing rather than being
// considered done.

#include <cstdint>

namespace esphome {
namespace vent_axia {

enum class SensorKey : uint8_t {
  AIRFLOW,               // status line2, %
  BOOST_TIME_REMAINING,  // status line2 countdown, minutes; unpublished outside a timed boost

  // Diagnostic pages 0/1: commanded airflow %, motor PWM % and fan RPM for
  // each motor. NOTE the correction in diagnostics.cpp: the manufacturer's
  // sheet labels the first field "flow", but it is identical on both motors
  // while PWM and RPM differ, and it matches the status screen's percentage
  // exactly -- it is a commanded setpoint, not a measured flow rate.
  SUPPLY_AIRFLOW_SET,
  SUPPLY_MOTOR_PWM,
  SUPPLY_FAN_RPM,
  EXTRACT_AIRFLOW_SET,
  EXTRACT_MOTOR_PWM,
  EXTRACT_FAN_RPM,
  // Diagnostic pages 2/3: T1/T2 sensor temperatures (fault flags are
  // BinaryKeys below, same pages).
  SUPPLY_AIR_TEMP,
  EXTRACT_AIR_TEMP,
  // Diagnostic page 4: the unit's own internal RH/temperature sensor.
  // Distinct from Indoor Temp (a menu setpoint, not a reading) -- see
  // vent-axia-esphome-project.md. Unpublished as a group when the page-4
  // hook's "no sensor fitted" sentinel fires (diagnostics.cpp).
  INDOOR_TEMP,
  INDOOR_HUMIDITY,
  INDOOR_HUMIDITY_AVG,
  // Diagnostic page 23: hours remaining on the filter timer. FILTER_CHANGE_DUE
  // (below) is derived from this reaching zero.
  FILTER_HOURS,
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
  // Diagnostic pages 2/3: T1/T2 sensor fault codes, nonzero == fault.
  SUPPLY_TEMP_FAULT,
  EXTRACT_TEMP_FAULT,
  // Diagnostic page 19: the control PCB's 24V rail / fuse FS1. INVERTED --
  // the field itself is 1 == ok, 0 == fault -- see diagnostics.cpp.
  RAIL_24V_FAULT,
  // Diagnostic page 11: wireless receiver fitted flag, nonzero == fitted.
  WIRELESS_FITTED,
  // Diagnostic pages 6/7/8: wall switch inputs SW1-3, nonzero == closed.
  SWITCH_LINE_1,
  SWITCH_LINE_2,
  SWITCH_LINE_3,
  // Diagnostic page 5, column 0: the switched-live boost input -- a wired
  // switched live (e.g. a light switch's pull-cord) that holds the unit
  // boosting for as long as it's asserted, distinct from SWITCH_LINE_1-3
  // above (pages 6/7/8 never report this input -- byte-identical whether it
  // is asserted or not, see diagnostics.cpp) and from BOOSTING above (true
  // for both switched and commanded boosts; this flag is 0 through a
  // genuine HA-commanded boost -- see diagnostics.cpp for the captures that
  // established that). Reaches this component only through the ~15-minute
  // fetch_diagnostics scrape, so it is stale by construction -- useful as
  // an explanation in Home Assistant, never a precondition for a command.
  SWITCHED_LIVE_BOOST,
  // Diagnostic page 24: antifrost mode != 0. ANTIFROST_MODE (TextKey, below)
  // carries the human-readable form of the same field.
  ANTIFROST_ACTIVE,
  // Link liveness, tracked by the hub from CRC-valid frame timing -- not
  // part of the status-screen decode, so it stays true/known even while a
  // sequence has parked the display in a menu.
  LINK_UP,
  // Stage 4: true while Keypad::busy() is -- a tap (including its trailing
  // gap) or a hold in progress. Stage 5 broadens this to Keypad::busy() OR
  // Runner::busy(): a sequence can be between keypresses (e.g. FetchDiagnostics'
  // settle steps) and still very much be "doing something" as far as a
  // dashboard is concerned. Lets a dashboard show that a slow operation is in
  // flight (PLAN.md risk 3: airflow_mode transitions can take ~25-30s) rather
  // than the entity just appearing to sit still.
  BUSY,
  COUNT,
};

enum class SwitchKey : uint8_t {
  // The bypass On/Off setting, user menu entry 2 -- PLAN.md §6's "Summer
  // Mode (Enable Bypass)". Written via sequence.h's WriteSetting, read back
  // via ReadSettings; see switch.py for why this is deliberately not
  // optimistic.
  SUMMER_MODE,
  COUNT,
};

enum class SelectKey : uint8_t {
  // Normal / Boost 30 min / Boost 60 min / Boost Continuous / Purge
  // (PLAN.md §3/§6) -- the Main key's cumulative press counter, exposed as
  // an absolute set-point rather than five separate "press N times"
  // buttons. Written via sequence.h's SetAirflowMode; NOT read back through
  // it, unlike SwitchKey/NumberKey above -- see select.py and
  // VentAxiaHub::publish_airflow_mode_() for why the confirmed value comes
  // from the passive status-line decode instead. Continuous boost was
  // excluded here until 13 Aug 2026 (a decision, not an oversight, per
  // CLAUDE.md/PLAN.md §4 at the time) -- reopened against live evidence
  // from 192.168.1.200; see select.py and status.h's continuous_boost() for
  // the discriminator that makes it decodable after all.
  AIRFLOW_MODE,
  COUNT,
};

enum class NumberKey : uint8_t {
  // The bypass target: the unit calls this screen "Indoor Temp", but it is a
  // SETPOINT (16-40 C, default 25), not a measurement -- SensorKey::INDOOR_TEMP
  // above is the real, measured reading from diagnostic page 4. Renamed here
  // to keep the two apart, same reasoning as mhrv_orig/summer_bypass.yaml.
  BYPASS_INDOOR_TEMP,
  // The outdoor cut-off, off the end of the documented menu -- reachable only
  // by stepping past Indoor Temp's editor (sequence.h's AdjustField/
  // ExitEditChain). Its range is 5-20 C, confirmed (PLAN.md risk 6) -- see
  // number.py.
  BYPASS_OUTDOOR_TEMP,
  COUNT,
};

enum class TextKey : uint8_t {
  DISPLAY_LINE_1,
  DISPLAY_LINE_2,
  STATUS_MESSAGE,
  // Diagnostic page 24's mode field, spelled out -- see diagnostics.cpp for
  // the mode -> text table.
  ANTIFROST_MODE,
  // Diagnostic page 20's tri-state field, spelled out -- see diagnostics.cpp.
  // What "West"/"Link" mean beyond the manual's own labels is not known;
  // nothing on this unit is known to depend on the value (PLAN.md §4).
  WEST_LINK_STATE,
  // Diagnostic pages 25/26: whole-line, trimmed.
  SERIAL_NUMBER,
  FIRMWARE_VERSION,
  // Optional escape hatch: "NN: <line2>" for whichever diagnostic page was
  // last seen, decoded or not (diagnostics::format_raw_page). Exists so a
  // page this component doesn't understand is still visible to a human
  // without a component change -- see diagnostics.h.
  RAW_DIAGNOSTIC_PAGE,
  // Stage 5: wall-clock timestamp of the last successful FetchDiagnostics
  // run, stamped by the hub (not by the sequence -- portable core has no
  // notion of wall-clock time, see FetchDiagnostics::set_on_success()).
  // Unpublished until the first run completes; skipped entirely (never
  // failing) if the hub has no time_id configured.
  DIAGNOSTICS_UPDATED,
  COUNT,
};

}  // namespace vent_axia
}  // namespace esphome
