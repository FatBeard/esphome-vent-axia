# Stage: raw-byte instrumentation for display capture

**Status:** specified, not implemented. Ready to execute.
**Source:** `DISPLAY-REVIEW.md` §6 and §7. That document is the reasoning; this one is the
work. Neither supersedes `PLAN.md`, which remains the authority on intent — when this lands,
it gets a `PLAN.md` §8 stage entry like any other stage, and that entry becomes the record.

---

## Why

`sanitize()` (`components/vent_axia/display.cpp:8-16`) replaces every byte failing
`std::isprint()` with `'*'`. Under the C locale that is true only for `0x20`–`0x7E`, so all
of `0x00`–`0x1F`, `0x7F` and `0x80`–`0xFF` collapse onto one character — the α annunciator,
`°`, `µ`, and all eight CGRAM custom glyphs become indistinguishable.

The shipped `humidity_boost` decode (`status.cpp:119-120`) rests on that collapse. Its own
commit message (`24e6cda`) flags the weakness: *"sanitize()'s mapping is many-to-one."*

The review's fix is a two-lane split — raw bytes for parsing, UTF-8 for presentation — but it
explicitly says **do not build the transcode table from the datasheet**. α = `0xE0` is an
inference from the HD44780 A00 ROM, never measured on this unit. Stage 14 already records
what reasoning ahead of a capture costs: an evidence strand offered, then withdrawn in
`9c2fd25` when 18% turned out to be the Low rate rather than Normal.

Nothing in the component has ever logged a raw byte, so the inference cannot be settled
today. **This stage builds the instrumentation that makes the capture possible.** The
two-lane refactor is a later stage that depends on its results.

## Scope

Observation only. No decode behaviour changes.

**Explicitly out of scope** — do not begin these: the two-lane raw/UTF-8 refactor, the
byte→UTF-8 transcode table, and any change to `sanitize()`, `Display`'s dedup, or
`has_sensor_boost_annunciator()`. Any existing test changing its expected value means
something broke; stop and report rather than editing the assertion.

---

## 1. `describe_unprintable()` — pure formatter in the portable core

Declared beside `sanitize()` in `components/vent_axia/display.h`, defined in `display.cpp`.
It is `sanitize()`'s diagnostic counterpart and belongs in the same file.

```cpp
std::string describe_unprintable(const std::string &raw);
```

Scans byte by byte, column = index. For every byte where
`std::isprint(static_cast<unsigned char>(byte)) == 0`, appends `"col N=0xXX"` (uppercase hex,
`0x`-prefixed), joined with `", "` in column order. Returns `""` for an entirely printable
line, so a call site can skip cheaply via `.empty()`.

Use a small anonymous-namespace hex helper rather than `<cstdio>` or `<sstream>` — the core
avoids both today and builds strings with `std::to_string` and concatenation
(`keypad.cpp`, `sequence.cpp`). No ESPHome headers, so `tests/CMakeLists.txt`'s glob compiles
it into the host suite automatically.

## 2. Call site in `vent_axia.cpp` / `vent_axia.h`

A private `void log_raw_frame_bytes_(const protocol::DisplayFrame &frame, uint32_t now_ms);`
on `VentAxiaHub`, called from `loop()` on the local raw `frame`.

**Critical:** place it **before** the existing `this->display_.update(frame.line1,
frame.line2, now);` at `vent_axia.cpp:212`. It must read `frame.line1` / `frame.line2` — the
raw strings. Reading `display_.line1()` would log post-sanitisation text, which is exactly
the information this stage exists to recover.

Two `ESP_LOGD` outputs — visible at `level: DEBUG`, which both `mhrv/mhrv.yaml:13` and
`example/production.yaml:24` already run, so no global verbosity change and no new config
option:

**(a) The six never-read frame bytes** — `unknown_header[0..3]`, `unknown_row1_addr`,
`unknown_row2_addr`, parsed at `protocol.cpp:24-29` and consumed by nothing today. One
combined hex line.

**(b) Non-ASCII content per line** — `describe_unprintable(frame.line1)` and
`describe_unprintable(frame.line2)`, each logged only when non-empty.

## 3. Anti-flood gating

Frames arrive roughly every 300ms (~3.3/s) and this device's logs are network-only, so this
is load-bearing rather than tidiness.

**Non-ASCII lines gate on the formatter's output changing** — *not* on the raw line changing.
A humidity boost is precisely the case where an airflow percentage ticks while the
annunciator sits still in column 15, so raw-line gating would re-log an unchanged
`col 15=0xE0` several times a second. Gating on the description means a steady annunciator
logs once and stays quiet until the non-ASCII content itself moves.

**Unknown bytes gate on the 6-byte tuple changing AND a 2000ms minimum interval.** The
change-gate alone protects nothing if one of those bytes turns out to be a counter, sequence
or frame-phase byte — and nobody knows what they are, which is the whole reason for logging
them. Without the rate limit that case produces ~3.3 lines/s indefinitely.

The rate limit costs no diagnostic value, and the comment at the site should say why:
constant bytes give one line and tell you everything; bytes that toggle with editor state
stay visible; bytes that differ every frame show up as a steady cadence at the limit, which
is itself the answer.

Track with private `VentAxiaHub` members (ESPHome-side state; `Display`, `Framer` and the
core are untouched):

```cpp
std::string last_logged_line1_unprintable_;
std::string last_logged_line2_unprintable_;
std::array<uint8_t, 4> last_logged_unknown_header_{};
uint8_t last_logged_unknown_row1_addr_{0};
uint8_t last_logged_unknown_row2_addr_{0};
bool have_logged_unknown_bytes_{false};
uint32_t last_unknown_log_ms_{0};
```

`have_logged_unknown_bytes_` makes the first frame log unconditionally rather than relying on
zero-initialised defaults never coinciding with a real frame's values.

`now_ms` comes from the `now` already computed in `loop()`. `millis()` is fine here —
the no-`millis()` rule binds the portable core, and `vent_axia.cpp` is the ESPHome side.

## 4. Host tests

Added to `tests/test_display.cpp`, covering `describe_unprintable()` only:

- all-ASCII line → `""`
- a high-range byte (`0xE0` at column 15)
- a control-range byte (`0x07` at column 4)
- multiple bytes, asserting column order and indices
- boundaries `0x1F` / `0x20` / `0x7E` / `0x7F` in one line — only `0x1F` and `0x7F` reported

**Known coverage gap, to be stated rather than papered over:** the gating and rate-limit
logic lives in `vent_axia.cpp`, which `tests/CMakeLists.txt` deliberately excludes from the
host build. It is verified only by the firmware compiles and eventually by the live capture.
Do not restructure the core to make a two-line comparison testable; do not let the `PLAN.md`
entry imply coverage that does not exist.

## 5. `PLAN.md` entry

Add the stage to §8 in the existing voice, after stage 14's live-confirmation block. State
plainly that it is instrumentation rather than a decode change, and name the three questions
the capture is meant to settle:

1. Which byte the alpha annunciator actually is (`0xE0` is currently an inference).
2. Which CGRAM slots (`0x00`–`0x07`) this unit uses — relevant to the `0x07` fixture
   currently asserted as "the Auto glyph" at `tests/test_display.cpp:57`.
3. Whether `unknown_row1_addr` / `unknown_row2_addr` carry a cursor or blink attribute. If
   either does, it could replace `editor_open()`'s 1200ms staleness heuristic with a direct
   protocol read — the heuristic CLAUDE.md records as having silently taken a 14 °C setpoint
   to 19 °C.

---

## Verification

1. **Host suite**, including the new formatter cases:
   ```sh
   cd tests && cmake -B build -S . && cmake --build build && ./build/run_tests
   ```

2. **Both firmware targets.** Capture exit codes explicitly (`EXIT=$?`) — a trailing pipe
   masks a failed build. The ESP32-IDF target is the only one exercising the
   `USE_TIME`-undefined paths, so it is not optional:
   ```sh
   docker run --rm -v /home/brian/docker:/config ghcr.io/esphome/esphome:latest compile esphome-vent-axia/mhrv/mhrv.yaml
   docker run --rm -v /home/brian/docker:/config ghcr.io/esphome/esphome:latest compile esphome-vent-axia/example/esp32-idf.yaml
   ```
   `mhrv/.esphome/build/vent-axia-mhrv/src/main.cpp` holds `secrets.yaml` values in
   plaintext — do not grep or dump it. A leak into a transcript has already cost one
   credential rotation on this project.

3. **Confirm nothing else moved**: `sanitize()`, `Display`'s dedup,
   `has_sensor_boost_annunciator()`, and every existing test's expected value.

4. **Opus review of the diff, then commit.** No commit before review, per CLAUDE.md's
   execution model. No flashing as part of this stage.

`%02X` against an unpromoted `uint8_t` matches ESPHome's own precedent and should compile
clean. If either target's `-Werror` disagrees, add an explicit `unsigned` cast rather than
suppressing the warning.

---

## After this stage

Flash, then capture during a humidity boost via `GET /events` (HTTP basic auth) rather than
`esphome logs` — CLAUDE.md records the latter consuming one of six native-API slots and once
locking out both Home Assistant and an external tool until a reboot. The 17 Aug capture
(`line2` reading `36%            *` at 74% indoor humidity) shows the condition reproduces.

One capture settles all three questions above, and only then does the transcode table get
built from measured codes rather than from the datasheet.
