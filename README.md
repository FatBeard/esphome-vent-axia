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
- **[alextrical](https://github.com/alextrical)** — the original ESPHome
  component for this device, and the
  [ESP32-Sentinel-Kinetic-Wireless-Dongle](https://github.com/alextrical/ESP32-Sentinel-Kinetic-Wireless-Dongle)
  hardware this component runs on.

This is an independent rewrite: same protocol and hardware target, different
architecture (see "Design notes" above) and license.

## Licence

GPL-3.0 -- see `LICENSE`.
