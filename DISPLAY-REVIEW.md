# Display capture: a design review

Written 17 Aug 2026, against the tree at `24e6cda` (*Decode line2's alpha annunciator as
humidity_boost*). Review only — no code was changed. Every structural claim below carries a
`file:line` anchor; check them rather than taking this document's word for it.

> **Status: a snapshot, not an authority.** `PLAN.md` remains the authority on intent, per
> CLAUDE.md; this document is a point-in-time finding whose output is a stage in `PLAN.md`
> §8. Where the two disagree, `PLAN.md` wins.
>
> Parts of this will go stale by design. §3 records α = `0xE0` as an **unverified datasheet
> inference**, and §6 exists to replace it with a measurement. Once that capture happens,
> the `PLAN.md` stage entry becomes the live record and this document is history — do not
> keep editing it to track the code.
>
> `DISPLAY-INSTRUMENTATION-PLAN.md` is the executable stage derived from §6 and §7.

The review answers three questions:

1. Are the design decisions behind display capture sound, and is there a better way?
2. Why does the α annunciator show as `*`?
3. Is there a fundamentally better way to sanitise, and does it need sanitising at all?

Question 2 is not academic. `24e6cda` shipped `humidity_boost`, which decodes the manual's
alpha symbol by testing `line2[15] == '*'` (`status.cpp:119-120`). That works only because
`sanitize()` happens to map α to `*`, and the commit message flags the weakness itself:
*"sanitize()'s mapping is many-to-one."* A shipped feature is resting on a lossy transform,
and the mitigation — narrowing the test to one column — manages the symptom rather than the
cause. That is the thread this review pulls.

---

## 1. The capture path as it stands

```
MVHR ──RS232 9600 8N1──▶ UART ──▶ Framer ──▶ DisplayFrame ──▶ Display ──▶ entities
                                  (bytes)     (raw text)      (sanitised)
```

**Wire.** The MVHR pushes a 41-byte frame roughly every 300ms carrying both display lines
as text. There is no segment bus and no glyph decoding: the dongle impersonates a dumb
serial terminal on the wired-remote port, so "display capture" is framing plus `substr`,
not bit-unpacking. Frame constants at `protocol.h:17-22`.

**Framing.** `protocol::Framer::feed` (`protocol.cpp:60-85`) is a byte-at-a-time state
machine, pumped from `VentAxiaHub::loop` (`vent_axia.cpp:207-221`), which drains whatever
the UART FIFO holds each tick. Sync byte `0x02`, 41 bytes, trailing big-endian checksum.

**Checksum.** `running_crc` (`protocol.cpp:7-13`) is `0xFFFF` minus the sum of every byte —
the protocol's own scheme, not a choice this project made.

**Parse.** `parse_display_frame` (`protocol.cpp:22-32`) slices the frame:

| Field | Frame bytes |
|---|---|
| `unknown_header` | 1..4 |
| `unknown_row1_addr` | 5 |
| **`line1`** | **6..21** (16 chars, space-padded) |
| `unknown_row2_addr` | 22 |
| **`line2`** | **23..38** (16 chars, space-padded) |
| checksum | 39..40 |

**Sanitise and dedup.** `Display::update` (`display.cpp:18-41`) sanitises both lines, then
compares each against the previous *sanitised* text and fires `on_change_` with two
booleans saying which lines actually moved.

**Publish.** The hub's callback (`vent_axia.cpp:22-52`) publishes `DISPLAY_LINE_1`,
`DISPLAY_LINE_2`, a trimmed `STATUS_MESSAGE`, and routes diagnostic pages. `StatusTracker`
is fed every frame regardless of change (`vent_axia.cpp:213-218`), because its aging clock
needs a heartbeat to notice time passing.

---

## 2. What is right

This deserves saying plainly before the criticism, because the rewrite fixed real defects
that the predecessor shipped with.

**Framing is now correct.** Two rules in `Framer`, both documented at `protocol.h:81-90` as
earned by getting them wrong once:

- Hunting for sync *skips* a non-`0x02` byte rather than aborting (`protocol.cpp:62-64`).
  The old component did `return` from the whole `loop()` on a stray byte
  (`mhrv_orig/…/vent_axia_sentinel_kinetic.cpp:132-134`), abandoning the rest of the FIFO
  and risking overrun.
- A CRC failure rescans the buffer for the next plausible `0x02` (`protocol.cpp:87-101`)
  rather than discarding outright, so a false lock inside a payload resyncs within the same
  frame period instead of costing a whole extra one.

**Dedup is per-line.** The old component compared the whole 41-byte frame with `memcmp`, so
a change in an unparsed byte republished text that had not moved. `Display` deduplicates
each line independently (`display.cpp:27-36`) and reports which one changed — the reasoning
is spelled out at `display.h:35-41`, and `tests/test_display.cpp:18-52` pins it.

**Unknown bytes are retained, not discarded.** `DisplayFrame` keeps `unknown_header` and
both `unknown_row*_addr` bytes (`protocol.h:47-50`) so a later stage can decode them without
another framing rewrite. That is the right instinct, and §7 argues it should now be cashed
in.

**The portable core takes `now_ms` instead of calling `millis()`.** That single rule is what
makes the host suite deterministic with no fake clock, and it is why the timing-sensitive
parts — `editor_open()`, `StatusTracker`'s aging — are testable at all.

**Sticky flags with timeout-based falling edges** are the correct answer to a
publish-on-change protocol where line1 alternates. "Not seen this frame" is not "gone"; the
class comment at `status.h:123-137` gets this exactly right, including the subtlety that the
aging clock must not advance while a sequence has parked the display in a menu.

**`editor_open()` is honest about being a heuristic** (`display.h:53-77`), including the
boot case where a naive staleness test would report an open editor for the first settle
window. §7 argues the protocol may make the heuristic unnecessary.

---

## 3. Why you see `*` instead of α

### The mechanism

`components/vent_axia/display.cpp:8-16`:

```cpp
std::string sanitize(const std::string &raw) {
  std::string out = raw;
  for (char &c : out) {
    if (std::isprint(static_cast<unsigned char>(c)) == 0) {
      c = '*';
    }
  }
  return out;
}
```

Under the C locale `std::isprint()` is true only for `0x20`–`0x7E`. So **every** byte in
`0x00`–`0x1F`, `0x7F`, and the entire `0x80`–`0xFF` range becomes the same `*`.

The Sentinel Kinetic's panel is an HD44780-compatible character LCD, and the MVHR sends the
character codes verbatim. On the standard A00 ROM those codes land like this:

| Byte | Glyph | Fate today |
|---|---|---|
| `0x00`–`0x07` | CGRAM custom characters (whatever this unit loaded) | `*` |
| `0xDF` | `°` | `*` |
| `0xE0` | **`α`** | `*` |
| `0xE4` | `µ` | `*` |
| `0xF4` | `Ω` | `*` |
| `0xF7` | `π` | `*` |

So the answer to "why `*`": **α is `0xE0`, `isprint()` rejects it, and the byte is
overwritten before anything downstream — including `has_sensor_boost_annunciator()` — ever
sees it.** The asterisk genuinely *is* the alpha, exactly as `status.h:46-48` says.

> **Unverified inference.** That α = `0xE0` comes from the HD44780 A00 datasheet, not from
> this unit. Nothing in this component has ever logged a raw byte (§6), so the specific code
> is not yet a measured fact. The *mechanism* — `isprint()` rejects it, it becomes `*` — holds
> for any non-ASCII byte and does not depend on which one it is.

`sanitize()` runs at `display.cpp:19-20`, the first thing `Display::update` does. It is the
single point at which the byte→character contract is decided, which is architecturally
sound — the problem is what the decision *is*, not where it is made.

### Three defects that follow from it

**1. Dedup runs on the sanitised text, so glyph changes are invisible.**

`display.cpp:27` and `:32` compare `s1`/`s2` — the sanitised strings — against the stored
previous values. Two *different* custom glyphs both sanitise to `*`, so a glyph-to-glyph
swap in the same column produces `s1 == line1_`, no change flag, no callback, no publish.
The display would have changed and the component would not notice.

This is latent rather than observed, and today the exposure is narrow. But it is exactly the
class of bug that `Display` was written to fix at the frame level, reappearing one layer up.

**2. The information is destroyed at the earliest possible point.**

The raw bytes exist. `parse_display_frame` (`protocol.cpp:28,30`) assigns them straight off
the wire into `DisplayFrame::line1`/`line2`. They survive precisely as far as
`display.cpp:19`, and nothing retains them afterwards.

That is the good news: **the loss has a single, well-placed chokepoint**, so fixing it is
localised rather than a rewrite.

**3. The one-column narrowing is a mitigation, not a fix — and it has a cost.**

`has_sensor_boost_annunciator` is two lines of code (`status.cpp:119-120`) carrying roughly
sixty lines of comment (`status.h:42-99`), a large part of which exists solely to reason
around the many-to-one collapse: why a whole-line scan would false-fire on Mode 2's Auto
glyph, why column 15 was measured at 45.4 px/char off a screenshot, why `ls` at columns
14–15 cannot satisfy it.

That comment is good work and the reasoning is sound. But most of it is the *cost of the
lossy transform*, not intrinsic complexity of the feature. If the raw byte survived, the
predicate becomes an exact test against a known code and the ambiguity argument simply
stops being necessary.

### A related soft spot in the tests

`tests/test_display.cpp:54-64` builds its fixture with `static_cast<char>(0x07)` commented
as *"the unit's non-ASCII Auto glyph"*. `0x07` is CGRAM slot 7 — plausible, but a guess
presented as a fixture. Worth relabelling as an assumption. More importantly, there is no
test asserting that two *different* non-printable bytes stay distinguishable, because today
they don't.

---

## 4. Does it need sanitising at all?

**Yes — but "sanitise" is answering two different questions at once, and only one of them
requires touching the data.**

The two responsibilities tangled up in `sanitize()`:

- **Safety.** Make the string legal to put in a protobuf field, a JSON stream, a log line
  and a SQLite TEXT column.
- **Representation.** Decide what a non-ASCII glyph should *look like* to a human.

The safety requirement is real, and raw pass-through is genuinely not an option:

- ESPHome's native API encodes text sensor state as a protobuf `string`, which is **defined**
  as UTF-8. A lone `0xE0` is not valid UTF-8. Depending on version the client raises on
  decode or substitutes — neither is a display.
- `GET /events` (the SSE stream CLAUDE.md recommends for observing the unit) is JSON, which
  is likewise UTF-8.
- `ESP_LOGD("%s", …)` feeds the network log stream.
- Home Assistant's recorder writes states to SQLite as TEXT.
- Separately, a `0x00` anywhere in the line truncates anything passing through `.c_str()`.

The project's own history corroborates this: the predecessor's notes record custom glyphs
"breaking string handling downstream", which is what motivated the fix that was ported here
in the first place.

**So the requirement is not in doubt. What is in doubt is the response to it.** The current
code satisfies a *transport-encoding* obligation by *deleting information at the capture
boundary*. Those are different operations that happen to both make the problem go away:

| | Guarantees valid UTF-8 | Keeps the byte | Distinguishes glyphs | Reversible |
|---|---|---|---|---|
| `isprint() → '*'` | yes | no | no | no |
| transcode to UTF-8 | yes | yes | yes | yes |

Transcoding gives the identical safety guarantee with none of the loss. So the honest answer
to "does it need sanitising at all" is: **the bytes must not reach the API, the log or HA as
raw bytes — but that is a reason to encode them, not a reason to throw them away.**

And note where the obligation actually applies: at the **publish** boundary. Nothing inside
the component needs UTF-8. The decoders want bytes.

---

## 5. The recommended design: two lanes

### The constraint that shapes everything

Every downstream decoder is **byte-offset indexed against a fixed 16-column line**:

- `screens::diagnostic_page` reads `line1[12]`/`line1[13]` (`screens.cpp:36-37`)
- `parser::parse_field(s, pos, len, out)` (`parser.cpp:12-30`), used at fixed offsets
  throughout `diagnostics.cpp`
- `parser::clock_rendered` pins positions 0–8 exactly (`parser.cpp:47-55`)
- `parser::clock_hour`/`clock_minute` index `s[4],s[5],s[7],s[8]` with no bounds check of
  their own (`parser.cpp:70-72`)
- `has_sensor_boost_annunciator` reads `line2[15]` (`status.cpp:119-120`)

UTF-8 is multi-byte by construction. Transcoding `line1()`/`line2()` in place turns a
16-character line into up to 48 bytes and **silently shifts every one of those offsets**.
Any fix that makes the published lines UTF-8 without addressing this breaks the decoders in
a way no existing test would catch, because the host tests all feed pure-ASCII fixtures.

That constraint is also the answer. Don't pick one representation — keep both.

### The shape

**Parsing lane — raw bytes, one byte per display column.** `Display` retains the raw 16-byte
lines alongside the presentation form. Every predicate in `screens`, `parser`, `status` and
`diagnostics` reads *this*, unchanged, at the offsets it already uses. Never published,
never logged as a string.

**Presentation lane — UTF-8.** A per-byte transcode table drives the two HA-facing text
sensors and nothing else. `°`, `α`, `µ` render as themselves in Home Assistant and in the
Lovelace card's `.lcd` panel.

**Dedup moves to the raw lane.** Strictly more sensitive than today, since distinct bytes
stay distinct — which closes defect 1 for free.

**Transcode lazily.** Only for a line that actually changed. Today `sanitize()` allocates two
strings per frame at ~3.3 frames/s whether anything moved or not; ordering the work as
*compare raw → transcode on change* makes the steady-state cost lower than it currently is,
not higher, which matters on an ESP8266's heap.

### What this buys

- α survives to Home Assistant as α.
- `has_sensor_boost_annunciator` becomes an exact test against a known byte. The
  many-to-one ambiguity argument at `status.h:50-54` — and the risk that Mode 2's Auto glyph
  could ever be confused with it — stops existing rather than being reasoned around.
- Every existing decoder keeps working with no offset changes.
- Glyph-to-glyph changes become visible.
- Per-frame cost goes down.

### On the transcode table

`0x20`–`0x7E` map to themselves. The A00 ROM upper region has well-known assignments
(`0xDF`=°, `0xE0`=α, `0xE2`=β, `0xE4`=µ, `0xF4`=Ω, `0xF7`=π, `0xFD`=÷, and katakana through
`0xA1`–`0xDF`).

`0x00`–`0x07` are CGRAM — **user-defined**, meaning whatever bitmaps this unit's firmware
loaded. No datasheet can tell you what they look like; only the physical panel can. They
should get stable, *individually distinguishable* placeholders (⓪①②…) rather than a shared
one, so a future decode can key off a specific slot the way `humidity_boost` keys off column
15 today.

Anything still unmapped falls back to a replacement character — but the raw lane keeps the
byte, so nothing is lost for decoding purposes. That is the whole point of the split.

---

## 6. Sequencing: instrument first

**Do not build the table from the datasheet.** PLAN.md stage 14 already records the cost of
reasoning ahead of a capture — three evidence strands offered, one later withdrawn in
`9c2fd25` because 18% turned out to be the Low rate rather than Normal. The same discipline
applies here: α = `0xE0` is an inference, and it is cheap to convert into a measurement.

**Step 1 — instrument.** Log any byte outside `0x20`–`0x7E` with its line and column, plus
the `unknown_header` and both `unknown_row*_addr` bytes, at VERBOSE. This does not exist
today in any form (§7).

**Step 2 — capture.** Read it off the live unit via `GET /events` with HTTP basic auth,
during a humidity boost — CLAUDE.md records `esphome logs` consuming one of six native-API
slots and once locking out both HA and an external tool until a reboot. The 17 Aug capture
(`line2` reading `36%            *` at 74% indoor humidity) shows the condition is
reproducible.

**Step 3 — transcode.** Build the table from measured codes. At that point the CGRAM slots
are also known, and the `0x07` fixture in `tests/test_display.cpp:57` can be corrected or
confirmed.

One capture settles all of it: which byte α is, which CGRAM slots the unit uses, and §7's
question below.

---

## 7. Secondary findings

**The `unknown_*` bytes are parsed and read by nothing.** `protocol.cpp:24-29` populates
`unknown_header`, `unknown_row1_addr` and `unknown_row2_addr`; a search across the component
finds no other consumer. Retaining them was the right call — but they have been sitting
there uninspected.

This is worth more than curiosity. If bytes 5 and 22 are plain HD44780 DDRAM set-address
commands they are constant and boring. But if either carries a **cursor or blink attribute**,
it would replace `editor_open()`'s 1200ms staleness heuristic with a direct read. CLAUDE.md
lists that heuristic among the device invariants "paid for in debugging on real hardware",
and records Up/Down inside an editor having *silently taken a 14 °C setpoint to 19 °C*. Any
chance of replacing a load-bearing heuristic with a protocol fact is worth one hex log —
and §6's instrumentation already produces it at no extra cost.

**`Framer` has no idle-gap reset.** `feed()` takes no time input (`protocol.h:93`), so a
frame interrupted mid-transmission leaves `len_ > 0` indefinitely; the next byte after an
arbitrary silence is appended to the stale partial. Self-correcting — 41 more bytes, one bad
CRC, then `resync_()` — so the cost is about one frame period. Fixing it properly means
threading `now_ms` into the core, which the no-`millis()` rule makes deliberate work.
**Probably not worth it**; noted so the decision is explicit rather than an oversight.

**The checksum is order-insensitive.** `running_crc` (`protocol.cpp:7-13`) subtracts bytes,
so it is blind to transpositions and to compensating errors. This is the protocol's design,
not a choice available to this project. The only consequence worth recording is that
`frames_dropped()` undercounts corruption — which `protocol.h:97-101` already frames
correctly as a link-health signal rather than a loss figure.

**`STATUS_MESSAGE` trimming is UTF-8-safe.** `parser::trim` (`parser.cpp:32` →
`screens.cpp:44-50`) strips only ASCII `0x20`, so it stays correct if the presentation lane
becomes UTF-8. No action needed — recorded because it is the kind of thing that would
otherwise need re-checking during the change.

---

## Summary

The capture architecture is sound, and materially better than what it replaced: the framing
is correct, dedup is per-line, unknown bytes are kept, the core is deterministic and tested.
The design decisions behind it hold up.

The one thing that does not is `sanitize()`. It collapses a 256-value alphabet onto one
character at the earliest point in the pipeline, and everything awkward downstream — the
sixty-line comment on a two-line predicate, the column-15 narrowing, the invisible
glyph-to-glyph change — traces back to that single loop.

Sanitisation is genuinely required. Deletion is not. The bytes must be *encoded* before they
reach the API, the log or Home Assistant; they do not need to be *destroyed* before they
reach the decoders. Splitting the parsing lane from the presentation lane satisfies both,
keeps every existing byte-offset decoder working unchanged, and costs less per frame than
the current code.

Do the hex log first. One capture during a humidity boost settles which byte α is, which
CGRAM slots the unit uses, and whether the two unread address bytes can retire the
`editor_open()` heuristic.
