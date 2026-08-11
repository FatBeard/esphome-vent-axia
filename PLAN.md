# Rewrite: `vent_axia` ESPHome component

## Context

`~/docker/esphome/mhrv_orig` runs a working Vent-Axia Sentinel Kinetic B (firmware `V32/05`) integration on an ESP8266 dongle at `192.168.1.200`. It works, but the split between C++ and YAML is upside down.

The upstream component (`alextrical/…`, running from a personal fork `FatBeard/…@fix/keypress-and-button-schema` with `refresh: always`) is a *transport only*: it frames the serial link and exposes four key-hold switches and two 16-character display strings. Nothing is decoded. So every piece of real device logic ended up in YAML:

- **~90KB of YAML** across 5 files: 20 scripts, 16 globals, 13 template buttons, 28 `internal: true` text sensors used purely as a C++→lambda→C++ transport, and a 7.8KB `vask_decode.h` included into every lambda's global namespace.
- **A hand-rolled mutex.** `ui_busy` is acquired at 5 sites and released at 12. Miss one release on a failure path and the device is deadlocked until reboot.
- **Cyclic file dependencies.** `mhrv.yaml` (the "hardware description") mutates 5 globals owned by two different packages; the packages reference ids back in `mhrv.yaml`. None of the three packages is independently removable.
- **~90% duplication.** `apply_summer_mode` / `apply_bypass_target` / `apply_bypass_outdoor` are three ~110-line near-copies. `adjust_day` / `adjust_hour` / `adjust_minute` likewise — and they already disagree on wrap semantics in a way that is easy to get wrong when spread across three YAML blocks.
- **The component's own logic is broken on this unit.** Its C++ diagnostic scrape waits for `"Diagnostic  28"`; firmware `V32/05` stops at 27, so it times out after 60s and abandons the display parked in the menu. `fetch_diagnostics` in `controls.yaml` is a corrected reimplementation of C++ that lives in YAML.
- **Real crash risks.** The component writes to the UART from a hardware timer ISR via non-`IRAM_ATTR` code (documented ESP8266 cache-miss crash), and builds `cmdbuffer_` byte-by-byte in loop context while that ISR may transmit mid-build.

The outcome wanted: a clean, readable, upstream-PR-quality component that owns everything device-facing, so the YAML becomes a declaration of *what to expose and when*, not *how to drive the unit*.

**Decisions taken** (from the planning conversation):
- All device-facing logic moves to C++.
- `loop()`-based TX cadence; no hardware timer ISR.
- New standalone git repo, pinned by tag.
- Clean break on Home Assistant entity IDs.
- **Chip-agnostic**: must build and run on ESP32 as well as ESP8266 (§2.1).
- **No continuous boost.** Not exposed, not detected (§4, risk 2).
- **Execution**: implement with Sonnet, review with Opus (§10).

---

## What stays in YAML, and why

The dividing line: **C++ owns "how to talk to the MVHR". YAML owns "what to expose and when to do it".**

| Stays in YAML | Moves to C++ |
|---|---|
| UART pins, board, wifi/api/ota | Framing, CRC, TX cadence, key masks |
| Entity names, icons, categories | Tap/hold/gap/watchdog sequencing |
| Schedules (`on_time` 04:30 / 04:05 Sun) | Menu navigation, editor state, closed-loop adjust |
| Which entities exist at all | Diagnostic scrape + page decode |
| Any user-specific automation | Boost / bypass / clock state machines |

This is the right split because *every* one of the YAML lambdas is really C++ that has been squeezed through a string-typed transport. `vask_decode.h` exists only because the component published nothing but display text; the 28 `internal: true` text sensors exist only to carry those strings back into a lambda. Once the component decodes, all three layers disappear.

---

## 1. Repo and file layout

New repo `esphome-vent-axia`, component name **`vent_axia`**.

```
components/vent_axia/
  __init__.py            hub schema, actions, triggers, codegen
  sensor.py              enum-keyed platform (see §5)
  binary_sensor.py
  text_sensor.py
  switch.py  number.py  select.py  button.py

  protocol.h/.cpp        frame constants, CRC, RX framing, TX frame build
  display.h/.cpp         line1/line2, change timestamps, screen classification
  parser.h/.cpp          field extraction, blank!=zero, clock, status line
  diagnostics.h/.cpp     the page/field table and its dispatch
  keypad.h/.cpp          key mask, TX cadence, tap/hold queue, watchdog
  sequence.h/.cpp        Sequence base + Runner (stack, timeouts, recovery)
  seq_*.cpp              one file per sequence (§3)
  vent_axia.h/.cpp       the hub: wires the above together, owns entities
  entities.h             SensorKey/BinaryKey/TextKey enums, single source of truth

tests/                   host-side CMake + doctest (§8)
example/mhrv.yaml        the reference config (§6)
```

Rule that makes the tests possible: `protocol`, `parser`, `diagnostics`, `screens`, `sequence` include **no ESPHome headers**. They are portable C++ compiled by both the firmware and the host test suite. Only `vent_axia.cpp` and the platform files touch `esphome/components/...`.

---

## 2.1 Chip agnosticism

**Yes — and dropping the timer ISR is what buys it.** The old component's only platform-specific code was the retransmit timer, and it needed two incompatible implementations: `timerBegin/timerAlarmWrite/timerAlarmEnable` on ESP32 and `timer1_enable/timer1_write` on ESP8266. Worse, the ESP32 path uses the Arduino-ESP32 **2.x** timer API, which was removed in 3.x — that code will not build against current ESPHome ESP32 targets at all.

Once TX is a `millis()` comparison in `loop()`, there is no `IRAM_ATTR`, no `hw_timer_t`, no `#ifdef USE_ESP32`, and nothing framework-specific left. The component becomes portable **by construction** rather than by porting effort:

- **ESP8266** (arduino) — the current dongle, `esp12e`.
- **ESP32 / ESP32-S3 / ESP32-C3** (arduino **and** esp-idf) — no code changes; only the UART pins differ in YAML.
- Any future ESP variant ESPHome supports.

Enforced rather than assumed:
- CI compiles `example/mhrv-esp8266.yaml`, `example/mhrv-esp32.yaml` and `example/mhrv-esp32-idf.yaml` on every push. A platform-specific regression fails the build.
- No `#ifdef` on chip family anywhere outside the (nonexistent) HAL. If one becomes necessary, that is a design smell to argue about first.
- The host test suite (§8) already forces the core to be framework-free, which is most of the work.

Two genuine ESP32 advantages worth noting, because they change the development experience:
- **A second UART.** The ESP8266 has one, shared with USB programming, forcing `logger: baud_rate: 0` and network-only logs. On ESP32 the MVHR goes on UART1/2 and the serial console stays live — which makes stage 1–3 bring-up substantially easier to debug.
- **The wokwi simulator becomes usable.** `alextrical/wokwi-VentAxiaSentinel-custom-chip` is ESP32-only. It replays display frames rather than modelling the menu, so it validates framing and cadence but no sequence logic — it complements `fake_mvhr` rather than replacing it. Schedule it after v1; its unique value is confirming the 20ms loop-based cadence produces the frame spacing a real bus expects.

Caveat, stated plainly: only the ESP8266 build gets validated against real hardware, because that is the only dongle in hand. ESP32 support means "compiles, and has no known platform dependency", not "tested on a unit".

## 2. The core architecture — sequence engine

This is the crux and the part most worth getting right. Every user-visible operation (fetch diagnostics, sync clock, write a bypass setting, set boost, reset filter) is a multi-second, multi-step state machine that must not block `loop()`, must be mutually exclusive with the others, and must release the keypad on *every* exit path.

Three layers:

**`Keypad`** — the only thing that touches the key mask. Itself a small state machine so `tap()` needs no `delay()`:

```
IDLE --tap(mask, ms)--> PRESSING (mask asserted, ms) --> GAP (mask clear, gap_ms) --> IDLE
```

`busy()` is true until the gap expires, so a caller waits on `!busy()` rather than on a timer. `loop()` re-sends the pre-built 8-byte frame whenever `>= 20ms` has elapsed and a mask is asserted, and sends one frame immediately on any mask change so a press is never zero-frame. An independent 30s watchdog force-releases the mask no matter what any sequence believes — this is the backstop that the old `watchdog_*` scripts provided.

**`Sequence`** — one unit of work.

```cpp
enum class Poll : uint8_t { RUNNING, DONE, FAILED };

class Sequence {
 public:
  virtual const char *name() const = 0;
  virtual Poll poll() = 0;                    // pumped while on top of the stack
  virtual void on_start() {}
  virtual void on_finish(Poll result) {}      // ALWAYS runs, however it ends
  virtual uint32_t timeout_ms() const { return 120000; }

 protected:
  Poll goto_step(uint8_t s) { step_ = s; entered_ = millis(); return Poll::RUNNING; }
  uint32_t elapsed() const { return millis() - entered_; }
  Poll await(Sequence &child, uint8_t on_ok);  // run child, then resume at on_ok
  Runner *runner_;
  uint8_t step_{0};
  uint32_t entered_{0};
};
```

**`Runner`** — a stack of sequences, pumped once per `loop()`. Only the top runs. `await()` pushes a child and resumes the parent at a named step when it completes; a child failure propagates as a parent failure. A single root sequence at a time gives mutual exclusion **structurally** — `ui_busy` stops existing rather than being reimplemented. `on_finish` is the one release site, replacing the old 12.

A non-trivial sequence then reads as a flat list of named steps. Writing Indoor Temp in full:

```cpp
Poll WriteSetting::poll() {
  switch (this->step_) {
    case NAVIGATE:   return await(runner_->goto_menu(spec_.menu_index), VERIFY);

    case VERIFY:     // right screen, and a value that actually parsed
      if (!display_->line1_starts_with(spec_.screen)) return Poll::FAILED;
      if (!display_->has_fresh_value()) return elapsed() < 3000 ? Poll::RUNNING : Poll::FAILED;
      return goto_step(OPEN);

    case OPEN:       return await(runner_->open_editor(), ADJUST);   // retries Set once
    case ADJUST:     return await(runner_->adjust_field(spec_, target_), COMMIT);
    case COMMIT:     return await(runner_->tap(Key::SET), SETTLE);
    case SETTLE:     return elapsed() >= 1800 ? goto_step(EXIT_CHAIN) : Poll::RUNNING;
    case EXIT_CHAIN: return await(runner_->exit_edit_chain(), HOME);
    case HOME:       return await(runner_->goto_menu(0), READ_BACK);
    case READ_BACK:  return await(runner_->read_settings(), FINISHED);
    default:         return Poll::DONE;
  }
}

void WriteSetting::on_finish(Poll result) {
  keypad_->release_all();
  if (result == Poll::FAILED) runner_->recover();   // one shared abort path
}
```

The three ~110-line YAML scripts collapse into this one class plus a `SettingSpec` table (menu index, screen name, parser, wrap mode, guard limit, min/max). The three clock `adjust_*` scripts collapse into `AdjustField` with a wrap modulus — and the day/hour/minute wrap disagreement becomes one field in a table instead of a discrepancy across three YAML blocks.

`recover()` is shared: release all keys, and if the display is parked on a menu screen, walk out of it using the *verified* exit gesture (hold Up to page 00, **release, 250ms, fresh hold** — holding straight through never exits; this cost a debugging round to discover and must be preserved), else wait out the unit's own 2-minute timeout.

Editor-open detection is a method, not a scattered idiom: `Display::editor_open()` returns `millis() - line2_changed_at_ < settle_ms_` (1200ms default). The ~350ms value blink is the only signal an editor is open, so this oracle is load-bearing and belongs in one place.

---

## 3. Sequences to implement

`GotoMenu`, `LeaveMenu`, `Tap`, `HoldUntil`, `OpenEditor`, `AdjustField`, `ExitEditChain` (primitives) then:

| Sequence | Notes |
|---|---|
| `FetchDiagnostics` | Up+Main to enter → hold Down 8s (unit auto-repeats all pages) → hold Up to page 00 → **release, 250ms, fresh hold** to exit. Terminates on *highest page seen*, never a hardcoded 28. |
| `ReadSettings` | Summer Mode, Indoor Temp, then Outdoor Temp via the edit chain. Clears the cached value **before** navigating (line2 only publishes on change) — except in the Outdoor hop, where it must wait for line1 first, then clear. |
| `WriteSetting` | Above. Serves summer mode, indoor temp, outdoor temp. |
| `SyncClock` | Set enters on day; each Set advances; a fourth commits. Day does not wrap; hour (mod 24) and minute (mod 60) take the shortest path. Re-reads the time each iteration so a rollover is followed. |
| `SetAirflowMode` | Normalise boost (tap Main up to 4×, probing between — line1 alternates, so the probe needs up to 8s) then apply N presses. Targets are **Normal / Boost 30 / Boost 60 / Purge only** — continuous boost is not a selectable target. Normalising may still pass *through* continuous on its way back to normal, which is transient and harmless. Purge = 5500ms Main hold. |
| `ResetFilter` | Requires the status screen, holds Up+Down 5500ms, then chains `FetchDiagnostics` and verifies page 23 moved off zero. |
| `ManualKey` | Backs the raw key buttons/switches so manual presses go through the same arbitration. |

Invariants that must survive into the code, each paid for in debugging on the real unit — worth a comment at each site:

- `gap_ms` **≥ 400**. 250ms drops ~1 press in 10; a dropped Set fails to open an editor and the following Up presses then walk *back up the menu*.
- Only **Set** is safe inside an editor. Up/Down adjust rather than navigate — walking Up out of an editor silently took a 14 °C setpoint to 19 °C.
- `LeaveMenu` presses Up **exactly once**, then waits out the unit's timeout. Mashing Up corrupts a setting if an editor is still open.
- `GotoMenu` uses the hard stop at the top (5× Up = absolute position 0), not relative counting.
- Blank ≠ zero everywhere. A blank temperature frame renders `"   C"`, not `""`.
- Page 28 does not exist on `V32/05`.

---

## 4. Decode layer

Data-driven, replacing 14 hand-written lambdas:

```cpp
struct Field { uint8_t pos, len; SensorKey key; Transform fn; };
struct Page  { uint8_t index; const Field *fields; uint8_t count; PageHook hook; };

constexpr Field PAGE_0[] = {{0, 3, SensorKey::SUPPLY_AIRFLOW_SET},
                            {4, 3, SensorKey::SUPPLY_MOTOR_PWM},
                            {10, 4, SensorKey::SUPPLY_FAN_RPM}};
constexpr Field PAGE_19[] = {{0, 1, BinaryKey::RAIL_24V_FAULT, Transform::INVERT}};
constexpr Field PAGE_23[] = {{0, 5, SensorKey::FILTER_HOURS}};
```

The ~10% that is not a plain field gets a named per-page hook rather than being forced into the table: page 4's `RH==0 && temp==0` "no sensor fitted" sentinel, page 24's antifrost enum, pages 25/26's trimmed strings. `filter_change_due` is derived in the page-23 hook from `hours == 0`.

`parser.cpp` carries `vask_decode.h`'s rules verbatim, because they encode real observations: reject blank/non-numeric and leave the value *unpublished*; permit leading spaces and `+`/`-` so the −20..50 temperature fields parse; validate the full `Ddd HH:MM` layout so a mid-blink clock frame is rejected.

Undecoded pages (5, 9, 10, 12–18, 20–22, 27) are exposed behind a single optional `raw_diagnostic_page` text sensor plus an `on_diagnostic_page` trigger, so future decoding needs no component change — rather than 28 always-present internal text sensors.

**New capability**: the status line is decoded too, which the old setup never did. `airflow %`, `boost_time_remaining` (live from `30m`/`29m`/…), purge countdown, and the status message. Boost time remaining was on the explicit wishlist.

**Continuous boost is deliberately out of scope.** It is the one mode with no reliable evidence on the display: timed boost shows `30m`/`60m` and purge shows `Purge`, but continuous boost shows only an airflow percentage indistinguishable from a high normal rate. Decoding it would rest entirely on catching `Boost Airflow` in a ~3.4s alternating status line. So: not a `select` option, and not decoded. The generic `boosting` binary sensor still goes true (it keys off the `Boost Airflow` flag, which is fine as a *hint*), and `boost_time_remaining` simply stays unpublished — correct, since there is no countdown. If someone sets continuous boost at the unit's own keypad, `boosting` reports it and `airflow_mode` may read `Normal`; that asymmetry is documented rather than papered over.

---

## 5. Python schema pattern

The enum-keyed platform, so 12 sensors are one loop rather than 29 copy-pasted blocks:

```python
SENSORS = {
    "supply_fan_rpm": sensor.sensor_schema(
        unit_of_measurement="rpm", accuracy_decimals=0,
        state_class=STATE_CLASS_MEASUREMENT, icon="mdi:fan"),
    "supply_air_temp": sensor.sensor_schema(
        unit_of_measurement=UNIT_CELSIUS, device_class=DEVICE_CLASS_TEMPERATURE,
        accuracy_decimals=0, state_class=STATE_CLASS_MEASUREMENT),
    # ...
}

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(CONF_VENT_AXIA_ID): cv.use_id(VentAxiaHub),
    **{cv.Optional(k): v for k, v in SENSORS.items()},
})

async def to_code(config):
    hub = await cg.get_variable(config[CONF_VENT_AXIA_ID])
    for key in SENSORS:
        if key in config:
            cg.add(hub.set_sensor(SensorKey[key.upper()], await sensor.new_sensor(config[key])))
```

Units, device classes, state classes and icons live in the schema, so YAML only supplies names. Hub keys: `uart_id`, `time_id`, `read_only`, and overrides for `tap_duration` / `key_gap` / `settle_time` / `key_watchdog` / `bypass_timeout`, defaulted to the values proven on the unit.

**A control platform wires two directions, and the loop above only shows one.** `sensor`/`binary_sensor`/`text_sensor` are read-only, so `hub.set_sensor(...)` is the whole job. `number`/`switch`/`select` also need the reverse pointer — `cg.register_parented(ent, hub)` plus `ent.set_key(...)` — so `control()`/`write_state()` can reach `write_number()`/`write_switch()`/`write_select()`. **Both halves, every time, and they are separate `cg.add` calls: neither implies the other.** Wiring only the entity→hub half is completely silent. Every publish helper nullptr-checks its slot before publishing, so the control commands the unit correctly and simply never reports a value back; nothing logs, nothing fails to compile, and the entity sits with no state in Home Assistant forever. All three control platforms shipped that way and it survived until stage 8 — see §8's rollout record.

---

## 6. The YAML surface

This is the deliverable asked for. `mhrv.yaml` in full — no packages, no scripts, no globals, no lambdas.

```yaml
substitutions:
  name: vent-axia-remote

esphome:
  name: ${name}
  friendly_name: "Vent-Axia Remote"
  min_version: 2024.11.0

esp8266:
  board: esp12e
  framework: {version: recommended}

logger:
  level: DEBUG
  baud_rate: 0          # UART0 is the MVHR link

api:
  encryption: {key: !secret api_key}
ota:
  - platform: esphome
    password: !secret ota_password
wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password
  ap: {ssid: "Vent-Axia Remote Fallback", password: !secret fallback_password}
captive_portal:

external_components:
  - source: {type: git, url: https://github.com/FatBeard/esphome-vent-axia, ref: v1.0.0}
    components: [vent_axia]

uart:
  id: vask_uart
  tx_pin: GPIO1        # ESP8266: UART0, shared with USB programming
  rx_pin: GPIO3
  baud_rate: 9600
# On ESP32 this is the only block that changes — e.g. tx_pin 17 / rx_pin 16 on
# UART1, which also lets `logger:` keep its serial console.

time:
  - platform: homeassistant
    id: ha_time
    timezone: Europe/Dublin      # pinned: build host is Etc/UTC, no DST rules
    on_time:
      - seconds: 0, minutes: 30, hours: 4
        then: {vent_axia.fetch_diagnostics: {id: vask}}
      - seconds: 0, minutes: 45, hours: 4        # the bypass settings, nightly
        then: {vent_axia.read_settings: {id: vask}}
      - seconds: 0, minutes: 5, hours: 4, days_of_week: SUN
        then: {vent_axia.sync_clock: {id: vask}}

vent_axia:
  id: vask
  uart_id: vask_uart
  time_id: ha_time

sensor:
  - platform: vent_axia
    airflow:                {name: "Airflow"}
    boost_time_remaining:   {name: "Boost Time Remaining"}
    supply_airflow_set:     {name: "Supply Airflow Setpoint"}
    supply_motor_pwm:       {name: "Supply Motor Drive"}
    supply_fan_rpm:         {name: "Supply Fan Speed"}
    extract_airflow_set:    {name: "Extract Airflow Setpoint"}
    extract_motor_pwm:      {name: "Extract Motor Drive"}
    extract_fan_rpm:        {name: "Extract Fan Speed"}
    supply_air_temp:        {name: "Supply Air Temperature (To House)"}
    extract_air_temp:       {name: "Extract Air Temperature (From House)"}
    indoor_temp:            {name: "Indoor Temperature (Unit Sensor)"}
    indoor_humidity:        {name: "Indoor Humidity (In Extract Air)"}
    indoor_humidity_avg:    {name: "Indoor Humidity 5-Minute Average"}
    filter_hours:           {name: "Filter Hours Remaining"}

binary_sensor:
  - platform: vent_axia
    summer_bypass:      {name: "Summer Bypass Active"}
    boosting:           {name: "Boost Active"}
    purging:            {name: "Purge Active"}
    defrost_active:     {name: "Defrost Active"}
    dryout_active:      {name: "Dryout Mode"}
    antifrost_active:   {name: "Frost Protection Active"}
    filter_change_due:  {name: "Filter Change Due"}
    supply_temp_fault:  {name: "Supply Air Sensor Fault (T1)"}
    extract_temp_fault: {name: "Extract Air Sensor Fault (T2)"}
    rail_24v_fault:     {name: "24 V Rail Fault (Fuse FS1)"}
    wireless_fitted:    {name: "Wireless Receiver Fitted"}
    switch_line_1:      {name: "Wall Switch SW1 (Boost Input)"}
    switch_line_2:      {name: "Wall Switch SW2 (Boost Input)"}
    switch_line_3:      {name: "Wall Switch SW3 (Boost Input)"}
    link_up:
      name: "MVHR Link"
      on_press:                          # the boot-time read — see below
        then:
          - delay: 15s
          - vent_axia.read_settings: {id: vask}
    busy:               {name: "MVHR Busy"}

text_sensor:
  - platform: vent_axia
    status_message:     {name: "Status Message"}
    antifrost_mode:     {name: "Frost Protection Mode"}
    unit_clock:         {name: "MVHR Clock (Unit Time)"}
    serial_number:      {name: "MVHR Serial Number"}
    firmware_version:   {name: "MVHR Firmware Version"}
    diagnostics_updated:{name: "Diagnostics Last Updated"}
    display_line_1:     {name: "Display Line 1 (Top)"}
    display_line_2:     {name: "Display Line 2 (Bottom)"}

select:
  - platform: vent_axia
    airflow_mode:
      name: "Airflow Mode"    # Normal / Boost 30 min / Boost 60 min / Purge

switch:
  - platform: vent_axia
    summer_mode: {name: "Summer Mode (Enable Bypass)"}

number:
  - platform: vent_axia
    bypass_indoor_temp:  {name: "Bypass Minimum Indoor Temperature"}
    bypass_outdoor_temp: {name: "Bypass Minimum Outdoor Temperature"}

button:
  - platform: vent_axia
    fetch_diagnostics: {name: "Refresh Diagnostic Sensors"}
    read_settings:     {name: "Refresh Summer Settings"}
    sync_clock:        {name: "Sync MVHR Clock"}
    reset_filter:      {name: "Reset Filter Timer"}
    key_up:            {name: "Key Up"}
    key_down:          {name: "Key Down"}
    key_set:           {name: "Key Set"}
    key_main:          {name: "Key Main"}
```

**When the settings get re-read is YAML's decision, not the component's.** `summer_mode` and the two bypass temperatures are deliberately not optimistic and deliberately do not restore from flash — the unit is the sole source of truth — so a value only ever arrives from a `ReadSettings` run. Three things start one, and they are three different mechanisms on purpose: the `read_settings` button (a human), `WriteSetting`'s own read-back (after every change made from here), and the two automations above (nightly, plus once at boot). The boot read hangs off `link_up`'s `on_press` rather than `esphome.on_boot` because `Runner::request()` refuses to start anything while the link is down, and at `on_boot` time it always is — the link only comes up once frames are arriving. A link recovery later re-reads too, which is harmless: a refused request is logged, never queued. `vent_axia.read_settings` exists as an action for exactly this — the component knows *how* to read the settings and has no opinion on *when*.

**Modelling choices.** Boost becomes a `select` (`airflow_mode`) rather than four buttons: the unit's Main key is a cumulative counter, so "boost for 60 minutes" is inherently a *set-absolute* operation that must normalise first — that is a state with a value, which is what a select is. Bypass stays a `switch` because Summer Mode genuinely is a two-state setting. The two temperatures stay `number`s. The raw key buttons are kept as an escape hatch but are now buttons (taps) rather than hold-switches, so nothing can be restored-on into a stuck key.

---

## 7. Safety and failure handling

- **Key watchdog** in `Keypad`, independent of the sequence engine: 30s hard release.
- **Sequence timeout** per root sequence in `Runner`; `on_finish(FAILED)` → `recover()`.
- **Reboot mid-sequence** leaves the unit's display in a menu; the unit's own 2-minute timeout closes any editor without committing. A `ReadSettings` run at boot re-synchronises from any position, since it starts with `GotoMenu(0)`. Not on `on_boot` as originally written here — that fires while the link is still down and `Runner::request()` refuses it — but 15s after `link_up` goes true; §6 has the reasoning and the YAML.
- **Link loss**: no valid frame for 30s → `link_up` false and sequences refuse to start. Replaces "infer liveness from line2 having stopped republishing".
- **Diagnostic page 27 (`Reset`)** is untried and writes. Set is *interlocked off* while the display shows a diagnostic page — asserted globally in the test suite, not just avoided by convention.
- **Filter reset** verifies the status screen first, is `entity_category: config`, and self-verifies against page 23 afterwards.
- **`read_only: true`** hub option mutes the keypad entirely — the same production firmware, transmitting nothing (§8, stage 1).

---

## 8. Verification

**Host tests** (`tests/`, CMake + doctest, no ESPHome checkout) are the primary safety net, and are why the core files avoid ESPHome headers:

- `test_protocol` — CRC against captured 41-byte frames; TX frames must byte-match `04 06 FF FF FF 10 FC E8` and the four key frames; resync after an injected dropped byte.
- `test_parser` — the blank≠zero surface: `"   C"` must not parse, `"018 029 % 0994  "` must; signs; short lines; clock rendering; status-line grammar (`18%`, `48%       30m`, `Purge      120 m`).
- `test_diagnostics` — feed each page's captured line, assert exactly which keys published: p4 `00 % 00 C` publishes nothing; p19 `1` → `rail_24v_fault = false`; p23 `00000` → `0` **and** `filter_change_due = true`; p24 `10` → `"Bypass"`.
- `test_sequence` — the engine and each sequence driven against a fake keypad and display with explicit `now_ms`: `on_finish` runs on every exit path (success, child failure, timeout), a second root is refused while one runs, `GotoMenu` issues 5 Up then N Down taps, `LeaveMenu` issues exactly one, `FetchDiagnostics` releases and settles rather than holding through, and no key is left asserted after any sequence ends however it ended.

### Out of scope (descoped during implementation)

- **`fake_mvhr`, the host model of the unit's menu behaviour.** Dropped. The sequence tests drive a fake keypad and display directly instead, which covers the engine's own contract — `on_finish` always running, mutual exclusion, no key left asserted — but *not* the unit's behavioural quirks (the edit chain, wrapping vs non-wrapping fields, the blink, dropped presses). Those remain verified only by reading, so the sequences that write settings carry more risk than the rest of the codebase.

### The staged rollout, as it happened

This section was written as a plan and is now a **record**: the rollout is underway on the live unit, not hypothetical. Extend it as each remaining stage goes live rather than treating it as pre-rollout planning. These rollout stages are numbered independently of the *build* stages the source comments count (`// Stage 5:` in `entities.h` and the YAML means "the fifth thing built", not "the fifth thing flashed").

The live device at 192.168.1.200 runs this component. `mhrv_orig` no longer ventilates the house; it is now purely a reference. A known-good rollback build of it lives in `../rollback/`, and keeping it there is the whole safety net — restore it and the unit is back on the old firmware.

| # | Stage | Status |
|---|---|---|
| 1 | Read-only soak, `read_only: true` | **Done, 11 Aug 2026.** 145 frames, 0 bad-CRC, decode tracked the live display. `read_only: false` since. |
| 2 | `FetchDiagnostics` | **Live.** Also on the 04:30 daily schedule. |
| 3 | `ReadSettings` | **Live.** Manual button. |
| 4 | `SyncClock` | **Live.** Also on the Sunday 04:05 schedule. |
| 5 | `WriteSetting` — `summer_mode`, the two bypass numbers, the raw key buttons | **Live.** The first stage that writes settings. |
| 6 | `airflow_mode` (`SetAirflowMode`) | **Live, 12 Aug 2026.** Normal → Boost 30 min → Normal exercised from HA; both transitions clean (~8.6s and ~9.6s of `busy`), no cancel hold involved since neither leg started from Purge. |
| 7 | `ResetFilter` | **Live, 12 Aug 2026.** Irreversible; last on purpose. Filters had just been cleaned. Full run (hold → self-verifying `FetchDiagnostics`) took ~25s, well inside the ~65s worst case; log: `ResetFilter: reset confirmed, 8712 hours to go`. |
| 8 | Entity registration fix, and `ReadSettings` on a schedule | **Built, not yet flashed.** The first defect found by *using* the component rather than by review — see below. |

What was validated against hardware when stages 6 and 7 first went live, and what still isn't:

- **`SetAirflowMode`.** The Normal↔Boost 30 leg is now confirmed live; a 5.5s Main hold actually cancelling a running purge is **still untested** — that only happens when a target is selected while Purge is already showing, which hasn't been exercised yet. The sequence refuses loudly rather than guessing if Purge is still showing after the cancel hold, so that failure is diagnosable from the log rather than silent. The purge screen's layout is still unresolved (risk 4), so the decode scans both lines. The 30-vs-60 latch (a 60-minute boost counting down through the same 1–30 range a 30-minute one shows) is also still unexercised — only Boost 30 has been tried live.
- **`ResetFilter`.** Resolved: this unit does **not** answer the Up+Down hold with a "Reset Filter?" prompt. The post-hold log line read `ResetFilter: after hold, line1='Low Airflow     ' line2='18%             '` — the ordinary status screen, unchanged — so the follow-up keypress the sequence was prepared to refuse rather than guess at was never needed. The self-verification against page 23 read the fourth of its four possible outcomes, "confirmed" (8712 hours), the maximum interval.

**Stage 8** was not planned. It came from the observation that the two bypass temperature sliders showed no value in Home Assistant — not a stale one, none at all — even immediately after being used to change a setting:

- **The cause was codegen, not the sequence engine or the decode.** `number.py`, `switch.py` and `select.py` wired the entity→hub half of §5's two-directional pattern and never the hub→entity half, so `numbers_[]`, `switches_[]` and `selects_[]` were `nullptr` for their entire lives. Every publish helper nullptr-checks its slot, so `ReadSettings` walked the menu, parsed the values correctly, emitted its `ReadSettings: Bypass minimum indoor temperature is N C` line — and then discarded the value one call later. The log telling the truth while the entity showed nothing is what would have made this hard to chase from the symptom alone. Writes were unaffected, because those travel the half that was wired. The symptom was therefore "a control that commands the unit perfectly and never reports back", across `summer_mode` and `airflow_mode` as well as the two numbers — `airflow_mode` worst of the three, since the hub recomputes it from the passive status decode ~3 times a second and had nowhere to put the answer. The fix is one `cg.add(hub.set_*(...))` per platform; §5 now states the rule that was missing.
- **Neither safety net could have caught it.** The host suite never runs codegen, and `esphome config` does not either. What *would* catch it is a check that the generated `main.cpp` contains a `set_number`/`set_switch`/`set_select` call per configured entity — this fix was verified that way by hand, and there is no automation for it. Worth remembering that the tests cover the C++ and nothing covers the Python.
- **Fixing the publish path only exposed the second half of the problem**: nothing ever *started* a `ReadSettings` run on its own. The three entities do not restore from flash, so a reboot left them blank until a human pressed the button. Hence `vent_axia.read_settings` (§6) and the two automations that now use it, nightly and 15s after `link_up`.

`v1.0.0` is still not tagged, and `mhrv/mhrv.yaml` still points at the component through a local path rather than the pinned git ref its commented-out block shows. Stage 7 working on the unit was the condition for tagging, and it is met — but stage 8 arrived first and has not been flashed yet, so the order now is: flash stage 8, confirm the three controls actually report state on the live unit, *then* tag `v1.0.0`. Tagging a version whose every control entity is write-only would be a poor first release.

---

## 9. Risks

1. **`loop()`-based TX jitter is a real regression from the timer.** ESPHome's `loop()` can stall tens of ms during Wi-Fi reconnects, API bursts and OTA. A stall inside a 50ms press could emit one frame instead of three — exactly the silent dropped-press that `gap_ms: 400` was raised to eliminate. Mitigations: send a frame immediately on mask change so a press is never zero-frame, and log a warning when a press emits fewer than two frames, converting an invisible failure into a diagnosable one. If stage 3 shows drops, raise `tap_duration` to 100ms (the auto-repeat threshold is above 260ms, so there is headroom). **Keep reinstating a timer as an open option.**
2. **Continuous boost is unsupported by decision, not oversight** (§4). Residual consequence: continuous boost set at the unit's own keypad shows as `boosting` true with `airflow_mode` reading `Normal`. Accepted; document it in the README. This removes what would otherwise have been the design's least reliable decode.
3. **`airflow_mode` transitions are slow and asymmetric.** Purge → Boost 30 is a 5.5s cancel hold, an 8s probe, up to four normalising taps with probes, then one tap: ~25–30s during which HA shows the old value. The `busy` binary sensor exists to surface this.
4. **The purge screen layout is not established** — the notes record `Purge      120 m` / `100%` without settling which line is which. Parser accepts `Purge` on either line; the countdown mapping is a guess until stage 6.
5. **Auto-recovery cannot distinguish us from a human at the unit's keypad.** Gating recovery on a long-unchanged menu screen makes a collision unlikely, not impossible. Scheduled work is at 04:05/04:30.
6. **`Outdoor Temp`'s real range is unknown** (14 °C observed; 5–25 is a guess) and the open question from the old notes stands: whether it is a pure setpoint or also *reports* something. The stated test — write a distinctive value, check it at the next daily read with nobody touching the unit — was written as falling out of stage 5 for free, but there was no daily read until stage 8 added one (04:45) and no way to see the result until the same stage fixed the publish path. It is genuinely available now, and is the obvious first thing to do with the fix once it is flashed.
7. **A second dongle (ESP8266 + RJ9 lead) is the cheapest real risk reduction**, removing "the house's ventilation is the dev target". It would pay for itself by stage 5.

---

## 10. Execution model

Implement with **Sonnet**, review with **Opus**, per unit of work rather than in one pass at the end — a review that arrives after ten files are written is a rewrite, not a review.

The unit of work is a stage from §8's build order (roughly: protocol+display, parser+status, diagnostics table, keypad, sequence engine, then one sequence at a time). For each:

1. **Sonnet implements** the stage plus its host tests, working against this plan and the reference files below.
2. **`tests/` must be green** before review — a failing suite is a bug report, not a review request.
3. **Opus reviews** the diff, weighted toward the things host tests cannot catch: whether the sequence reads clearly enough to reason about, whether an invariant from §3 was preserved *with its comment* rather than silently dropped, whether a failure path leaks a held key, and whether anything platform-specific crept in (§2.1).
4. Findings are applied before the next stage starts.

Two review gates get extra weight because a mistake there is expensive:
- **The sequence engine** (§2), because every later sequence inherits its shape — reviewing it after five sequences exist is too late.
- **Anything that presses Set**, from stage 3 on, because Set is the only key that writes.

The `/code-review` skill covers the per-stage pass. Reserve `/code-review ultra` for the sequence engine and for the pre-`v1.0.0` sweep.

---

## Reference files

New code, so these are read-while-writing rather than modified:

- `mhrv_orig/summer_bypass.yaml` — edit-chain, open-editor-with-retry and closed-loop-adjust logic → `seq_write_setting.cpp`. The comments carry the reasoning for each guard; port those too.
- `mhrv_orig/controls.yaml` — `fetch_diagnostics`, `sync_clock`, `boost_*`, `goto_menu`/`leave_menu`, watchdogs → `seq_*.cpp` and `sequence.cpp`'s recovery.
- `mhrv_orig/vask_decode.h` — parsing rules that move near-verbatim into `parser.cpp`.
- `mhrv_orig/diagnostic_sensors.yaml` — the 28-page field map → the `DIAGNOSTIC_PAGES` table.
- `mhrv_orig/.esphome/external_components/fbfb4ebe/components/vent_axia_sentinel_kinetic/vent_axia_sentinel_kinetic.cpp` — the only known-good RX framing / CRC / TX frame construction. Re-derive in `protocol.cpp` and check byte-for-byte against it.
- `mhrv_orig/vent-axia-esphome-project.md` — captured page contents and status-line examples → the fixtures in `tests/`.
