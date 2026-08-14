# esphome-vent-axia

An ESPHome component for the Vent-Axia Sentinel Kinetic MVHR, driven through its
wired-remote serial port.

The component impersonates the wired remote. It decodes the unit's display into
typed Home Assistant entities and drives the keypad to read and write the unit's
settings, so the YAML config is a list of entities rather than a program.

Developed against a **Sentinel Kinetic B, firmware V32/05**.

## Status

Under construction. See `PLAN.md` for the design and the staged rollout.

## Features

- **Live status decode** — airflow, fan RPM and drive percentage, supply/extract
  air temperatures, indoor temperature and humidity (plus a 5-minute average),
  filter hours remaining, and the raw display lines, all as typed sensors
  instead of scraped strings.
- **State flags as binary sensors** — summer bypass, boost, purge, defrost,
  dryout, filter-change-due and MVHR link status, plus a `busy` sensor so a
  slow keypad-driven operation (up to ~30s) is visible on a dashboard instead
  of looking hung.
- **Diagnostic page scrape** — a scheduled sequence walks the unit's
  diagnostic pages and exposes frost protection state/mode, sensor and
  24V-rail fault flags, wireless-receiver and wall-switch status, serial
  number and firmware version.
- **Settings read/write** — summer bypass on/off and its indoor/outdoor
  temperature targets, read back from the unit (not held optimistically) and
  writable through Home Assistant numbers and switches.
- **Clock sync** — a weekly scheduled job keeps the unit's clock correct; it
  has no daylight-saving awareness and drifts after ~2 weeks without mains
  power.
- **Boost and purge as a set-point** — an `airflow_mode` select (Normal /
  Boost 30 min / Boost 60 min / Boost Continuous / Purge) rather than key
  presses, since the unit's Main key is a cumulative counter.
  **Continuous boost is reported and commandable.** It shows no countdown on
  the display, so it is decoded as "boosting, not purging, and no countdown
  seen at any point in this boost episode" — held for longer than the status
  line's own alternation timeout before being believed, so that a timed boost
  expiring (countdown gone, boost flag not yet aged out) is never mistaken for
  it. That confirmation costs ~20s, so a continuous boost takes about that
  long to appear.
- **A switch-driven `Boost Continuous` cannot be cancelled from Home
  Assistant** — established on hardware, not merely suspected. A wired wall
  switch (commonly a *switched live* taken off a bathroom or toilet light, so
  it stays asserted for as long as the light is on) holds the unit's own boost
  input directly. The unit goes on cycling its status loop and accepting key
  presses, but the boost does not move: its tap counter is not what is holding
  it. Nothing but the switch releasing will clear it.

  Selecting `Normal` during one therefore fails, on purpose and quickly.
  `SetAirflowMode` watches the display across its normalising taps and, after
  two consecutive Main taps that move neither the airflow percentage nor the
  countdown, gives up and logs that the boost appears held on by an external
  switched live. That takes a few seconds rather than the full four-tap guard,
  and it names the cause instead of reporting that it does not know.

  A `Switched Live Boost Input` binary sensor reads this from diagnostic
  page 05, which is the only page that reports it — the `Wall Switch SW1-3`
  sensors read **OFF** right through a switch-initiated boost, asserted or
  not, so they are not a corroborating signal for anything here. Note that
  page 05 arrives on the ~15-minute diagnostics scrape and is stale by
  construction: treat it as an explanation after the fact, not a live
  interlock. The status line remains the live evidence.
- **Raw keypad escape hatch** — Up/Down/Set/Main exposed as bounded-duration
  buttons (never hold-switches, which could stick a key down forever across a
  reboot) for cases the decoded entities don't cover.
- **Structural mutual exclusion** — a single sequence engine (`Runner`) pumps
  one command sequence at a time, so overlapping operations are refused and
  logged rather than racing on the wire.
- **Chip-agnostic** — no hardware timer ISR, no chip-family `#ifdef`. CI
  compiles ESP8266, ESP32 (Arduino) and ESP32 (IDF) examples.
- **Host-testable core** — the protocol framing, display/status decode,
  diagnostic table and sequence state machines compile and run outside
  ESPHome, so most of the logic is covered by a fast, hardware-free test
  suite (`tests/`).

## Design notes

### Portable core

`protocol`, `screens`, `display`, `parser`, `status`, `diagnostics` and
`sequence` include **no ESPHome headers**. They are plain C++ compiled both
into the firmware and into the host test suite in `tests/`, which is what
makes the protocol, the status-line decode and the menu-driving state
machines testable without hardware. Only `vent_axia.cpp` and the platform
files (`sensor.py` and friends) touch `esphome/components/...`.

If a core file stops compiling on the host, it has grown an ESPHome dependency
and the dependency belongs on the other side of that line.

### Chip agnostic

There is no chip-family `#ifdef` anywhere, and no interrupt handler. Held keys
are retransmitted from `loop()` on a `millis()` comparison, which is the single
decision that keeps the component portable: the previous implementation needed
two incompatible hardware-timer backends, and its ESP32 one used an
Arduino-ESP32 2.x API that 3.x removed.

CI compiles `example/esp8266.yaml`, `example/esp32-arduino.yaml` and
`example/esp32-idf.yaml`, so a platform-specific regression fails the build.

Only the ESP8266 build is validated against real hardware, because that is the
only dongle in hand. ESP32 support means "compiles, and has no known platform
dependency" -- not "tested on a unit".

## Hardware

- **Board**: alextrical's wired-remote-replacement dongle
  ([hardware repo](https://github.com/alextrical/ESP32-Sentinel-Kinetic-Wireless-Dongle)),
  early revision, ESP8266EX.
- **Link**: the MVHR's 2-wire wired-remote port (RJ9/4P4C) at 9600 8N1. These
  are true RS232 levels (+/-9 V), not TTL -- the dongle's MAX3232 is doing
  necessary work.
- **Not** the separate BMS/MODBUS socket, which is untested.

## Home Assistant card

`lovelace-card/sentinel-remote-card.js` is a custom Lovelace card that
reinterprets the physical wired remote (16x2 display, Boost/Down/Select/Up)
as a dashboard panel, plus the things the physical remote has no room for: a
vent glyph that spins in proportion to actual airflow, an alert rail that stays
empty until something needs attention, a single-line row of numeric chips, an
airflow-mode selector (which is also how purge is started and stopped), and a
row of maintenance actions.

<img src="lovelace-card/MHRV-Card.png" alt="Sentinel Remote Card showing Boost Airflow at 48%, 22 minutes remaining" width="250">

> **Note:** the screenshot above is out of date. It predates the alert rail,
> the airflow-mode row and the maintenance actions, and shows an older chip row
> (humidity and filter hours). It needs a retake.

### Installation

1. Download `lovelace-card/sentinel-remote-card.js` and copy it into
   `<config>/www/sentinel-remote-card.js` (create the `www` folder in your
   Home Assistant config directory if it doesn't exist yet — you can use the
   Studio Code Server / File Editor add-on, or Samba/SSH).
2. In Home Assistant: **Settings -> Dashboards -> ⋮ (top right) -> Resources
   -> Add Resource**.
   - URL: `/local/sentinel-remote-card.js?v=1`
   - Resource type: `JavaScript Module`

   The `?v=1` is not decoration — see [Updating the card](#updating-the-card)
   below. Adding it now means later updates are a one-character edit.
3. Reload the dashboard (or do a hard browser refresh).
4. Add a new card, choose **Manual** / **Edit in YAML**, and paste in a
   config like the one below.

### Updating the card

Copying a newer `sentinel-remote-card.js` over the old one is often not enough
on its own. Browsers cache the file aggressively, and the resource URL is what
they key that cache on, so an unchanged URL can keep serving the old card
indefinitely — through dashboard reloads, and sometimes through a full Home
Assistant restart. The usual symptom is a change that appears on one device but
not another, or new config options being silently ignored.

The fix is to change the URL, which makes it a different file as far as the
browser is concerned:

1. Copy the new file over `<config>/www/sentinel-remote-card.js`.
2. **Settings -> Dashboards -> ⋮ -> Resources**, open the card's resource, and
   bump the version query: `?v=1` becomes `?v=2`, and so on. Any value works —
   only *changing* it matters.
3. Reload the dashboard.

If you skipped the query string when first adding the resource, add one now
(`/local/sentinel-remote-card.js?v=2`); it works the same as bumping an existing
one. A hard refresh (Ctrl/Cmd-Shift-R) sometimes clears it too, but it has to be
repeated on every browser and device that has ever loaded the dashboard, which
is why bumping the version is the reliable route.

For your "Vent-Axia MHRV" ESPHome device specifically, this is ready to
paste as-is (entity IDs pulled from your live device page):

```yaml
type: custom:sentinel-remote-card
title: Vent Axia MVHR
line1_entity: sensor.house_vent_axia_mhrv_display_line_1_top
line2_entity: sensor.house_vent_axia_mhrv_display_line_2_bottom
boost_button: button.house_vent_axia_mhrv_key_main
down_button: button.house_vent_axia_mhrv_key_down
select_button: button.house_vent_axia_mhrv_key_set
up_button: button.house_vent_axia_mhrv_key_up
airflow_entity: sensor.house_vent_axia_mhrv_airflow
airflow_mode_entity: select.house_vent_axia_mhrv_airflow_mode
busy_entity: binary_sensor.house_vent_axia_mhrv_mvhr_busy
link_entity: binary_sensor.house_vent_axia_mhrv_mvhr_link

# Chip row, in display order. Supply and extract sit together on one line.
chips: [supply_temp, extract_temp, boost_remaining]
supply_temp_entity: sensor.house_vent_axia_mhrv_supply_air_temperature_to_house
extract_temp_entity: sensor.house_vent_axia_mhrv_extract_air_temperature_from_house
indoor_temp_entity: sensor.house_vent_axia_mhrv_indoor_temperature_unit_sensor
humidity_entity: sensor.house_vent_axia_mhrv_indoor_humidity_in_extract_air
humidity_avg_entity: sensor.house_vent_axia_mhrv_indoor_humidity_5_minute_average
supply_rpm_entity: sensor.house_vent_axia_mhrv_supply_fan_speed
extract_rpm_entity: sensor.house_vent_axia_mhrv_extract_fan_speed
boost_remaining_entity: sensor.house_vent_axia_mhrv_boost_time_remaining
diagnostics_updated_entity: sensor.house_vent_axia_mhrv_diagnostics_last_updated

# Alert rail. Every one of these stays invisible until it fires.
bypass_entity: binary_sensor.house_vent_axia_mhrv_summer_bypass_active
antifrost_entity: binary_sensor.house_vent_axia_mhrv_frost_protection_active
antifrost_mode_entity: sensor.house_vent_axia_mhrv_frost_protection_mode
defrost_entity: binary_sensor.house_vent_axia_mhrv_defrost_active
dryout_entity: binary_sensor.house_vent_axia_mhrv_dryout_mode
purge_entity: binary_sensor.house_vent_axia_mhrv_purge_active
switched_live_entity: binary_sensor.house_vent_axia_mhrv_switched_live_boost_input
filter_due_entity: binary_sensor.house_vent_axia_mhrv_filter_change_due
filter_entity: sensor.house_vent_axia_mhrv_filter_hours_remaining
supply_fault_entity: binary_sensor.house_vent_axia_mhrv_supply_air_sensor_fault_t1
extract_fault_entity: binary_sensor.house_vent_axia_mhrv_extract_air_sensor_fault_t2
rail_fault_entity: binary_sensor.house_vent_axia_mhrv_24_v_rail_fault_fuse_fs1

boost_active_entity: binary_sensor.house_vent_axia_mhrv_boost_active

# Maintenance actions. Omit any you would rather not have one tap away.
refresh_diagnostics_button: button.house_vent_axia_mhrv_refresh_diagnostic_sensors
refresh_settings_button: button.house_vent_axia_mhrv_refresh_summer_settings
sync_clock_button: button.house_vent_axia_mhrv_sync_mvhr_clock
reset_filter_button: button.house_vent_axia_mhrv_reset_filter_timer
```

### Options

Only the display and the four buttons are required. **Every status option is
opt-in by presence**: name the entity and the icon or chip appears, delete the
line and it is gone.

| Option | Effect |
| --- | --- |
| `line1_entity`, `line2_entity` | The two LCD rows. Alternatively `display_entity` for a single sensor holding both, split on `display_separator` (default `"\n"`). One form or the other is required. |
| `boost_button`, `down_button`, `select_button`, `up_button` | The four keys. Up/Down repeat while held, matching the physical remote's fast-scroll. |
| `airflow_mode_entity` | The **Normal / 30m / 60m / Cont / Purge** segmented row — the only way to start or stop a purge from the card. See [Boost button vs airflow mode](#boost-button-vs-airflow-mode). |
| `airflow_entity` | Spins the header vent glyph in proportion to airflow, from one turn per 3.2 s at low flow to 0.8 s at 100 %, with the percentage beside it. Still when flow is zero. |
| `busy_entity` | Shows a progress bar under the LCD and locks the mode row and actions while a keypad operation or sequence is in flight. Worth setting: a mode change takes ~25–30 s, and without it the card looks hung. |
| `link_entity` | Greys the whole panel, raises an **MVHR offline** pill and disables the controls when no frames are arriving from the unit. |
| `chips`, `alerts` | Ordered lists of ids controlling which readouts appear and in what order — see below. Omit either and the historical default is used. |
| `chip_wrap` | `true` lets the chip row wrap onto more than one line again. Default `false`: one line, ellipsised. |
| `diagnostics_updated_entity` | Adds "updated *hh:mm*" to the tooltip of every chip fed by the diagnostics scrape, so you can tell how stale those figures are. |
| `boost_active_entity` | Glows the panel edge and pulses the Boost button while a boost is running. |
| `running_entity` | Fallback fixed-rate spin for the vent glyph when `airflow_entity` is not set. Redundant if it is. |
| `filter_warning_threshold` | Level at which the filter alert's fallback trigger fires. Defaults to `336` when the sensor reports `h`, otherwise `14`. |
| `refresh_diagnostics_button`, `refresh_settings_button`, `sync_clock_button`, `reset_filter_button` | Maintenance actions. `reset_filter_button` is guarded by a two-tap confirm — see [Button combos](#button-combos). |
| `title`, `accent_color`, `theme` | Panel heading, LCD/accent colour (default `#3ddc84`), and `auto` / `light` / `dark`. |

#### Chips

`chips:` is an ordered list of ids. Listing an id shows that chip, in that
position; leaving it out hides the chip without having to delete its entity
line. Each still needs its `*_entity` key set to appear at all.

```yaml
chips: [supply_temp, extract_temp, humidity]
```

| Id | Entity key | Notes |
| --- | --- | --- |
| `supply_temp` | `supply_temp_entity` | Supply air, to the house (T1). |
| `extract_temp` | `extract_temp_entity` | Extract air, from the house (T2). |
| `indoor_temp` | `indoor_temp_entity` | The unit's own room sensor. **Not** the same as the unit's "Indoor Temp" menu screen, which is the bypass setpoint. |
| `humidity` | `humidity_entity` | Relative humidity in the extract air. |
| `humidity_avg` | `humidity_avg_entity` | The 5-minute average — steadier for a glance than the instantaneous figure. |
| `co2` | `co2_entity` | This hardware has no CO2 source; the id exists for other units. |
| `supply_rpm`, `extract_rpm` | `supply_rpm_entity`, `extract_rpm_entity` | Fan speeds. The one readout that distinguishes "commanded 30 %" from "actually turning". |
| `supply_pwm`, `extract_pwm` | `supply_pwm_entity`, `extract_pwm_entity` | Motor drive percentage. Rising drive at constant RPM is the early signal of a blocked filter or duct. |
| `filter_hours` | `filter_entity` | Filter life remaining. Not shown by default — the alert rail carries the actionable version. |
| `boost_remaining` | `boost_remaining_entity` | Countdown, hidden automatically when it reads zero. |

Default when `chips:` is omitted: `supply_temp, extract_temp, humidity, co2,
boost_remaining` — the set and order the card used before this key existed.

Everything except `humidity`-vs-`co2` comes off the ~15 minute diagnostics
scrape rather than the live status frames, so those chips can be a quarter of
an hour old; `diagnostics_updated_entity` puts the scrape time in the tooltip.

#### Alerts

Same idea for the alert rail. Every pill stays invisible until it fires, so
naming all of them costs nothing.

| Id | Entity key | Tint |
| --- | --- | --- |
| `offline` | `link_entity` | Red. Fires when the link is **down**. |
| `supply_fault`, `extract_fault` | `supply_fault_entity`, `extract_fault_entity` | Red. T1/T2 sensor faults. |
| `rail_fault` | `rail_fault_entity` | Red. 24 V rail — check fuse FS1. |
| `bypass` | `bypass_entity` | Accent. Summer bypass open. |
| `antifrost` | `antifrost_entity` (+ `antifrost_mode_entity` for detail text) | Accent. |
| `filter` | `filter_due_entity`, falling back to thresholding `filter_entity` | Amber. |
| `defrost` | `defrost_entity` | Accent. |
| `dryout` | `dryout_entity` | Accent. |
| `purge` | `purge_entity` | Accent. |
| `switched_live` | `switched_live_entity` | Accent. **The most useful pill on the rail** — see below. |

Bypass, antifrost, defrost, dryout and purge are accent-tinted rather than
amber on purpose: each is the unit protecting itself or the house
automatically, and must not read as a fault. Amber is for the filter, the one
alert that needs you to do something; red is for things that are actually
broken.

`switched_live` earns its place because a wall switched live holding the boost
on is otherwise indistinguishable from the card being broken: selecting
`Normal` during one **fails, on purpose**, and nothing but the switch releasing
will clear it. It is read from the diagnostics scrape, so it can lag the boost
it explains by up to ~15 minutes — the tooltip says so.

The alert rail renders nothing at all while the unit is healthy, so anything
appearing between the LCD and the chips is worth a look.

### Boost button vs airflow mode

Both change the boost, and they are not the same thing.

- The **Boost button** taps the unit's Main key, exactly as the physical remote
  does. That key is a *cumulative counter* with no usable timeout, so repeated
  taps walk through boost states rather than setting one.
- The **airflow mode row** drives the `airflow_mode` select, which is an
  absolute set-point: "Boost 60 min" means *be* in a 60 minute boost, from
  wherever the unit currently is. The component works out the normalising key
  presses needed to get there.

Use the mode row to choose a state; the Boost button is there for remote
fidelity and for the cases the decoded entities don't cover.

Because the select is deliberately not optimistic — it reports only what the
unit's own status line confirms — a tapped segment shows a dashed *pending*
outline until the unit agrees, which takes ~25–30 s (and up to 20 s more to
confirm a continuous boost). Setting `busy_entity` alongside it is strongly
recommended.

### Button combos

The physical remote has three held gestures, and the card reaches all three —
but by pressing the buttons that run the corresponding sequence, not by trying
to hold keys itself.

| Physical gesture | Card equivalent |
| --- | --- |
| Hold `Up`+`Main`, then `Down` for 8 s | **Refresh** action → the `fetch_diagnostics` sequence |
| Hold `Main` for 5.5 s | Airflow mode row → **Purge** → the `SetAirflowMode` sequence |
| Hold `Up`+`Down` for 5.5 s | **Filter** action (two-tap confirm) → the `ResetFilter` sequence |

**Why not hold the keys directly?** Home Assistant's `button.press` is
fire-and-forget; there is no "hold this entity" service. A real hold would have
to be a `hold_key` call followed later by a `release_keys` call, with the
network in between — and a lost release leaves a key asserted until the
firmware's 30 s watchdog clears it. That is the exact failure this project
already designed out once, which is why the raw keys are bounded-duration
buttons rather than hold-switches. The card's Up/Down fast-scroll is honest
about the same limit: it is repeated short taps, not a hold.

Timing a hold on the device side, inside a sequence that can also check the
screen before and verify the result afterwards, is both safer and more capable.
`ResetFilter`, for instance, refuses outright if the display isn't on the
status screen, and confirms against diagnostic page 23 afterwards.

The filter reset is the one irreversible operation the component exposes, and
on the remote its deliberateness comes from having to hold two keys for 5.5
seconds. A single dashboard tap has none of that, so the card supplies its own:
the first tap only arms the button (it turns amber and reads **Confirm?**) and
nothing is sent unless a second tap follows within four seconds.

## Running the tests

```sh
cd tests
cmake -B build -S . && cmake --build build && ./build/run_tests
```

No dependencies beyond a C++17 compiler and CMake.

## Acknowledgements

This project would not exist without the reverse-engineering and hardware
work of others:

- **[aelias-eu/vent-axia-remote](https://github.com/aelias-eu/vent-axia-remote)**
  — the original reverse-engineering of the Sentinel Kinetic's wired-remote
  serial protocol. The framing and command decode here builds directly on
  that work.
- **[alextrical](https://github.com/alextrical/ESPHome-Vent-Axia-Sentinel-Kinetic)** — the original ESPHome
  component for this device, and the
  [ESP32-Sentinel-Kinetic-Wireless-Dongle](https://github.com/alextrical/ESP32-Sentinel-Kinetic-Wireless-Dongle)
  hardware this component runs on.

This is an independent rewrite: same protocol and hardware target, different
architecture (see "Design notes" above) and license.

## Licence

GPL-3.0 -- see `LICENSE`.
