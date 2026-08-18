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
- **Continuous boost: supported since 13 Aug 2026.** Reported and commandable (§4, §8 stage 9). The original "not exposed, not detected" decision was retired after its premise was re-tested on the live unit.
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
| `SetAirflowMode` | Normalise boost (tap Main up to 4×, probing between — line1 alternates, so the probe needs up to 8s) then apply N presses. Targets are **Normal / Boost 30 / Boost 60 / Boost Continuous / Purge** (continuous added 13 Aug 2026, §4). Normalising still passes *through* other boost states on its way round the counter, which is transient and harmless — `publish_airflow_mode_()` suppresses publishing entirely while this sequence runs. Purge = 5500ms Main hold. |
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

Undecoded pages are exposed behind a single optional `raw_diagnostic_page` text sensor plus an `on_diagnostic_page` trigger, so future decoding needs no component change — rather than 28 always-present internal text sensors. Page 5 left this list on 13 Aug 2026 (stage 10) and page 20 on 14 Aug 2026 (see below); the rest, audited 13 Aug 2026, are below with the reason each is still out and what it would take to bring it in. The point of writing the reasons down rather than just the page numbers: "not fitted on this unit" and "never tested in the state that would show anything" look identical from the outside, and only one of them is a finished decision.

| Page | Meaning | Captured | Why undecoded |
|---|---|---|---|
| 9, 10 | SW4 / SW5: raw, closed (pos 5), momentary time | `1022 0 25 00    ` | Not wired — see below. **RESOLVED 14 Aug 2026** (§8 stage 11): tested against all three of the house's switched lives and never moved. |
| 12–16 | Wireless T0–T4 timers, security PIN digits (`10` = not set) | `000           10` | No wireless receiver, no PIN. Page 11's fitted flag reads 0, which corroborates it. |
| 17, 18 | P1/P2 plug-in sensor: raw, type (0 RH / 1 CO2 / 2 T), scaled | `0000 0 000      ` | No plug-in sensors fitted; all-zero. |
| 21, 22 | Pressure sensor 1 / 2, raw | `0000            ` | Not fitted; all-zero. |
| 27 | `Reset` — "press Set to reset" | `Reset           ` | Deliberately never read *or* pressed; see §7 and the `PAGES` table comment. |

**Page 20 decoded 14 Aug 2026 — as text, not as a claim about meaning.** Unlike pages 9/10 below, this was not a re-test of a wrong assumption; it was always legible (`0410 1          `, tri-state at column 5), just unclaimed because nothing on this unit was known to consume it. It is now `west_link_state` (`TextKey::WEST_LINK_STATE`, `diagnostics.cpp`'s `page20_link_hook`), publishing the manual's own labels — "West" / "Link" / "No Link" for values 2/1/0, `"State N"` otherwise — verbatim. What those labels mean for this unit's actual network topology is still not established; the entity exists for visibility, same reasoning as `antifrost_mode`, not because a consumer is known.

**Pages 9/10 looked like a lead — RESOLVED 14 Aug 2026, and it wasn't one.** The reasoning for treating them as open was sound at the time: every other row above justifies itself with an all-zero capture or absent hardware, while 9/10 captured `1022 0 25 00` — nonzero raw, and a momentary time of 25 — against pages 6/7/8's inert `0000 1 0 000 00 `, and the "SW4/SW5 not wired" note was inherited from `mhrv_orig` without ever being checked against an asserted switch. Stage 11 (§8) ran that check across all three of the house's switched lives, in the same scrapes that nailed down page 05. Pages 9/10 read `1021-1022 0 25 00` / `1020-1021 0` in every one of them, switched live asserted or not, and the small drift between samples (1020→1021, 1022→1021) matches a free-running counter incrementing between reads, not a per-switch or any-switch flag. So the original "not wired" note turns out to have been right, just for the wrong reason (nobody had checked, not "checked and confirmed"). Left undecoded, now for a settled reason rather than an open one.

Undecoded *fields* on pages that are decoded, for completeness: page 2/3's fault code is collapsed to a boolean (1 s/c vs 4 o/c on T1, 2/8 on T2, discarded — a problem sensor is enough to alert on and the code stays visible raw); page 4's sample timer; page 5's cols 2-3, 5-6, 8, 10-11 and 13-15 (§8 stage 10 covers each); pages 6/7/8's raw, link state, west % and west time; page 11's raw, rx nibble count, rx byte and timer; page 24's status, countdown minutes and stored temperature, plus a pre-heater % that is documented but blank here. All are internal sampler state with no consumer — listed so that "we looked and chose not to" is distinguishable from "nobody looked."

**New capability**: the status line is decoded too, which the old setup never did. `airflow %`, `boost_time_remaining` (live from `30m`/`29m`/…), purge countdown, and the status message. Boost time remaining was on the explicit wishlist.

**Sensor-boost annunciator decoded — 17 Aug 2026.** Line2 column 15 (the last of 16) carries the manual's alpha symbol — "If the installation has proportional sensors or an internal humidity sensor fitted, and any of these are boosting the airflow, an alpha symbol will be displayed" — rendered `*` by `sanitize()` (display.cpp) since alpha is not printable ASCII. `has_sensor_boost_annunciator()` (status.h/status.cpp) checks column 15 specifically, not a line scan, because sanitize()'s mapping is many-to-one (any custom glyph anywhere on the line collapses to the same `*`); the column was measured, not guessed, from a captured screenshot at 45.4 px/char pitch. Exposed as `humidity_boost` (§8 stage 14) — named for this unit's actual hardware (pages 17/18 read all-zero, no proportional sensor fitted) while the entity comment records that the manual's alpha also covers proportional 0-10V sensors on P1/P2. Backed by a sticky `Flag` on `ALTERNATION_TIMEOUT_MS`, not a direct read, because stage 10's `ls` annunciator already proved this same right-hand zone of line2 can blink transiently. **Confirmed against a real humidity boost the same evening**: `36%            *` on the live display with humidity at 74%, line1 alternating `Normal Airflow` ⇄ `Summer Bypass On` and never `Boost Airflow`, the annunciator steady rather than blinking, and the rate modulating (36% → 34%) while everything else held. See §8 stage 14 for the full evidence, and §9 risk 8 for the `continuous_boost()` question this raised and that capture then largely closed.

**Continuous boost is decoded and commandable — REVISED 13 Aug 2026.** This paragraph previously ruled it out of scope on two grounds, both re-tested against the live unit and both found not to support the conclusion:

1. *"shows only an airflow percentage indistinguishable from a high normal rate"* — true of **line2 only**, and line2 was never the discriminator. Line1's `Boost Airflow` is. No part of the decode needs an airflow-percentage threshold, so the ambiguity is not on the path.
2. *"would rest entirely on catching `Boost Airflow` in a ~3.4s alternating status line"* — factually true, but the design **already** rests three things on that same catch: the `boosting` binary sensor, `airflow_mode`'s entire Boost-vs-Normal split (including the 30-vs-60 latch), and `SetAirflowMode`'s `PROBE_CHECK`/`PROBE_WAIT` normalise probe. Measured on the unit: `Boost Airflow` reappears every 6-8s against a 12 000 ms sticky timeout, and line2 does **not** alternate — it held `48%       28m` unchanged across ~9 full cycles. Continuous boost was never a *new* dependency on a fragile signal; it is the identical dependency, minus a countdown.

The decode is `boosting() && !purging() && no countdown seen at any point in this boost episode`, confirmed over `StatusTracker::CONTINUOUS_CONFIRM_MS`. **That constant must exceed `ALTERNATION_TIMEOUT_MS`** — the one real hazard the original reasoning missed. At a timed boost's expiry the countdown vanishes while `boosting_` stays sticky-true for up to the alternation timeout, so a shorter window would report continuous boost on *every* timed-boost expiry. Measured 14.0s from the last `Boost Airflow` frame to `boosting()` dropping, hence 20 000.

`boost_time_remaining` still stays unpublished during continuous boost — correct, since there is no countdown.

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
      - seconds: 0, minutes: [0, 15, 30, 45]   # every 15 min, wall-clock aligned
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
    # Page 05 col 0, added stage 10 -- the only page that reports a wired
    # switched live. The three SW flags above never do; see stage 10.
    switched_live_boost: {name: "Switched Live Boost Input"}
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
| 8 | Entity registration fix, and `ReadSettings` on a schedule | **Live, 12 Aug 2026.** The first defect found by *using* the component rather than by review — see below. All four control entities confirmed reporting state on the unit, and a slider round-trip confirmed end to end. |
| 9 | Continuous boost decoded and commandable | **Implemented 13 Aug 2026**, from live observation of 192.168.1.200 — see §4 and the stage 9 notes below. Not a planned stage: it came from re-testing risk 2's premise on the hardware rather than from the roadmap. |
| 10 | Switched-live boost: page 05 decoded, and `SetAirflowMode` bails early | **Implemented 13 Aug 2026**, from a user report that cancelling a toilet boost did nothing — see the stage 10 notes below. Settles stage 9's open item in the negative and replaces risk 2's residual with a measured one. |
| 11 | Pages 9/10 checked against all three switched lives | **Done 14 Aug 2026** — see the stage 11 notes below. Negative result: confirms the free-running-counter reading, no code change. |
| 12 | Purge screen layout confirmed; Purge cancel-hold exercised live; Outdoor Temp write path stress-tested; page 20 decoded | **Done 14 Aug 2026** — see the stage 12 notes below. |
| 13 | Decode hunt: does any diagnostic page carry summer bypass's real damper state (not just the status line)? | **Done 14 Aug 2026, negative result** — see the stage 13 notes below. No dedicated flag found. |
| 14 | Decode the `*` annunciator at line2 column 15 as `humidity_boost` | **Implemented 17 Aug 2026** from a user-supplied screenshot of the card's own LCD panel (`lovelace-card/IMG_3778.jpeg`), and **confirmed live the same evening** during a real humidity boost — see the stage 14 notes below. Line1, `boosting`/`airflow_mode` and blink-vs-steady are all now answered; the trailing edge (the annunciator clearing) is the one part still unobserved. |
| 15 | Raw-byte instrumentation for the display capture — no decode change | **Implemented 18 Aug 2026.** `describe_unprintable()` (display.h/.cpp) and a gated `log_raw_frame_bytes_()` (vent_axia.h/.cpp) log every byte `sanitize()` collapses to `*`, plus the six never-read `unknown_*` frame bytes, at DEBUG — instrumentation only, built to make one live capture answer questions this project has so far only inferred. See the stage 15 notes below. |
| 16 | The two-lane split: raw bytes for parsing, UTF-8 for presentation — α reaches Home Assistant | **Implemented 18 Aug 2026**, on the strength of stage 15's measurement. `sanitize()` is gone; `Display` keeps a raw lane (every decoder's byte offsets, unchanged) and a UTF-8 presentation lane (`to_utf8()`, built on `glyphs::ALPHA = 0xE0`), transcoding only a line that actually changed. `has_sensor_boost_annunciator()` is now an exact byte comparison. Dedup moved to the raw lane, closing the glyph-to-glyph blindness `DISPLAY-REVIEW.md` §5 identified. See the stage 16 notes below. |

What was validated against hardware when stages 6 and 7 first went live, and what still isn't:

- **`SetAirflowMode`.** The Normal↔Boost 30 leg is now confirmed live; a 5.5s Main hold actually cancelling a running purge is **still untested** — that only happens when a target is selected while Purge is already showing, which hasn't been exercised yet. The sequence refuses loudly rather than guessing if Purge is still showing after the cancel hold, so that failure is diagnosable from the log rather than silent. The purge screen's layout is still unresolved (risk 4), so the decode scans both lines. The 30-vs-60 latch (a 60-minute boost counting down through the same 1–30 range a 30-minute one shows) was **still unexercised as of stage 8, and is now confirmed**: during stage 9's capture the unit sat in a 60-minute boost and `airflow_mode` read `Boost 60 min` from 31m down to 23m, which can only come from `was_boost_60_this_episode_` latching on a >30 countdown.
- **`ResetFilter`.** Resolved: this unit does **not** answer the Up+Down hold with a "Reset Filter?" prompt. The post-hold log line read `ResetFilter: after hold, line1='Low Airflow     ' line2='18%             '` — the ordinary status screen, unchanged — so the follow-up keypress the sequence was prepared to refuse rather than guess at was never needed. The self-verification against page 23 read the fourth of its four possible outcomes, "confirmed" (8712 hours), the maximum interval.

**Stage 8** was not planned. It came from the observation that the two bypass temperature sliders showed no value in Home Assistant — not a stale one, none at all — even immediately after being used to change a setting:

- **The cause was codegen, not the sequence engine or the decode.** `number.py`, `switch.py` and `select.py` wired the entity→hub half of §5's two-directional pattern and never the hub→entity half, so `numbers_[]`, `switches_[]` and `selects_[]` were `nullptr` for their entire lives. Every publish helper nullptr-checks its slot, so `ReadSettings` walked the menu, parsed the values correctly, emitted its `ReadSettings: Bypass minimum indoor temperature is N C` line — and then discarded the value one call later. The log telling the truth while the entity showed nothing is what would have made this hard to chase from the symptom alone. Writes were unaffected, because those travel the half that was wired. The symptom was therefore "a control that commands the unit perfectly and never reports back", across `summer_mode` and `airflow_mode` as well as the two numbers — `airflow_mode` worst of the three, since the hub recomputes it from the passive status decode ~3 times a second and had nowhere to put the answer. The fix is one `cg.add(hub.set_*(...))` per platform; §5 now states the rule that was missing.
- **Neither safety net could have caught it.** The host suite never runs codegen, and `esphome config` does not either. What *would* catch it is a check that the generated `main.cpp` contains a `set_number`/`set_switch`/`set_select` call per configured entity — this fix was verified that way by hand, and there is no automation for it. Worth remembering that the tests cover the C++ and nothing covers the Python.
- **Fixing the publish path only exposed the second half of the problem**: nothing ever *started* a `ReadSettings` run on its own. The three entities do not restore from flash, so a reboot left them blank until a human pressed the button. Hence `vent_axia.read_settings` (§6) and the two automations that now use it, nightly and 15s after `link_up`.

What the flash confirmed on the unit, 12 Aug 2026:

- **The boot read fires and lands.** `link_up` → 15s → `ReadSettings`, whole run ~17s of `busy`, no warnings: `Summer Mode is On` → switch `ON`, `Bypass minimum indoor temperature is 18 C` → `18.00`, `Bypass minimum outdoor temperature is 9 C` → `9.00`. Every one of those publishes was silently discarded before this stage.
- **All four control entities now report state**, read straight off the API rather than inferred from the log: `Airflow Mode = Normal`, `Summer Mode = True`, the two temperatures `18.0` / `9.0`. `Airflow Mode` needed the API check because it never published during the capture — see the boost note below.
- **The slider round-trip, which is the symptom that started this.** Commanded 19 °C over the API exactly as Home Assistant's slider does: the entity held its old value for ~24.5s and then published `19.0` — that being `WriteSetting`'s read-back reporting what was actually on the unit, not an optimistic echo of the command. Restored to 18 °C the same way, confirmed the same way. Worth noting the two runs' log lines came from *different* sinks (`vent_axia:111` for the read-back, `vent_axia:103` for the boot read), which is the two-separate-`ReadSettings`-instances design visible from the outside.
- **Risk 2 turned up live and unprompted.** The unit went into boost partway through the capture with no countdown on the status line — continuous boost, someone or something at the house — so `boosting` went true while `airflow_mode` stayed `Normal`, which is exactly the accepted residual §9 risk 2 describes rather than a fault. It also explains why the select never republished during the capture: its value genuinely never changed. `Outdoor Temp` reading 9 °C is a second observed value for risk 6 (14 °C was the only one before), still inside the guessed 5–25 range.

**Stage 9 — continuous boost**, 13 Aug 2026. Also unplanned. It came from asking whether risk 2's premise still stood, and observing 192.168.1.200 read-only over `GET /events` (which carries entity states *and* the DEBUG log without consuming one of the six native-API connection slots — the better observation channel than `esphome logs`, given mhrv.yaml:20-26's record of slot exhaustion locking everyone out).

- **The measurements that retired the assumption.** Line1 alternates `Boost Airflow` ↔ `Summer Bypass On` on a ~7.0s period (worst gap between successive `Boost Airflow` frames 8s) against `ALTERNATION_TIMEOUT_MS` of 12 000. Line2 does **not** alternate at all — it held `48%       28m` unchanged across ~9 full cycles, moving only on the once-a-minute countdown tick. So "no countdown on line2" is a continuously-present observation, not something that has to be caught in a window.
- **The case turned up live, again, unprompted** — as it had in stage 8. At 21:10:32, with 21 minutes still owed on a running 60-minute boost, line2 went `48%       21m` → `48%` in a single frame while line1 kept alternating unbroken; `boost_active` stayed ON and `airflow_mode` published `Normal`. The cause was an **external toilet switch**, confirmed by the user at the time.
- **SW1/SW2/SW3 read OFF right through it** — see risk 2. The reason given here at the time ("momentary switch, contact already reopened by the time pages 6/7/8 are sampled") was **wrong, and stage 10 disproved it**: the switch is a light-linked switched live that stays asserted for the whole boost, and pages 6/7/8 still read OFF. Those pages never report this input at all. Page 05 does.
- **The overrun measured 10m20s** (21:10:32 → 21:20:52). Continuous boost is a ten-minute state occurring several times a day in this house, not a fleeting one — which is most of why decoding it is worth anything.
- **The trailing edge, which set the constant.** Last `Boost Airflow` frame 21:20:49; boost ended 21:20:52 with line1 → `Low Airflow` and line2 → `18%` **in the same frame**; `boost_active` → false at 21:21:03. That is 14.0s from last boost frame to flag drop against a nominal 12s, the extra ~2s most likely client-side SSE/timestamping jitter. `CONTINUOUS_CONFIRM_MS` is 20 000 rather than 15 000 because a false-positive guard should not sit 1s clear of an observed edge on the strength of an assumption about where jitter came from.
- **Open, and needing a human at the switch**: whether Main presses can clear a switch-driven overrun boost at all. If they cannot, selecting `Normal` during one exhausts `SetAirflowMode`'s normalise guard and logs the refusal. Worth settling, because it decides what the README promises. **Settled the same day by stage 10 — they cannot**, and the guess about what would happen turned out to be exactly right.

**Stage 9 went live the same evening.** Flashed over OTA at 22:08 with the unit idle at Normal/18%, and all three commandable legs exercised end to end:

| Time | What happened |
|---|---|
| 22:12:02 | `Boost Continuous` commanded; `busy` on |
| 22:12:10 | taps land in order — line2 `48%       30m`, then `48%       60m` |
| 22:12:11 | third tap: line2 `48%` (no countdown), `boosting` true, `boost_time_remaining` → null, `busy` off (~9s run) |
| 22:12:31 | `airflow_mode` → **`Boost Continuous`** — exactly 20s after the countdown vanished |
| 22:14:16 | `Boost 30 min` commanded; normalises to `18%`, then `48%       30m` at 22:14:30 |
| 22:14:30 | `airflow_mode` → `Boost 30 min` published **immediately** (countdown present, so no confirm wait), no spurious continuous in the transition |
| 22:16:44 | `Normal` commanded; countdown gone 22:16:48, `airflow_mode` → `Normal` 22:16:57, `boosting` false 22:16:59 |
| 22:16:59+ | 90+ seconds of steady Normal with **no spurious `Boost Continuous`** — the trailing-edge regression, confirmed on hardware rather than only in the suite |

Two things worth keeping from that run. The three taps landing as three *distinct* presses (30 → 60 → continuous, visible one after another on line2) is direct confirmation of the `key_gap` reasoning `presses_for_()` cites. And a scheduled `fetch_diagnostics` ran at 22:15:01-22:15:28 squarely inside the Boost 30 — the display parked in the diagnostic pages for ~27s, `airflow_mode` did not republish, and no false continuous appeared: the `is_status_screen` freeze validated in production, not just in `continuous_boost_accumulator_freezes_while_parked_on_a_menu_screen`.

**Operational note found while testing.** This build's `web_server` (v2, ESPHome 2026.7.4) exposes **no per-entity REST endpoints** — `GET /sensor/airflow`, `/select/airflow_mode`, `POST /select/airflow_mode/set` all 404. Only `/` and `/events` exist. `/events` remains the best read-only observation channel, but *commanding* an entity outside Home Assistant needs the native API; `aioesphomeapi` ships inside the `ghcr.io/esphome/esphome` image, so a short `--entrypoint python3` script against `select_command()` is the way in.

**Stage 10 — the switched live**, 13 Aug 2026, the same evening. Unplanned again, and this one came from a user report rather than from observation: *"when I try to cancel a toilet-related boost I get the percentage followed by the string `ls`"*, followed by *"the switch is still on in the toilet when I did this"* and *"boost is not cancelled"*. That is stage 9's open item arriving with a human already standing at the switch.

The switch is a **light-linked switched live** — it stays asserted for as long as the toilet light is on, which is why stage 9's "momentary contact" story was never right. Everything below was captured read-only over `GET /events` except the two commands, both issued through `aioesphomeapi`.

- **Main presses cannot clear a switched-live boost.** Selecting `Normal` ran the full four-tap `NORMALISE_GUARD`, probing between each, and failed with the generic message. line1 alternated `Boost Airflow` / `Summer Bypass On` unbroken throughout, so the unit was alive and cycling its own status loop — it simply ignored the key. The tell is line2: it stayed pinned at `48%             ` and **never once showed `30m` or `60m`**, whereas stage 9's own timeline above shows a healthy normalise visibly walking line2 `30m` → `60m` → bare. The boost is held by the input, not by the tap counter, so walking the counter achieves nothing.
- **`ls` is real, and it is the unit's own.** line2 read `48%           ls` — `48%` at columns 0-2 and the two characters at columns **14 and 15**, i.e. nowhere near the countdown field at 10-12. The component cannot have manufactured it: the frame passed `rx_crc_valid()`, `display_line_2` publishes verbatim, and `sanitize()` only ever maps non-printables to `*`. It appeared only while Main was being tapped, and did **not** appear once across a full commanded-boost cycle (start, run, scrape, cancel). So it is a switched-live-specific annunciator at the right-hand edge — plausibly "live switch", though the expansion is a guess and nothing in the code depends on it.
- **Diagnostic page 05 carries the switched live, and nobody had looked at it.** Pages 6/7/8 are byte-identical (`0000 1 0 000 00 `) asserted and released, so there was no column to retarget there. Page 05 was not in the `PAGES` table at all:

  ```
            0123456789012345
  switched  1 00 05 0 00 030    toilet light on, boosting
  commanded 0 00 00 0 05 000    Boost 30 min from HA, light off
  idle      0 00 00 0 00 000    light off, not boosting
  ```

  The decisive comparison is the middle row. **Column 0 stayed `0` right through a genuine commanded boost** (line2 `48%       30m`, `boost_active` ON, `airflow_mode` `Boost 30 min` in the same capture), so it is a switched-live flag and not a boosting flag. Note the whole page arrives on the ~15-minute scrape, so it is stale by construction: an explanation in Home Assistant, never a precondition for refusing a command.
- **Cols 13-15 are still open, and now open in a sharper way.** Sampled three times across one continuous switched-live episode that evening they read `030`, then `029`, then `030` again. So the field is neither a fixed configured period (it could not have shown 029) nor a remaining countdown (it could not have gone back up). What fits is an overrun timer reloaded for as long as the switch is held, with 029 the one sample caught between a tick and its reload — which would make it a real countdown only *after* the switch releases. Untested, because every sample so far was taken with the light on and the scrape needs ~90s to reach the page. Settling it needs a scrape started immediately after the light goes off. Left undecoded meanwhile: this is exactly the kind of field where a plausible name would have outrun the evidence, twice over.
- **Open: the house has three switched lives, and only one has ever been tested.** Every capture so far came from the same toilet light. Page 05's remaining fields divide into candidates and non-candidates across all nine samples taken that night: cols 2-3 and col 8 were `00`/`0` in *every* state (asserted, commanded, idle), so they are either unused or wait on a condition not yet produced; cols 10-11 tick in every state and are a counter; cols 5-6 read `05` only while asserted, which makes them the likeliest "which input" field — col 0 already carries *that* a switched live is on, so a second field repeating that boolean would be redundant. (`05` is `0b101` if it is a bitmask, which would mean inputs 1 and 3 and does not fit a single light, so either it is not a bitmask or the bit order is not what it looks like.) Pages 6/7/8 are the unit's own per-switch pages and are the natural home for per-switch detail, but they were byte-identical asserted and released, so this is probably not there. **The test**: assert one of the *other two* switches alone for ~2 minutes and scrape. Cols 5-6 changing identifies the input; cols 2-3 or col 8 going nonzero makes them per-switch flags worth their own entities; nothing but col 0 moving means col 0 is an "any switched live" aggregate — in which case `Switched Live Boost Input` is still named honestly, having never claimed to be per-switch. **Watch pages 09 and 10 in the same scrape** (added 13 Aug 2026, from the §4 audit of what else is undecoded): they are SW4/SW5, they are the only undecoded pages on this unit that captured anything nonzero (`1022 0 25 00    `, against 6/7/8's inert `0000 1 0 000 00 `), and their "not wired" note is inherited from `mhrv_orig`'s page map without ever having been checked with a switch asserted — the same gap that made pages 6/7/8 look settled twice. The scrape already passes through them, so this costs nothing but reading two more rows of the capture.
- **Verified on hardware the same night.** Flashed at 23:0x, then the toilet light went on and the whole path ran end to end: `airflow_mode` → `Normal` refused after **2** Main taps instead of 4, logging *"boost appears held on by an external switched live (a wired wall/toilet switch) -- the display has not moved across 2 consecutive Main taps"*; `Switched Live Boost Input` published **ON** off page 05 (`1 00 05 0 00 029`) while the boost was still running; and line2 flashed `48%           ls` again, exactly as in the original report.
- **The fix is in-band, not on the stale flag.** `SetAirflowMode` now samples `(airflow_percent, countdown_minutes)` at each normalise probe and bails after `STUCK_TAP_LIMIT` (2) consecutive taps that move neither, naming the switched live as the cause. Two rather than one because a probe landing mid-alternation could plausibly repeat a value once on a healthy unit. `NORMALISE_GUARD` stays at 4 as the outer bound — four taps is one full lap of the Normal→30→60→continuous counter, which is what makes an exhausted guard state-neutral, and collapsing the two constants would give that up for counters that *are* moving, just slowly.
- **Two existing tests had to change, and the change is an improvement.** Both `normalise_guard_caps_at_4_taps_…` and `normal_target_from_boosting_start_normalises_through_continuous_…` held line2 completely static because only line1 mattered to what they were proving. That is now literally the stuck-boost signature. They now step line2 the way real hardware does (`30m` → `60m` → bare for the healthy normalise; a nudged percentage for the guard test), which is a more faithful fixture, not a weakened assertion.

**Stage 11 — the other two switched lives**, 14 Aug 2026, closing stage 10's open item. Same method: `fetch_diagnostics` triggered on demand via the `Refresh Diagnostic Sensors` button (native API `button_command()`, `--entrypoint python3` against the ESPHome image) while each switch was held asserted, observed read-only over `GET /events`. Each of the two switches not yet tested was held alone for the whole scrape; a full sweep now completes in well under a minute (~30s button-press to `captured 28 of 28 pages`), not the ~90-120s worst case quoted in stage 10, so there was no need to align the hold with a scrape start time — press, then hold until the log shows page 27 well past.

Second switch, two page-05 samples (down-sweep and return sweep of the same scrape):
```
1 00 05 0 03 030
1 00 05 0 00 030
```
Third switch, four page-05 samples (the scrape parks briefly on page 05 before advancing, yielding more reads than one pass):
```
1 00 05 0 04 030
1 00 05 0 05 029
1 00 05 0 00 030
1 00 05 0 02 030
```

Both closed out the questions stage 10 left open:

- **Col 0 fires for all three switched lives.** `switched_live_boost` and the raw column 0 both read ON/`1` for every switch tested, confirming it as an *aggregate* input flag rather than something specific to the toilet light.
- **Cols 5-6 do not identify the input.** All six new samples across both switches read `05`, identical to every prior toilet-switch capture. Stage 10's "likeliest 'which input' field" guess is wrong: whatever these columns are, they don't vary with which of the three switched lives is asserted. No other page in the sweep varies by input either (see the pages 9/10 resolution above and in §4), so **there is no per-input signal anywhere in the diagnostic pages this unit exposes** — col 0 is confirmed as the whole story, and `Switched Live Boost Input` was already named honestly as an aggregate.
- **Cols 2-3 and col 8 stayed inert (`00`/`0`) for both switches too**, consistent with "unused," not "per-switch flag waiting to be asserted."
- **Cols 10-11 kept ticking non-monotonically** (03→00 on the second switch; 04→05→00→02 on the third) across both tests, confirming the free-running-counter reading over any per-switch-identity reading.
- **Cols 13-15 (the overrun field) stayed at `030`** for almost every sample, with one `029` on the third switch — the same "reloads while held, occasionally caught mid-tick" pattern already documented for the toilet switch, now seen on independent inputs. Still not decoded into an entity; the theory is corroborated, not newly proven, since the light-off transition still hasn't been sampled for any of the three switches.
- **No code or entity changes result from this stage.** The existing decode (`PAGE_5_FIELDS` → `SWITCHED_LIVE_BOOST` only) already matches what all three switched lives actually do; extending it to a per-input field would have needed evidence that doesn't exist. This is the negative-result counterpart to stage 10 — worth recording precisely because "we tested and it doesn't discriminate" looks identical from the outside to "we never tested," and only one of those is a closed question.

**Stage 12 — purge layout, purge cancel, Outdoor Temp write stress test, page 20**, 14 Aug 2026. Four items closed in one session, all via the native API against the live unit with `GET /events`-equivalent state+log streaming for observation.

- **Purge screen layout (risk 4) and the purge cancel-hold (risk 3, stage 6's untested leg) confirmed together.** `airflow_mode` commanded to `Boost Continuous` then `Purge`: `status_message` published `"Purge      120 m"`, `airflow` `100.0`, `boost_time_remaining` `120.0` ticking to `119.0` a minute later — the existing parser's field mapping was already right, nothing to fix. Then, with Purge still running, `airflow_mode` commanded to `Normal`: cancelled cleanly in ~5s (`"Purge      000 m"` → `"Normal Airflow"`, `airflow_mode` → `Normal` at T+17s). Both had been open questions since stage 6/8.
- **The Outdoor Temp write path (the hop through Indoor Temp's editor, §3's `WriteSetting`) was stress-tested live for the first time** — three runs, watching `display_line_1`/`display_line_2` and the DEBUG log throughout: a small 2-step decrease (17→15), a large 9-step decrease (15→6), and a large 11-step increase (6→17). All three landed the target exactly, confirmed by `ReadSettings`' own read-back log line, and Indoor Temp's value was unaffected in every case. One of these runs' *predecessor* (an unlogged first attempt, 8→17) appeared to show Indoor Temp drop from a cached 18°C to 16°C — but the 18°C was HA's stale cached state, not something re-verified that session, and every subsequent fresh `ReadSettings` (triggered incidentally by the later runs) read Indoor Temp as 18°C, because by then it genuinely was — a direct restore had just set it there. The likelier explanation, given three clean reproductions across both directions and both step sizes found nothing: Indoor Temp had drifted to 16°C for a reason unconnected to this component (most plausibly, someone using the unit's own physical keypad in the two days since it was last confirmed at stage 8 on 12 Aug), and the first fresh read that session simply caught HA's cache up to it. **Open, and worth a human check**: if 16°C was a deliberate change made at the unit, restoring it to 18°C undid that — nothing in this session can tell the two apart from the device side alone.
- **Page 20 decoded** — see §4's dedicated note. Unrelated to the live tests above; a pure code/table addition (`west_link_state`), covered by new host tests, both firmware targets recompiled clean.
- **Outdoor Temp's setpoint-vs-report question (risk 6's remaining half) is now set up to resolve itself.** The last live write left it at 17°C; the next unattended 04:45 `ReadSettings` will show whether it holds (pure setpoint) or has moved (also reports something) with nobody touching the unit in between.

**Stage 13 — decode hunt for a diagnostics-page bypass flag**, 14 Aug 2026, negative result. `summer_bypass` (§6) is derived entirely from the status line catching `"Summer Bypass On"`, which never appears during Purge or any menu/diagnostic screen -- so the flag simply times out to false 45s after the display stops showing it, whether or not the damper is actually still open. Asked whether any of the 28 diagnostic pages carries the same information independent of the status line.

Method: with the live unit confirmed bypass-ON (status line alternating `Summer Bypass On`/`Low Airflow`), a `fetch_diagnostics` run captured every page then reachable via the `raw_diagnostic_page` escape hatch. `bypass_minimum_indoor_temperature` was then set to 40°C (well above the unit's real ~25°C indoor reading, so the "indoor too warm" condition guaranteed false) rather than disabling Summer Mode outright -- forcing the same real decision path bypass normally uses, not switching the feature off. The real bypass dropped about a minute later (last `Summer Bypass On` sighting ~21:09:11, confirmed off by the `summer_bypass` flag's own 45s decay at 21:09:56), and a second `fetch_diagnostics` captured the same pages bypass-OFF. The setpoint was restored to 18°C afterward.

- **Page 24's `antifrost_mode` value 10, labeled `"Bypass"` on the manufacturer's sheet, was the obvious candidate and is ruled out.** It read `Off` (mode 0) in both captures, including the bypass-ON one -- so mode 10 is something else, most likely a winter antifrost/defrost-via-bypass state that shares the same damper hardware but not the same trigger as summer economy bypass.
- **Of the 12 pages both captures actually published a fresh value for** (dedup on the text sensor means a page whose content did not change from an earlier scrape does not republish, so only pages both sides freshly reported are directly comparable), 11 were identical bypass-ON vs bypass-OFF: 00/01 (fan RPM jitter only), 08, 11, 14, 17, 20, 23, 26, 27. The 16 pages that appeared in only one capture were all independently explainable by existing decode work (05's free-running counter, 09's pages 9/10 resolution from stage 11, absent-hardware pages 12/13/15/16/18/21/22, page 19's rail fault, page 24 already covered above) -- none of them plausibly bypass-related.
- **The one page that moved**: page 2 (Supply Air Temperature, T1) read `19 C` bypass-ON and `20 C` bypass-OFF -- a 1°C shift in the direction the physical mechanism predicts (bypass routes incoming air around the heat-exchange core, so supply air tracks outdoor temperature more closely when bypass is open, and warms toward tempered/indoor-ish when it closes). Too small a single sample to call decisive on its own -- six minutes of ordinary thermal drift could produce the same delta -- but it is the one candidate consistent with the physical mechanism rather than coincidence.
- **Conclusion**: no diagnostic page carries a dedicated summer-bypass flag on this unit. If this needs settling further, Supply Air Temperature's ON/OFF delta is the lead worth repeating (several forced on/off cycles, larger sample) rather than hunting for a new field -- everything else in the 28-page table is already accounted for.

**Stage 14 — decode the `*` annunciator as `humidity_boost`**, 17 Aug 2026. Unlike every other stage in this section, this one did not start from a live capture: it started from a user-supplied screenshot of the card's own `.lcd` panel (`lovelace-card/IMG_3778.jpeg`), reading line1 `Summer Bypass On` / line2 `31%            *`.

- **Three independent lines of evidence support "this is the humidity sensor boosting the airflow."** (1) The manual, quoted verbatim in §4. (2) Why it renders as `*` and not the literal alpha glyph: the `.lcd` block's radial-gradient ground, text glow and scanline overlay match the screenshot exactly, confirming it is a render of *our own card* rather than the physical LCD, and alpha is not printable ASCII so `sanitize()` maps it to `*` before anything downstream sees it -- the asterisk *is* the alpha. (3) ~~The airflow figure: 31% is neither this unit's Normal 18% nor its Boost 48%.~~ **WITHDRAWN 17 Aug 2026 -- this third strand was wrong and is struck rather than deleted.** 18% is this unit's *Low* airflow rate, not its Normal one, which §8 stage 9 above already records (`line1 -> Low Airflow` with `line2 -> 18%`); the post-flash capture the same evening showed `Normal Airflow` / `30%` with column 15 blank. 31% is therefore one point above the normal rate, not a rate strictly between two others, and is far too small a margin to carry any evidential weight -- consistent with a proportional ramp that has only just started, but indistinguishable at that margin from ordinary variation. The conclusion is unaffected because it never rested on this: strands (1) and (2) are the manufacturer's own documentation and a mechanical explanation of the rendering, and neither involves a percentage. Kept visible because misreading a Low rate as a Normal one is a mistake that would otherwise be repeated -- and because a strand of evidence quietly vanishing between commits is exactly what makes a record untrustworthy.
- **The column was measured, not guessed.** Line1 is a full 16 characters, giving a pitch of 45.4 px/char from `S` at x=161 to `n` at x=845 in "Summer Bypass On". The asterisk's own glyph run sits at x=840-878 -- column 15, the last column. Line2 is therefore literally `31%` followed by 12 spaces and the annunciator.
- **Deliberately not confused with stage 10's `ls`.** Both occupy the same right-hand zone of line2, but `ls` sits at columns 14-15 (two ordinary characters, `l` and `s`) and appeared only transiently while Main was being tapped during a switched-live boost. `has_sensor_boost_annunciator()` checks line2[15] against `*` alone, which `ls` cannot satisfy (column 15 there holds `s`) -- the two are structurally distinct, not merely different in the one capture of each seen so far.
- **One binary sensor, not a new enum**, per the plan this shipped under: the alpha is a boolean annunciator, `airflow_mode` already owns the Normal/Boost/Continuous/Purge enum, and a second derived "boost source" enum would need precedence rules against hardware that has not been tested. Named `humidity_boost` rather than something covering "proportional sensor" too, because pages 17/18 (§4's P1/P2 plug-in sensor pages) read all-zero on this unit -- no proportional sensor is fitted, so the alpha here can only be the internal humidity sensor, and the entity comment records that the name would need revisiting on a unit with a P1/P2 sensor wired in.
- **Backed by a sticky `Flag` on `ALTERNATION_TIMEOUT_MS`, not a direct per-frame read**, even though stage 9 measured line2 as not alternating (airflow_percent_/countdown_minutes_ ride a direct read for that reason): stage 10's `ls` already proved this same right-hand zone of line2 can blink transiently, and a direct read of a blinking annunciator would flap the entity at the key-repeat rate. 12s of trailing lag is cheap against a state that persists for minutes.
- **Built before it was verified, then verified** -- unlike every prior stage in this table, which came from observing 192.168.1.200 directly, this one was built from a single screenshot and the manual's own words, deliberately tested first (host suite plus both firmware targets) and left to be captured during the next actual humidity boost. That boost arrived the same evening (user: "the humidity boost is on at the moment if you want to prove out assumptions"), and the capture below settles all but one of the questions it was waiting on.

**Stage 14, live confirmation — 17 Aug 2026, ~22:09 unit time.** Two `GET /events` captures against the live unit, ~114s and ~30min, with indoor humidity sitting at 74%. Taking the four questions the stage was left open on, in order:

- **The decode is right, on the real display.** `humidity_boost` read `ON` with `display_line_2` at exactly `36%            *` -- `*` at column 15, where the screenshot's pitch measurement put it. Nothing else on the unit *names* the boost: no line1 message, no countdown, no flag on any diagnostic page. **One correction to the commit message's "no diagnostic page", which was too strong:** a diagnostics fetch taken mid-boost read `supply_airflow_setpoint` 34% and `extract_airflow_setpoint` 37% (pages 0/1) against the 42%/48% left over from an earlier scrape, i.e. the boosted *rate* is visible there, and so are the fans that follow it (supply drive 43%, 1366 RPM; extract 45%, 1409 RPM). What no page carries is the *cause* -- nothing distinguishes 34% raised by humidity from 34% for any other reason, which is stage 13's finding about summer bypass repeating in a different field. Column 15 remains the only thing on the unit that says *why*.
- **Line1 never reads `Boost Airflow` -- this settles §9 risk 8, in the safe direction.** Through both captures line1 alternated `Normal Airflow` ⇄ `Summer Bypass On` on the usual ~3s beat and showed nothing else, with `boosting` `OFF` and `airflow_mode` `Normal` throughout. Risk 8's false-`Boost Continuous` hazard needs `boosting_` to go active; on this unit a humidity boost is *silent* on line1, so `ms_without_countdown_`'s `!boosting_.active` reset holds and the hazard never arms. See §9 risk 8 for what remains of it (a *commanded* continuous boost coinciding with high humidity is still untested -- this capture could not exercise that, because no continuous boost was commanded).
- **Steady, not blinking -- the sticky `Flag` is retro-justified as harmless rather than necessary.** `display_line_2` publishes only on an actual string change (`vent_axia.cpp:35`, the `line2_changed` gate), so an annunciator blinking at anything near the ~300ms frame rate would have flooded the stream. Across ~114s it published `36%            *` exactly twice: once at connect, once on return from a `read_settings` excursion through `Set Clock`/`Summer Mode`, with the identical string both times. Line1 alternated ~35 times over the same window while line2 did not move once, re-confirming stage 9's "line2 does not alternate" independently. So a direct per-frame read *would* have worked here; the `Flag` is kept because stage 10's `ls` blink happened under key taps, which this capture never reproduced, and 12s of trailing lag costs nothing against a state lasting many minutes.
- **The rate is modulated, which is the first real evidence of proportional control.** The airflow percentage drifted 36% -> 34% between the two captures (line2 `34%            *`) with the humidity readout still showing 74% and line1 still `Normal Airflow`. A fixed boost rate cannot do that. It is *not* a demonstration that the rate tracks the published `indoor_humidity` figure -- that stayed on the same integer across both samples, so the unit is evidently modulating against something finer than the 1% readout -- but "the percentage moves while nothing else does" is a stronger fact than the withdrawn 31%-vs-30% margin ever was, and it comes from movement rather than from a single reading.
- **The park-freeze was exercised for real, twice.** The `Flag`'s "hold the last status-screen reading while the display is elsewhere" behaviour is what stops a menu excursion being read as the annunciator clearing, and both excursions in these captures put it under load: a `read_settings` run through `Set Clock`/`Summer Mode`, and a full `fetch_diagnostics` sweep of all 28 pages (`mvhr_busy` `ON` for ~20s, line2 cycling through every raw page and back). `humidity_boost` did not republish once across either -- it stayed `ON` throughout and line2 returned to the identical `34%            *`. Worth having watched rather than assumed, because a diagnostics fetch is the longest the display is ever away from the status loop, and `ALTERNATION_TIMEOUT_MS` is only 12s.
- **Still unobserved: the trailing edge.** Nothing has yet caught the annunciator *clearing* -- whether `humidity_boost` drops ~`ALTERNATION_TIMEOUT_MS` after the last `*` frame (the sticky `Flag`'s expected trailing lag, the same shape `boosting()`'s measured 14.0s has) and whether the rate falls back to the 30% normal figure. README's "it clears itself once humidity drops" is the manual's behaviour and the code's intent, not something watched happen. That is the one item to catch on the next boost's end.

**Stage 15 — raw-byte instrumentation for the display capture**, 18 Aug 2026. Instrumentation only, not a decode change: nothing this stage adds changes what any entity reports. Its whole purpose is to make a future capture possible instead of another inference.

`sanitize()` (`display.cpp`) replaces every byte failing `std::isprint()` with `'*'`, which under the C locale is true only for `0x20`-`0x7E` — so `0x00`-`0x1F`, `0x7F` and `0x80`-`0xFF` all collapse onto the same character. Stage 14's `humidity_boost` decode rests entirely on that collapse, and its own commit message already flagged the weakness: "sanitize()'s mapping is many-to-one." Nothing in the component has ever logged a raw byte, so until this stage the alpha annunciator's identity (`0xE0`) was an inference from the HD44780 A00 ROM, never measured on this unit — exactly the kind of reasoning-ahead-of-a-capture that stage 14's withdrawn third evidence strand (18% turning out to be the Low rate, not Normal) already showed is worth paying to avoid repeating.

`describe_unprintable()` is `sanitize()`'s diagnostic counterpart: instead of destroying a non-printable byte it describes it (`"col N=0xXX"`, column = byte index), returning `""` for an all-ASCII line. `VentAxiaHub::log_raw_frame_bytes_()` calls it on the RAW frame text — before `display_.update()` sanitizes it, and never on `display_.line1()`/`line2()`, which would already be post-collapse and defeat the point — plus logs the six frame bytes `protocol.cpp` parses but nothing else has ever read (`unknown_header[0..3]`, `unknown_row1_addr`, `unknown_row2_addr`). Both are gated: the non-ASCII lines on the *formatter's output* changing rather than the raw line (a humidity boost ticks the airflow percentage every frame while the annunciator sits still in column 15, so raw-line gating would re-log the same `col 15=0x??` several times a second), and the unknown bytes on the 6-byte tuple changing — each path also carrying a 2000ms floor, because nobody yet knows whether one of those bytes is a counter or frame-phase value that would otherwise flood a network-only logger at the link's ~3.3 frames/s.

Three of the review's findings were about the instrumentation failing to capture the very thing it was built for, and are worth recording because each is a false negative rather than a crash — the kind that would have been read as an answer. **A floor alone silently loses a change that also reverts inside it**, and question 3 (a cursor or blink attribute) is exactly that shape at the ~350ms editor blink cadence, so a suppressed change is now carried forward and the next eligible line says the bytes moved and came back. **The falling edge was being swallowed**: logging only non-empty descriptions meant the annunciator *clearing* produced no line at all, leaving it to be inferred from an absence — and stage 14's own open item is that trailing edge, so a clear now logs `(none)`. **The line path had no floor at all**, which an open editor blinking a line containing any non-ASCII byte would have turned into ~190 log lines while `LeaveMenu` waits out the unit's ~2-minute timeout. The first frame is deliberately exempt from the floor, so the at-rest baseline is stamped at link-up rather than up to 2s later; the cost of the floor everywhere else is that an edge can be stamped up to 2000ms late, which is worth knowing when reading the capture's timestamps.

**Flashed 18 Aug 2026, and the flash immediately found a fourth defect of the same family — this one only visible on hardware.** A 45s `GET /events` capture right after the OTA showed the unit healthy and *zero* `raw frame` lines. That is what log-on-change is supposed to look like when nothing is moving, but it is also exactly what a broken instrument looks like, and the two were indistinguishable. The cause is that this device's logger is network-only (`baud_rate: 0`, because UART0 is the MVHR link): the first-frame baseline — the deliberately rate-limit-exempt line, the reading everything else is read against — is written a few hundred ms after boot, long before WiFi and the SSE stream exist, so it goes nowhere. If the six unknown bytes are constant, which is the likeliest case, nothing is ever logged again for the rest of the boot. An observer connecting later sees silence and cannot tell "these bytes never move" from "the code isn't running". The same applies to an annunciator that appeared before they connected — the whole capture scenario this stage was built for.

The fix is a `RAW_LOG_HEARTBEAT_MS` (60s) re-emit of an unchanged line, marked `(unchanged -- heartbeat)`. The unknown-byte tuple always repeats; a line's non-ASCII description repeats only while it *has* content, since the tuple's heartbeat already proves once a minute that the function runs. One line a minute costs nothing on a logger that only transmits to a connected client, and it turns two silences into positive statements. Worth recording as a pattern rather than a one-off: every defect this stage produced was the instrument failing to capture what it was built for, and this one was invisible to review, to the host suite and to both compiles — only running it on the unit and reading the resulting silence exposed it.

One capture, during a humidity boost via `GET /events` (the same channel and caution as stage 14 — `esphome logs` has cost a native-API slot lockout before), is meant to settle three questions:

1. Which byte the alpha annunciator actually is (`0xE0` is currently an inference).
2. Which CGRAM slots (`0x00`-`0x07`) this unit uses — relevant to the `0x07` fixture currently asserted as "the Auto glyph" at `tests/test_display.cpp:57`.
3. Whether `unknown_row1_addr`/`unknown_row2_addr` carry a cursor or blink attribute. If either does, it could replace `editor_open()`'s 1200ms staleness heuristic with a direct protocol read — the heuristic CLAUDE.md records as having silently taken a 14°C setpoint to 19°C.

**The capture happened the same day, 18 Aug 2026, ~10:36 unit time — a humidity boost was already running when the heartbeat build was flashed.** 100s of `GET /events`, with `humidity_boost` `ON`, `airflow_mode` `Normal`, `airflow` 36% and `display_line_2` reading `36%            *`:

```
raw frame: line2 non-ASCII: col 15=0xE0
raw frame: unknown_header=0x98 0x42 0x04 0x00, unknown_row1_addr=0x15, unknown_row2_addr=0x16  (unchanged -- heartbeat)
```

- **Question 1 is answered, and the datasheet was right: the alpha annunciator is `0xE0`.** It is no longer an inference. The published `*` at line2 column 15 and the raw `0xE0` are now known to be the same byte on this unit, which is what `humidity_boost`'s decode has been resting on since stage 14. The whole line is otherwise plain ASCII — `0xE0` was the only non-printable byte in either line for the entire capture.
- **Question 3 has a first, partial answer: the six unknown bytes are constant.** One heartbeat line in 100s and no change lines at all, across a `read_settings` excursion that took the display through `Set Clock`, `Summer Mode`, `Indoor Temp` and `Outdoor Temp` and back. So they do not track the screen, and `unknown_row1_addr`/`unknown_row2_addr` are **not** HD44780 DDRAM set-address commands as `protocol.h` guessed — those would be `0x80`/`0xC0`-based and would move per row; these read `0x15`/`0x16` and never moved. What is still untested is the case that matters: no *editor* was open at any point (`read_settings` navigates and reads; only a write opens one), so whether either byte carries a cursor or blink attribute remains open. That needs a capture across a `WriteSetting`.
- **Question 2 is untouched: no CGRAM byte has ever been seen.** Nothing in `0x00`–`0x07` appeared in either line. The `0x07` "Auto glyph" fixture in `tests/test_display.cpp` is still an assumption, and is now labelled as one in both places it appears rather than asserted as fact.
- **The alternation is visible in the log, and reads correctly.** line2's description alternated `col 15=0xE0` ⇄ `(none)` as line1 alternated `Normal Airflow` ⇄ `Summer Bypass On`. That is the status loop, not a blinking annunciator — consistent with stage 14's "steady, not blinking" finding, and exactly the alternation `StatusTracker`'s sticky `Flag` and `ALTERNATION_TIMEOUT_MS` exist to ride out. Read with the 2000ms floor in mind: the log samples that alternation, it does not measure its period.

With `0xE0` measured, the byte→UTF-8 transcode table (the two-lane raw/UTF-8 refactor `DISPLAY-REVIEW.md` recommends) can be built on a measured code for the one glyph that matters today. The CGRAM slots remain unmeasured, so their placeholders stay individually distinguishable and unassigned rather than guessed. `sanitize()` itself, `Display`'s dedup and `has_sensor_boost_annunciator()` are all untouched by this stage — see `DISPLAY-INSTRUMENTATION-PLAN.md` for the full spec and `DISPLAY-REVIEW.md` §6/§7 for the reasoning.

**Stage 16 — the two-lane split: raw bytes for parsing, UTF-8 for presentation**, 18 Aug 2026. Built directly on stage 15's measurement, per `DISPLAY-REVIEW.md` §6's own condition: the transcode table was not built until a capture existed to build it from.

`Display` now keeps two representations of each line instead of one. The **raw lane** (`raw_line1()`/`raw_line2()`) is the bytes exactly as they arrived, one per LCD column — every decoder in `screens::`, `parser::`, `status::` and `diagnostics::` reads this, at the same fixed offsets it always used, because `DISPLAY-REVIEW.md` §5 is right that transcoding in place would silently shift every one of them (UTF-8 is multi-byte, ASCII is not). The **text lane** (`text_line1()`/`text_line2()`) is the UTF-8 transcode of the same update, computed lazily — only for a line whose raw text actually changed, so the steady-state cost is lower than the two unconditional `sanitize()` calls per frame it replaces, not higher. `sanitize()` itself is deleted; `to_utf8()` (display.h/.cpp) replaces it, mapping `0x20`-`0x7E` to themselves, `glyphs::ALPHA` (`0xE0`, the one home for the measured fact) to α (`\xCE\xB1`, U+03B1), and everything else to `<XX>` uppercase hex — nothing from the HD44780 A00 ROM datasheet, exactly as `DISPLAY-REVIEW.md` §6 insisted. `describe_unprintable()` is untouched; it is still the tool that would name whatever a future `<XX>` turns out to be.

Dedup moved to the raw lane, which is not merely a refactor: it closes a real defect. Under the old single-lane sanitize()'d dedup, two DIFFERENT non-printable bytes in the same column — both collapsing to the same `'*'` — produced no change at all, an invisible glyph-to-glyph transition. `test_display.cpp`'s `two_different_non_printable_bytes_in_the_same_column_both_fire_change` is the regression test: it feeds `0x07` then `0xFF` at the same column and asserts the change callback fires both times, which the old sanitize()'d-string dedup could not have done (both bytes collapse to the same `'*'`, so the second `update()` would have seen no textual difference at all).

`has_sensor_boost_annunciator()` (status.h/status.cpp) is now `line2.size() >= 16 && static_cast<unsigned char>(line2[15]) == glyphs::ALPHA` — an exact byte comparison against a measured constant, reading the raw lane directly. The many-to-one ambiguity its comment used to reason around (`sanitize()`'s collapse meaning a whole-line scan could confuse the annunciator with, say, Mode 2's Auto glyph) stops existing rather than being reasoned about: the raw lane cannot conflate two different bytes. Column 15 stays the test, not a whole-line scan — that is where the byte was measured (both the 45.4 px/char screenshot pitch and the 18 Aug live capture), and nothing has ever shown `glyphs::ALPHA` appearing anywhere else on the line, so widening the scan now would be reasoning ahead of a capture again. The comment's hardware history — the px measurement, the withdrawn-31%-evidence record, the stage-10 `ls` distinction, the live confirmations — is preserved; only the collapse-specific reasoning was cut.

Every call site touching `Display::line1()`/`line2()` (37 in the component proper — `sequence.cpp`, every `seq_*.cpp`, `vent_axia.cpp` — plus one more in a test helper, `test_set_airflow_mode.cpp`, that feeds `StatusTracker` in lockstep with production code) was classified into one of the two lanes and the old accessors deleted outright, so nothing compiled until every site had a lane — a compile error at every one of those sites is cheap insurance against a silent default landing in the wrong direction. Decoders, predicates and guards took `raw_*`; every log string and every value that becomes a published entity state took `text_*`. `publish_diagnostic_page_()` is the one site with both: it takes the raw line2, transcodes it exactly once for its two presentation uses (the `RAW_DIAGNOSTIC_PAGE` text sensor and the YAML `on_diagnostic_page` trigger), and passes the raw bytes on to `diagnostics::decode_page()`, which keeps reading raw — every diagnostic page ever observed on this unit is pure ASCII, so this is behaviourally identical to before and stays correct if that ever stops being true.

`test_parser.cpp`, `test_diagnostics.cpp`, `test_screens.cpp` and every sequence test needed zero fixture edits — the proof no byte offset moved. `test_display.cpp` and `test_status.cpp` did, deliberately: `non_printable_glyphs_are_replaced_with_asterisk` asserted the single-lane collapse itself, which no longer exists, and became `raw_lane_keeps_the_byte_text_lane_renders_it`; every `"31%            *"` / `"0000           *"` fixture in `test_status.cpp` held a literal ASCII `0x2A` and now carries `0xE0`, with a new case (`has_sensor_boost_annunciator_false_for_a_literal_asterisk`) asserting that an actual `'*'` at column 15 no longer fires — the real behaviour change stage 16 makes, now that the predicate reads an exact byte rather than a many-to-one collapse.

Neither user-visible outcome is confirmed on hardware yet -- this stage is implemented and compiled, not flashed. What it should produce: `display_line_2` publishing `36%            α` rather than `36%            *`, and the Lovelace card's `.lcd` panel should render it — `el.textContent` is UTF-8-safe with no injection risk, and the `.lcd` font stack (`VT323`, `Share Tech Mono`, `Consolas`, monospace) falls back per character, so α renders even though `VT323` itself carries no Greek glyphs. Confirming this on hardware is the next thing to do: flash, then a `GET /events` capture during a humidity boost should show the raw and text lanes agreeing in the same log — `raw frame: line2 non-ASCII: col 15=0xE0` alongside `display_line_2` publishing the α — which is stage 15's own instrumentation now serving as stage 16's verification instrument.

Explicitly not touched: `editor_open()`'s 1200ms staleness heuristic stays, because stage 15's capture never had an editor open (`read_settings` only navigates and reads) and retiring the heuristic needs a capture across a `WriteSetting`, which writes to the unit and is its own stage. `Framer`'s missing idle-gap reset and the protocol's order-insensitive checksum are unchanged, per `DISPLAY-REVIEW.md` §7's own verdict that neither is worth fixing here.

`v1.0.0` is still not tagged, and `mhrv/mhrv.yaml` still points at the component through a local path rather than the pinned git ref its commented-out block shows. Stage 7 working on the unit was the condition for tagging; stage 8 then arrived and added a second one, that the control entities actually report state. Both are now met on the live unit, so **tagging `v1.0.0` is the next thing to do** — and switching `mhrv/mhrv.yaml` over to the pinned git ref its commented-out block already carries, whenever iteration on the component slows down enough to want the version bump.

---

## 9. Risks

1. **`loop()`-based TX jitter is a real regression from the timer.** ESPHome's `loop()` can stall tens of ms during Wi-Fi reconnects, API bursts and OTA. A stall inside a 50ms press could emit one frame instead of three — exactly the silent dropped-press that `gap_ms: 400` was raised to eliminate. Mitigations: send a frame immediately on mask change so a press is never zero-frame, and log a warning when a press emits fewer than two frames, converting an invisible failure into a diagnosable one. If stage 3 shows drops, raise `tap_duration` to 100ms (the auto-repeat threshold is above 260ms, so there is headroom). **Keep reinstating a timer as an open option.**
2. **RESOLVED 13 Aug 2026 — continuous boost is now supported** (§4, §8 stage 9). This risk previously recorded the accepted residual that continuous boost read as `boosting` true with `airflow_mode` saying `Normal`. Re-testing the premise on the live unit retired it; see §4 for the two grounds and why neither held.

   One claim made here was **falsified outright** and is worth keeping visible: *"`boosting` (and the SW1/SW2/SW3 flags…) is where the truth lives."* The wall-switch half is wrong. A `fetch_diagnostics` ran at device-clock 22:15:23 squarely inside a switch-initiated continuous boost that was still running, and **all three SW flags read OFF**. They are not merely stale (the scrape-interval objection) — they never report it at all. The status-line decode is the *only* live evidence of continuous boost, which argued for decoding it rather than against.

   The *reason* given for that at the time — momentary switch, contact already reopened — was itself wrong, and stage 10 disproved it a few hours later: the switch is a light-linked switched live held for the whole boost, and pages 6/7/8 still read OFF. The input is reported on **page 05 column 0**, a page the table did not cover. Two wrong explanations in a row for the same observation is the lesson worth keeping: the flags being OFF was solid, and both stories invented to explain it were guesses that a five-minute capture would have settled.

   The new residual, in its place: a `Boost Continuous` reading may be switch-driven, and **if it is, it cannot be cancelled from Home Assistant at all** — established on hardware, stage 10, not merely suspected. `SetAirflowMode` now detects it from the unmoving display and fails within ~2 taps naming the switched live, instead of spending four taps to say it does not know. Nothing can clear it but the switch itself.
3. **`airflow_mode` transitions are slow and asymmetric.** Purge → Boost 30 is a 5.5s cancel hold, an 8s probe, up to four normalising taps with probes, then one tap: ~25–30s during which HA shows the old value. The `busy` binary sensor exists to surface this.
4. **RESOLVED 14 Aug 2026 — the purge screen layout is confirmed correct as already parsed.** `Boost Continuous` → `Purge` commanded live via `airflow_mode`: `status_message` published `"Purge      120 m"`, `airflow` published `100.0`, `boost_time_remaining` published `120.0` then ticked to `119.0` a minute later. No parser change was needed — this confirms the existing field mapping was already right, not a guess that needed fixing. Same live session also exercised the other half of this risk, stage 8's untested leg: commanding `Normal` while Purge was still running cancelled cleanly in ~5s (`Purge      000 m` → `Normal Airflow`, `airflow_mode` → `Normal` at T+17s), settling the "5.5s cancel hold actually cancelling a running purge" question risk 3 and stage 8 both flagged as unexercised.
5. **Auto-recovery cannot distinguish us from a human at the unit's keypad.** Gating recovery on a long-unchanged menu screen makes a collision unlikely, not impossible. `read_settings` and `sync_clock` are still confined to 04:05/04:45, but `fetch_diagnostics` moved (2026-08-12) from a single 04:30 window to every 15 minutes around the clock -- a deliberate widening of this collision window in exchange for fresher diagnostics, not an oversight (see the `on_time` comment in §6 / `mhrv.yaml`).
6. **RESOLVED 14 Aug 2026 — `Outdoor Temp`'s real range is 5-20 °C**, confirmed and no longer a guess; `number.py` and `entities.h` updated to match (the guessed 5–25 had the right floor but the wrong ceiling). Indoor Temp's 16-40 °C, taken from the manual since stage 1, is unaffected and was already correct. The remaining open question from the old notes still stands: whether Outdoor Temp is a pure setpoint or also *reports* something. The stated test — write a distinctive value, check it at the next daily read with nobody touching the unit — was written as falling out of stage 5 for free, but there was no daily read until stage 8 added one (04:45) and no way to see the result until the same stage fixed the publish path. It is genuinely available now, and is the obvious first thing to do with the fix once it is flashed.
7. **A second dongle (ESP8266 + RJ9 lead) is the cheapest real risk reduction**, removing "the house's ventilation is the dev target". It would pay for itself by stage 5.
8. **LARGELY CLOSED 17 Aug 2026 — a humidity boost does not touch line1, so it cannot fake `Boost Continuous`.** §8 stage 14 decoded line2 column 15's alpha annunciator as `humidity_boost` from a screenshot, leaving open what line1 reads while it shows. The hazard as written: if a humidity boost drove line1 to `Boost Airflow` with no countdown, `airflow_mode` would report `Boost Continuous` wrongly after `CONTINUOUS_CONFIRM_MS`. The reasoning that made this urgent was correct and is worth keeping — `ms_without_countdown_` (status.cpp) has three reset conditions (`!boosting_.active`, a countdown parsed this frame, or purge), and a line2 of `31%            *` rules out two outright (no `m` for `find_countdown_minutes()`, no purge), leaving `boosting_` never going active as the *only* thing that could prevent a false reading. The absent countdown armed the hazard rather than mooting it.

   **That single variable has now been observed, and it falls the safe way.** Stage 14's live capture caught a real humidity boost with line1 alternating `Normal Airflow` ⇄ `Summer Bypass On` and nothing else, `boosting` `OFF` and `airflow_mode` `Normal` throughout, over two captures spanning ~30 minutes. `boosting_` never went active, so the surviving reset condition holds and no false `Boost Continuous` is possible from this cause. The screenshot had caught the `Summer Bypass On` half of the alternation; the other half turns out to be `Normal Airflow`, not `Boost Airflow`.

   **What is left is narrower and is still not guessed at.** Untested: a *commanded* continuous boost coinciding with high humidity — line1 `Boost Airflow` and the column-15 annunciator at the same time. That combination is now known to be the only way the two can collide, and in it `Boost Continuous` would be the *correct* report, so the tempting guard on `!humidity_boost()` stays deliberately NOT implemented; it would suppress exactly the true positive. Nothing here needs a code change on current evidence. Two wrong explanations in a row for the SW1/2/3 flags (risk 2) is why this was written down rather than guessed at, and the record is kept intact above rather than deleted, because a hazard that was correctly reasoned and then correctly retired is worth being able to tell apart from one nobody checked.

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
