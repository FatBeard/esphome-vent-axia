# Architecture review — `vent_axia` component

Reviewed 18 Aug 2026 against commit `dc3f44c`, on branch `architecture-review`.

This review reads the **code as written**. PLAN.md, CLAUDE.md and the code
comments were deliberately set aside and treated as claims to be checked, not as
specification — several turned out to describe intent that the code does not
implement, and those cases are recorded below as findings rather than corrected
silently.

Baseline at the time of review: 235 host tests passing, 0 failures; ~6,700 lines
across `components/vent_axia/`.

---

## 1. How the component actually works

### 1.1 The two halves

The component impersonates the MVHR's wired remote over a 9600 8N1 UART. The
unit pushes a 41-byte display frame roughly every 300 ms; a keypress is an 8-byte
frame repeated for as long as the key is held, and **a release is silence** —
there is no key-up frame. That single protocol fact shapes most of the design.

The code splits along a line that is enforced, not merely documented:

- **Portable core** — `protocol`, `screens`, `parser`, `display`, `status`,
  `diagnostics`, `keypad`, `sequence` and the six `seq_*.cpp`. No ESPHome
  headers. Compiled into the host test binary.
- **ESPHome adapter** — `vent_axia.cpp/.h` and the seven platform `.py` files.
  The only place that may include `esphome/...`.

The enforcement is `tests/CMakeLists.txt`: it globs *every* component `.cpp`
except `vent_axia.cpp`. A core file that grows an ESPHome dependency stops
compiling on the host. This is the single best structural decision in the
component, and everything else worth keeping follows from it.

### 1.2 Inbound data flow

```
UART byte
  └─ protocol::Framer::feed()          byte-at-a-time resync, CRC check
       └─ DisplayFrame {line1, line2, 6 undecoded bytes}
            ├─ VentAxiaHub::log_raw_frame_bytes_()   instrumentation only
            ├─ Display::update(raw1, raw2, now)
            │    ├─ raw lane   → every decoder (fixed byte offsets)
            │    ├─ text lane  → UTF-8, only for a line that changed
            │    └─ on_change(l1_changed, l2_changed)
            │         ├─ text sensors: display_line_1/2, status_message
            │         └─ if DIAGNOSTIC screen → diagnostics::decode_page()
            │                                    → page/field table → Sink
            ├─ status::StatusTracker::update(..., is_status_screen, now)
            │    └─ sticky Flags aged by elapsed time, frozen while parked
            └─ VentAxiaHub::publish_status_()  → binary sensors, sensors, select
```

Two details here are genuinely well judged. **Dedup happens on the raw byte
lane**, so a change from one non-printable byte to a *different* non-printable
byte is still a change — the transcoded lane would collapse both. And
`StatusTracker` **freezes rather than ages** while a sequence has parked the
display in a menu, so a 20-second diagnostics scrape is not misread as the bypass
having closed.

### 1.3 Outbound control flow

Two paths reach the wire, and both funnel through one choke point:

```
button / YAML action ─┐
                      ├─→ Runner::tap() / press()  ─→ Keypad ─→ UART
sequence primitive  ──┘         │
                                └─ Set interlock: refused on a diagnostic page
```

`Runner::tap()/press()` being the only way to reach the `Keypad` is what makes
the Set interlock (page 27 is "Reset", writes, never tried) impossible to bypass
— and, because `Runner` is portable core, it is testable. Moving that check out
of the hub was the right call.

### 1.4 The sequence engine

`Runner` holds a fixed 4-deep stack of `Sequence`s and pumps only the top one per
tick. Three properties do the real work:

1. **One root at a time.** `request()` refuses rather than queuing. Mutual
   exclusion is structural — there is nowhere for a second root to run. This
   replaces a hand-rolled busy flag that had to be acquired at 5 sites and
   released at 12.
2. **`on_finish()` always runs**, however a sequence ends — completed, cascaded
   failure, or root timeout. It is the single keypad-release site.
3. **A child failure is not survivable.** It cascades to the root, which triggers
   `recover()`.

Each sequence is a `switch` on `step_`, with `goto_step()` the only sanctioned
mutation so `entered_`/`elapsed()` cannot drift. Sequences are long-lived hub
members reused across runs, so per-run state resets in `on_start()`.

This design is sound and I recommend no change to any of it. The findings below
are about what has been layered *on* it.

### 1.5 The weak point everything else works around

`Display::editor_open()` is a **heuristic, not a flag**: the protocol carries no
"editor open" signal, so it is defined as `now - line2_changed_at_ms < 1200ms`,
inferring an open editor from the ~350 ms blink of the value being edited.

This is load-bearing — every navigation sequence gates on it — and it is
ambiguous in one specific way: a *successful* commit also repaints line2. So any
check that needs to tell "closed" from "open" must settle longer than the
heuristic's own window, which is why `1800ms` appears three separate times
(`ExitEditChain::COMMIT_SETTLE_MS`, `SyncClock::SETTLE_MS`) against
`OpenEditor::SETTLE_MS`'s 700 ms and `Display::settle_ms_`'s 1200 ms.

Four constants exist to work around one missing bit of information. The
instrumentation in `log_raw_frame_bytes_` was built to find that bit — whether
`unknown_row1_addr`/`unknown_row2_addr` carry a cursor or blink attribute — but
**the capture that would settle it has never been taken**, because the stage-15
capture never opened an editor. See recommendation R1.

---

## 2. Findings

Thirteen findings. Two are functional defects; one is an architecture violation
the test suite has written off as unfixable; the rest is duplication. The
recurring theme is **plumbing that must be wired by hand at N sites, where a
missed site fails silently**.

### F1 — Dependency wiring is hand-rolled at ten sites, and has already failed

`set_log_sink()` is defined 10 times. Four of those implementations hand-forward
to child sequences the hub cannot reach:

```cpp
void set_log_sink(LogSink sink) {
  this->log_ = sink;
  this->exit_chain_.set_log_sink(sink);
  this->adjust_field_.set_log_sink(sink);      // added late — see below
  this->read_back_.set_log_sink(std::move(sink));
}
```

`setup()` then constructs the *same* three-lambda triple **eight times**:

```cpp
this->fetch_diagnostics_.set_log_sink({
    [](const std::string &msg) { ESP_LOGI(TAG, "%s", msg.c_str()); },
    [](const std::string &msg) { ESP_LOGW(TAG, "%s", msg.c_str()); },
    [](const std::string &msg) { ESP_LOGE(TAG, "%s", msg.c_str()); },
});
```

**This is not hypothetical duplication — it has already caused a real bug.**
`sequence.h:846` records that `adjust_field_` forwarding was missing, so
`AdjustField`'s "target unavailable" error "logged nothing". The failure mode is
silence, which is the worst kind: the code looks wired, and the only symptom is a
diagnostic that never appears.

Sequences already reach the `Runner` for `now_ms()`, `display()`, `tap()` and
`press()`. Logging is the one dependency that does not travel that route, for no
reason. Moving the `LogSink` onto `Runner` deletes 8 lambda triples, 8 members, 10
setters and 4 hand-forwarding chains — and makes the whole class of bug
unreachable, because there is nothing left to forget to wire.

The same argument applies to `set_on_switch`/`set_on_number` (forwarded through
`WriteSetting` into `read_back_`), `set_status`, `set_time_source`,
`set_filter_hours_source` and `set_on_success`.

### F2 — Real decision logic lives on the untestable side of the line

`VentAxiaHub::publish_airflow_mode_()` (`vent_axia.cpp:613-694`) is not a thin
publish adapter. It contains a state machine:

- a per-episode latch, `was_boost_60_this_episode_`, set when the countdown
  exceeds 30 and cleared when boosting ends, so the second half of a 60-minute
  boost doesn't silently report as a 30;
- the continuous-boost branch and its confirm window;
- suppression of all publishing while `SetAirflowMode` is the running root.

None of it touches ESPHome except the final `publish_select_()` call. All of it is
pure logic over `std::optional<bool>`/`std::optional<int>`. And **none of it is
tested**, because it sits in the one file the host suite excludes.

`tests/test_set_airflow_mode.cpp:967` states the position explicitly:

> There is no way to reach either fix from this host test binary without linking
> ESPHome itself, so neither is covered here — both were instead verified by
> compiling [...] and by reading the diff.

That conclusion is wrong. Linking ESPHome is not the alternative; **moving the
derivation into the portable core** is, which is precisely what the architecture's
own dividing line prescribes. The boundary was drawn to make exactly this kind of
logic testable, and here it has been used to justify leaving it untested.

`reconcile_filter_change_due_()` has the same shape on a smaller scale: the
"live wins by recency" policy is a real decision, untested for the same reason.

### F3 — The `Tap` primitive is dead; 14 sites hand-roll it instead

`Tap` is declared and implemented as a first-class primitive. Production uses:
**zero**. Only `tests/test_sequence.cpp` instantiates it.

What sequences write instead, 14 times:

```cpp
case COMMIT:
  if (!this->runner_->tap(SET, TAP_MS)) return Poll::FAILED;
  return this->goto_step(WAIT_COMMIT_TAP);
case WAIT_COMMIT_TAP:
  return this->runner_->keypad_busy() ? Poll::RUNNING : this->goto_step(SETTLE);
```

Two steps and two enum members each. `seq_sync_clock.cpp:143` explains the
avoidance — "never `Tap()`, which would need a temporary Sequence with nowhere
long-lived to live" — which is a fair objection to `Tap` as designed, and an
argument for a helper method rather than for writing the pair out 14 times.

### F4 — **Functional:** `tap_duration` never reaches any sequence

`tap_duration` is a documented YAML option, validated, plumbed to
`VentAxiaHub::tap_duration_ms_`, and reported by `dump_config()`. It reaches the
four manual key buttons and the `vent_axia.tap_key` action.

**No sequence uses it.** All five sequence files define their own constant:

```
sequence.cpp:16              constexpr uint32_t MENU_TAP_MS = 50;
seq_write_setting.cpp:25     constexpr uint32_t TAP_MS = 50;
seq_read_settings.cpp:10     constexpr uint32_t TAP_MS = 50;
seq_sync_clock.cpp:10        constexpr uint32_t TAP_MS = 50;
seq_set_airflow_mode.cpp:11  constexpr uint32_t TAP_MS = 50;
```

This matters because of what `keypad.h:141` prescribes. It counts presses that
emitted fewer than 2 frames — a loop() stall landing inside a short tap, a
silently dropped keypress — surfaces the count in `dump_config()`, and says:

> if it ever climbs in practice the documented remedy is raising `tap_duration`
> to 100 ms

Following that instruction today fixes the four manual buttons and leaves every
sequence tap at 50 ms. The documented remedy does not reach the code that does
almost all of the tapping. Since sequence taps are where a dropped press actually
hurts — a dropped Set fails to open an editor, and the following Up presses walk
back up the menu — the remedy is misdirected exactly where it is needed.

### F5 — **Functional:** a link drop mid-run is never noticed

`Runner::set_link_up()` is fed every tick from the hub. It is read in exactly one
place: `Runner::request()`, which refuses to *start* a sequence while the link is
down.

Nothing checks it again. Once a run is under way, `Runner::loop()` never consults
it. If the link drops mid-run:

- the display stops updating, so every predicate stops firing;
- `AdjustField::CHECK` fails to parse line2 and returns `Poll::RUNNING`
  indefinitely — its guard only increments on a *successful* parse, so the bound
  that is supposed to stop a runaway never advances;
- the run continues queueing taps at a unit that is not listening;
- it ends only when the **root** timeout expires — up to **450 s** for
  `SyncClock`, 480 s for `WriteSetting`.

The `Keypad` watchdog (30 s) will force-release a stuck *hold*, so this is bounded
and not dangerous, but for up to eight minutes the component transmits keypresses
into a dead link and reports `busy` throughout. The information needed to stop is
already present and already updated every tick; it is simply never read.

### F6 — One `static_assert` in the whole component

Exactly one, in `seq_write_setting.cpp:48`. Every other load-bearing relationship
between constants is enforced by prose alone. Two examples where the comments
themselves say the relationship is critical:

`status.h:180` — "Must EXCEED `ALTERNATION_TIMEOUT_MS`", with CLAUDE.md adding
"do not 'tidy' the two constants toward each other". Nothing enforces it. A
future edit lowering `CONTINUOUS_CONFIRM_MS` to 12000 compiles cleanly and makes
the component report continuous boost on *every* timed-boost expiry.

The root timeouts (`450000`, `480000`, `240000`, `120000`, `90000`) are each
justified by hand-arithmetic in a comment summing constants that live elsewhere:

> Roughly 76 + 4 + 3 + 2.3 + 1.35 + 1.8 + 157 + 130.5 =~ 376s

That sum is stale the moment any input moves, and nothing will say so. These are
derivable expressions, not magic numbers.

### F7 — The running sequence is identified by string comparison

```cpp
if (this->runner_.busy() && std::strcmp(this->runner_.running_name(),
                                        this->set_airflow_mode_.name()) == 0)
```

The comment defends this as avoiding a duplicated string literal — a real concern,
correctly identified, solved the expensive way. Both objects are hub members;
pointer identity (`runner_.is_running(&set_airflow_mode_)`) is exact, cheaper, and
cannot be defeated by two sequences sharing a name.

### F8 — `default:` arms defeat the exhaustiveness checking the design wants

`write_switch`, `write_number` and `write_select` each end in a `default:` that
logs "no mapping for this key" at runtime, on a device with no serial console.
Removing those arms lets `-Wswitch` see an unhandled enum member instead.

**Corrected during implementation.** This finding originally claimed that removing
the `default:` arms "turns an unhandled enum member into a compile error". That is
false as stated, and the correction matters more than the original point.
`-Wall -Wextra -Werror` are the **host test suite's** flags
(`tests/CMakeLists.txt`); the ESPHome firmware build does not use `-Werror`.
Verified by adding an unmapped enum member and compiling `mhrv.yaml`: the result
is a `-Wswitch` **warning** and `exit=0` — a line in a long build log, not a
failure.

Since `write_switch`/`write_number`/`write_select` live in `vent_axia.cpp`, the
one file the host suite excludes, no arrangement of `default:` arms there can buy
a compile-time guarantee. Getting one requires moving the **mapping** across the
portable-core line, where `-Werror` actually applies — the same move F2 calls for,
for the same reason. Re-verified after the change: an unmapped member is now
`error: enumeration value 'DUMMY_UNMAPPED' not handled in switch [-Werror=switch]`
in the host build.

The general lesson, worth more than the specific fix: "the compiler will catch it"
is a claim to test, not to assume. It was wrong here in the direction that feels
safest.

### F9 — An out-of-range step reports success

Eleven sequence switches end `default: return Poll::DONE;`. A `step_` that is
somehow out of range — a bad `goto_step()`, a resumed `await` naming a stale enum
after an edit — is a bug, and it currently reports the sequence **succeeded**. For
`WriteSetting` that means reporting a write landed when nothing was pressed.
`Poll::FAILED` routes the same situation through `recover()`.

### F10 — `sequence.h` is 1,561 lines and 70% prose

11 classes in one header, 985 comment lines against 423 of code. It is designated
in CLAUDE.md as "the file to read first", and it is the file least amenable to
being read. The natural seams already exist and match the `.cpp` layout:
engine (`Poll`/`Sequence`/`Runner`), primitives, and the six concrete sequences.

### F11 — Investigative instrumentation is embedded in the hub

`log_raw_frame_bytes_` is ~120 lines with 9 supporting hub members
(`unknown_change_suppressed_`, `last_line1_log_ms_`, …) implementing rate
limiting, a heartbeat, and change-suppress-and-revert detection.

It is the most intricate function in `vent_axia.cpp`, it is not production
behaviour, and by living in the excluded file it is untestable — including the
suppress-and-revert path, which exists specifically to catch a sub-2-second blink
attribute and cannot be exercised at all. It is pure logic over bytes and
timestamps and belongs in the core.

### F12 — 47 comments cite paths that do not resolve from this repo

`mhrv_orig/controls.yaml`, `mhrv_orig/summer_bypass.yaml` and
`mhrv_orig/vent-axia-esphome-project.md` are cited 47 times, written as though
repo-relative. `mhrv_orig/` is not in this repo and never has been (no git
history). It resolves on this machine to `/home/brian/docker/esphome/mhrv_orig` —
outside the repo, and absent for anyone cloning the public GitHub project.

These citations are the stated authority for a large share of the measured
constants: `PURGE_HOLD_MS`, `HOLD_MS`, `NORMALISE_SETTLE_MS`, the three clock
guard limits, the `SettingSpec` guard limits, the `ReadSettings` timeouts. The
evidence for the numbers this component depends on does not ship with it.

### F13 — `have_frame_` exists twice

`VentAxiaHub::have_frame_` and `Display::have_frame_` track the same fact, set
from the same event. Two sources of truth for "have we ever heard from the unit",
one of which gates link liveness and the other of which gates `editor_open()`.

---

## 3. What I recommend against changing

Worth stating explicitly, since a review that only lists problems invites
over-correction:

- **The sequence engine's shape.** Single root, cascading failure,
  `on_finish()` as the one release site. Do not add queuing, priorities, or
  survivable child failures.
- **The device invariants.** The 400 ms `key_gap`, "a release is silence", the
  two-stage diagnostic exit, `GotoMenu`'s absolute targeting, `LeaveMenu`'s
  at-most-one-Up. Each was paid for on real hardware and several are documented
  with the exact failure they prevent.
- **Not being optimistic.** Switch, number and select all refuse to publish what
  Home Assistant asked for, publishing only what the unit was observed to do. A
  toggle that snaps back until the round trip completes is the honest behaviour
  for a remote control pressing buttons on a menu.
- **`StatusTracker`'s sticky Flags and park-freeze.** Correct, and subtle enough
  that it would likely be got wrong on a rewrite.
- **The 20 s `CONTINUOUS_CONFIRM_MS` margin over a 14.0 s measurement.** Not a
  round number picked for looks; leave it.

---

## 4. Recommendations beyond this branch

**R1 — Take the capture that would retire `editor_open()`'s heuristic.**
The highest-value change available to this component is not a refactor. Open an
editor on the unit while watching `GET /events` with the existing
`log_raw_frame_bytes_` instrumentation, and read whether
`unknown_row1_addr`/`unknown_row2_addr` move. If either carries a cursor or blink
attribute, `editor_open()` becomes a direct protocol read instead of a 1200 ms
staleness guess — and the three settle constants that exist only to disambiguate
it can collapse. The instrumentation for this was built and shipped; the capture
was simply never taken, because stage 15's capture never opened an editor. It is
ten minutes of observation against a heuristic that CLAUDE.md records as having
silently walked a 14 °C setpoint to 19 °C.

**R2 — Check in the evidence, or say where it lives.** See F12. Either a digest
of the `mhrv_orig` observations checked into this repo, or a note in `README.md`
naming its actual location. The measurements are the most valuable thing in the
comments and currently the least verifiable.

**R3 — Consider whether `read_only` should refuse sequences.** In `read_only`
mode the keypad suppresses transmission but `Runner` still runs sequences to
completion, so a scheduled `fetch_diagnostics` fails on timeout every day and
fires `on_sequence_failed`. Not a defect — soak-testing the exact code path is
the documented purpose — but the failure noise is worth a deliberate decision.
