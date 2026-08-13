# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Layout

`esphome-vent-axia/` (this directory) is a single git repo, published at `github.com/FatBeard/esphome-vent-axia` (public, `origin`), and holds everything:

- **`mhrv/`** — the live ESPHome device config. `mhrv.yaml` is git-tracked (nothing secret in it — every credential goes through `!secret`); `secrets.yaml` holds the real values and is gitignored, `secrets.yaml.example` is the template (`cp secrets.yaml.example secrets.yaml` and fill in). `mhrv.yaml` pulls the component in via `external_components: {type: local, path: ../components}`, so edits to the component take effect on the next compile with no version bump.
- **`components/`, `example/`, `tests/`** — the component itself, where nearly all development happens. `PLAN.md` is the approved design and the authority on intent; `README.md` covers the portable-core and chip-agnostic rules. `example/production.yaml` is the public template of `mhrv/mhrv.yaml` for anyone outside this checkout — it pulls the component via a git-source block (pinned to `main` until a release is tagged) rather than the local path `mhrv/mhrv.yaml` uses for iteration speed.

The target is a Vent-Axia Sentinel Kinetic B, firmware **V32/05**, on an ESP8266 dongle at 192.168.1.200.

## Commands

Host tests (primary safety net — no hardware, no ESPHome checkout needed):

```sh
cd tests
cmake -B build -S . && cmake --build build && ./build/run_tests
```

The framework is hand-rolled (`tests/test_framework.h`) and has **no name filter** — `main()` runs the whole suite, which takes well under a second. To isolate a case, comment out others or add a temporary `TEST_CASE`; don't go looking for a `--filter` flag. `CMakeLists.txt` globs `test_*.cpp` and every component `.cpp` except `vent_axia.cpp`, so new files are picked up automatically. Warnings are errors (`-Wall -Wextra -Wpedantic -Werror`).

Firmware builds go through Docker (no local ESPHome install). **The mount is `/home/brian/docker`, not `/home/brian/docker/esphome`** — this repo is a *sibling* of `esphome/`, not a child of it, so the older `-v /home/brian/docker/esphome:/config` form fails with `No such file or directory` before the compiler is ever invoked (corrected 13 Aug 2026, having wasted a build round on it):

```sh
docker run --rm -v /home/brian/docker:/config ghcr.io/esphome/esphome:latest compile esphome-vent-axia/mhrv/mhrv.yaml
docker run --rm -v /home/brian/docker:/config ghcr.io/esphome/esphome:latest compile esphome-vent-axia/example/esp32-idf.yaml
```

Both can run concurrently — different build directories, and only `mhrv.yaml` needs secrets (resolved from `mhrv/secrets.yaml`, next to the config). Each takes well under a minute warm.

Flashing the live unit is OTA over the network (add `--network host` so the container can reach it):

```sh
docker run --rm --network host -v /home/brian/docker:/config ghcr.io/esphome/esphome:latest upload esphome-vent-axia/mhrv/mhrv.yaml --device 192.168.1.200
```

**Observing and driving the live unit.** `GET /events` (HTTP basic auth, `admin` + `web_server_password`) is a Server-Sent Events stream carrying every entity state *and* the DEBUG log, without consuming one of the six native-API connection slots — the best read-only channel, and far safer than `esphome logs` (mhrv.yaml records slot exhaustion once locking out both HA and an external tool until a reboot). But this build's `web_server` exposes **no per-entity REST endpoints** — `/sensor/<id>`, `/select/<id>`, `POST /select/<id>/set` all 404 (verified 13 Aug 2026, ESPHome 2026.7.4, web_server v2). To *command* an entity outside Home Assistant, use the native API: `aioesphomeapi` is already inside the ESPHome image, so run a short script with `--entrypoint python3` and call `select_command()`/`button_command()`. Pass the API key through an env var rather than inlining it — anything echoed lands in the transcript.

Compile **both** an ESP8266 and an ESP32-IDF target before calling a change done. The IDF example declares no `time:` platform, so it is the only thing that exercises the `USE_TIME`-undefined paths; `esphome config` does not invoke the C++ compiler and catches neither that nor ESPHome signature mismatches. When compiling in a shell pipeline, capture the exit code explicitly (`EXIT=$?`) — a trailing `grep` will otherwise mask a failed build.

## Architecture

**The dividing line: C++ owns "how to talk to the MVHR"; YAML owns "what to expose and when to do it."** This inverts the previous project (`mhrv_orig/`, still running on the live unit, kept as reference). Every YAML lambda there was really C++ squeezed through a string-typed transport. Resist moving device logic back into YAML.

**Portable core.** `protocol`, `screens`, `display`, `parser`, `status`, `diagnostics`, `keypad`, `sequence` and every `seq_*.cpp` include **no ESPHome headers** — only `vent_axia.cpp` and the `*.py` platform files do. They still live in `namespace esphome::vent_axia` (the rule is about headers, not namespace; a global `::vent_axia` broke ESPHome's generated `using namespace esphome;`). Anything the core needs from the ESPHome side arrives through an injected `std::function` sink — display changes, log lines, wall-clock time, diagnostics-page callbacks. If a core file stops compiling on the host, it grew a dependency that belongs on the other side of the line.

**No `millis()` in the core.** Everything takes `uint32_t now_ms` from `Runner`/`Keypad`. That is what makes the tests deterministic with no fake clock.

**Sequence engine** (`sequence.h`, the file to read first). `Runner` holds a fixed-depth stack of `Sequence`s and pumps only the top one per tick. One root sequence at a time gives mutual exclusion *structurally* — the old config's hand-rolled `ui_busy` global (acquired at 5 sites, released at 12) does not get reimplemented, it stops existing. `on_finish()` always runs however a sequence ends, and is the single keypad-release site. `await(child, on_ok)` nests; a child failure cascades to the root, which triggers `Runner::recover()`. Each sequence is a `switch` on `step_` reading as a flat list of named steps; bodies live in one `seq_*.cpp` per sequence with declarations in `sequence.h`. Sequences are long-lived hub members reused across runs — no dynamic allocation in steady state — and `push_child_` resets `step_`/`entered_`, so per-run state resets belong in `on_start()`.

**Entities** are enum-keyed: `entities.h` holds one enum per platform with a `COUNT` sentinel, each platform `.py` builds `CONFIG_SCHEMA` from a `{key: schema}` dict comprehension, and the hub indexes a `std::array<T*, COUNT>`. Adding an entity means an enum member, a dict entry and a publish call — never a new setter or a switch arm. Optional platforms need `#ifdef USE_SENSOR` / `USE_BINARY_SENSOR` / `USE_TEXT_SENSOR` / `USE_TIME` guards on includes, members *and* setters, with `#ifndef`-guarded no-op bodies; a config that declares no entities of a kind never compiles that platform.

## Device invariants

Each of these was paid for in debugging on real hardware. They carry explanatory comments at their sites — preserve the comment, not just the value.

- **A release is silence.** The protocol has no key-up frame, so a hold-to-hold transition needs an explicit gap (~400ms) or the unit reads one unbroken press.
- **`key_gap` ≥ 400ms.** 250ms drops ~1 press in 10; a dropped Set fails to open an editor, and the following Up presses then walk *back up the menu*.
- **Only Set is safe inside an editor.** Up/Down adjust the value under the cursor rather than navigating — this silently took a 14 °C setpoint to 19 °C. Anything that might press Up after an edit must check `Display::editor_open()` first.
- **`editor_open()` is a heuristic, not a flag**: `now - line2_changed_at_ms < 1200`. An open editor blinks its value ~every 350ms while a settled screen goes silent. A *successful* commit also repaints line2, so any check gated on this needs a settle longer than 1200ms (1800ms is the established figure) or it cannot tell a closed editor from an open one.
- **`GotoMenu` is absolute**, exploiting the hard stop at the top (5× Up = index 0), never relative counting.
- **`LeaveMenu` presses Up at most once**, then waits out the unit's own ~2-minute timeout, which closes an editor without committing. Mashing Up corrupts settings.
- **Blank ≠ zero.** A blank temperature renders `"   C"`; parsers must leave the value unpublished rather than publish 0.
- **Main is Boost**, never a menu key, and its press counter is cumulative with no usable timeout — absolute targeting must normalise first.
- **Page 28 does not exist** on V32/05. Terminate diagnostic scrapes on highest-page-seen, never a hardcoded page.
- **Set is interlocked off** while the display shows a diagnostic page (page 27 is `Reset`, writes, and has never been tried). Enforced in `Runner::tap()/press()` so no path can bypass it.

## Constraints

- **The rollout is underway, not hypothetical.** The live unit has been flashed with the `vent_axia` component and runs `mhrv.yaml` with `read_only: false`: every currently-implemented entity and sequence (fetch_diagnostics, read_settings, sync_clock, the raw key buttons, summer_mode, the bypass numbers) transmits to the real unit and has been exercised against it. PLAN.md §8's staged order is the record of what's been tried and in what sequence — keep extending it as new features go live rather than treating it as pre-rollout planning. Flashing (`esphome upload`/`run`) is a real, deliberate action now, not a forbidden one: compile and validate first (see Commands above), and confirm with the user before flashing rather than doing it as a matter of course, since it's still a physical, hard-to-reverse action on hardware that ventilates an occupied house.
- **Continuous boost IS supported** as of 13 Aug 2026 — the old "out of scope by decision" constraint was retired after the assumption behind it was re-tested on the live unit (PLAN.md §4 and §8 stage 9). It was never truly indistinguishable: only *line2* is ambiguous, and line2 was never the discriminator. Line1's `Boost Airflow` is, and the component already rested `boosting`, `airflow_mode`'s whole Boost-vs-Normal split and `SetAirflowMode`'s normalise probe on catching exactly that. The live invariant that replaces it: **`StatusTracker::CONTINUOUS_CONFIRM_MS` must exceed `ALTERNATION_TIMEOUT_MS`.** At a timed boost's expiry the countdown vanishes from line2 while `boosting_` stays sticky-true for up to the alternation timeout; anything shorter reports continuous boost on *every* timed-boost expiry. Measured on the unit: 14.0s from the last `Boost Airflow` frame to `boosting()` dropping, hence 20000 against a nominal 12000 — do not "tidy" the two constants toward each other.
- Work proceeds one stage at a time: implement with a Sonnet subagent plus host tests, review as Opus, apply findings with regression tests, then commit. A review after ten files exist is a rewrite, not a review.

## Conventions

Comments carry *reasoning*, especially the hardware observation behind a magic number, and are unusually dense by design — match the surrounding density rather than trimming to house-average. Commit messages are prose explaining why, including defects found in review. When a test asserts current-but-wrong behaviour, fix the assertion explicitly and say so; don't quietly weaken it.
