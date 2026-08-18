#pragma once

// Display line state: two parallel lanes (raw bytes for decoding, UTF-8 for
// presentation), change bookkeeping and screen classification. Plain C++17,
// no ESPHome headers -- see README "Portable core". The ESPHome hub owns the
// UART and the text_sensor objects; this class only decides *what* changed
// and *when*, via the on_change callback.
//
// Stage 16 replaced the single sanitize()'d representation this class used
// to keep with two: see DISPLAY-REVIEW.md §4/§5 for why one lossy string
// could not serve both jobs (byte-offset decoding AND safe-to-publish
// presentation) at once.

#include <cstdint>
#include <functional>
#include <string>

#include "screens.h"

namespace esphome {
namespace vent_axia {

/// Measured display byte values. One home for every fact of this kind, so
/// nothing downstream re-states a magic number that could drift out of sync
/// with the measurement backing it.
namespace glyphs {

/// The Sentinel Kinetic's alpha (sensor-boost) annunciator, measured live on
/// this unit 18 Aug 2026 (PLAN.md §8 stage 15's `GET /events` capture, taken
/// during a real humidity boost: `line2` non-ASCII byte `col 15=0xE0`
/// alongside `display_line_2` publishing `36%            *` under the old
/// sanitize()'d pipeline). Not a HD44780 A00 ROM datasheet inference --
/// DISPLAY-REVIEW.md §6 deliberately withheld the transcode table until this
/// measurement existed, the same discipline PLAN.md §8 stage 14's withdrawn
/// 31% evidence strand argues for.
constexpr unsigned char ALPHA = 0xE0;

}  // namespace glyphs

/// Transcodes one raw display line (16 raw bytes off the wire, one byte per
/// LCD column) into a UTF-8 string safe to publish to Home Assistant's text
/// sensors and to the ESPHome log stream -- both are protobuf/JSON
/// transports that require valid UTF-8, so a lone byte outside 0x20-0x7E
/// cannot pass through unescaped (DISPLAY-REVIEW.md §4). Replaces
/// sanitize(), which satisfied that same requirement by destroying the
/// byte instead of encoding it.
///
/// Mapping, in order of precedence:
///   0x20-0x7E  -> itself (printable ASCII)
///   glyphs::ALPHA (0xE0) -> "\xCE\xB1", i.e. α (U+03B1) -- the one glyph
///     this unit has ever had a measured code for (see glyphs::ALPHA).
///   everything else -> "<XX>", uppercase hex of the byte, via the same
///     to_hex_byte() table lookup describe_unprintable() uses.
///
/// Nothing here is mapped from the HD44780 A00 ROM datasheet -- °, µ, Ω, π
/// and all eight CGRAM slots stay as "<XX>" until something on THIS unit
/// measures them, exactly as glyphs::ALPHA was. The hex escape is not a
/// fallback to apologise for: it keeps distinct unmeasured bytes distinct
/// in the presentation lane too (unlike sanitize()'s single '*'), and it
/// names the exact byte a future capture needs to identify.
///
/// Guarantees: the result is always valid UTF-8, never contains a 0x00 byte
/// (which would truncate anything passing the result through .c_str()),
/// and an all-ASCII input is returned unchanged with no allocation growth.
/// A literal '<' in display text would in principle be ambiguous against
/// the escape's own syntax, but every display field is a menu label or a
/// numeric/time field (README "Portable core"), never free text, so this
/// is theoretical -- and preferable to a mapping that loses the byte to
/// avoid a collision that has never been observed.
std::string to_utf8(const std::string &raw);

/// to_utf8()'s diagnostic counterpart: instead of encoding non-printable
/// bytes, describes them. For every byte in `raw` where
/// std::isprint(static_cast<unsigned char>(byte)) == 0, appends
/// "col N=0xXX" (N = the byte's index, 0xXX uppercase hex), joined with
/// ", " in column order. Returns "" when the whole line is already
/// printable ASCII, so a call site can skip cheaply via .empty() rather
/// than build and discard a string every frame.
///
/// Still useful post-stage-16: to_utf8() now PRESERVES every byte (as
/// itself, as α, or as "<XX>"), so nothing is lost the way sanitize() used
/// to lose it -- but "<XX>" still reads as an unidentified byte rather than
/// a name, and this is the tool that first named glyphs::ALPHA. The
/// instrumentation stage that calls this (VentAxiaHub::log_raw_frame_bytes_(),
/// vent_axia.cpp) reads the RAW frame text specifically to go capture
/// whatever a future "<XX>" turns out to be -- the CGRAM slots (0x00-0x07)
/// among them, still unmeasured as of stage 15's capture.
std::string describe_unprintable(const std::string &raw);

/// Owns the two 16-character display lines, in two lanes, plus per-line
/// change tracking.
///
/// **Parsing lane** (raw_line1()/raw_line2()): the bytes exactly as they
/// arrived off the wire, one byte per LCD column. Every decoder/predicate in
/// screens::, parser::, status:: and diagnostics:: reads this, at the fixed
/// offsets it already used before stage 16 -- see DISPLAY-REVIEW.md §5 "the
/// constraint that shapes everything": UTF-8 is multi-byte, so transcoding
/// in place would silently shift every one of those offsets. Never
/// published, never logged as a string.
///
/// **Presentation lane** (text_line1()/text_line2()): the UTF-8 transcode of
/// the SAME update, computed lazily -- only for a line that actually
/// changed (see update()). Feeds the HA text sensors and log strings, and
/// only those; nothing inside this class or its callers needs UTF-8 for
/// decoding.
class Display {
 public:
  // Args: did line1 change, did line2 change. Fired at most once per
  // update(), and only when at least one of the two is true, so a caller can
  // publish exactly the lines that actually changed rather than re-publish
  // an unchanged line alongside a changed one.
  using ChangeCallback = std::function<void(bool line1_changed, bool line2_changed)>;

  /// Feeds one already CRC-validated frame's raw text. Updates
  /// raw_line1_/raw_line2_ and their changed-at timestamps *independently*
  /// -- deduplicating on the RAW text of each line, not on the raw 41-byte
  /// frame and not on a sanitised/transcoded copy. The old component
  /// deduplicated on the whole frame via memcmp, so a change in one of the
  /// still-unparsed bytes (1..4, 5, 22) republished text that had not
  /// actually changed; the pre-stage-16 sanitize()'d dedup had the opposite
  /// defect, blind to a change from one non-printable byte to a DIFFERENT
  /// non-printable byte in the same column (both collapsed to the same
  /// '*'). Deduplicating on the raw byte lane fixes that for free -- see
  /// test_display.cpp's regression test.
  ///
  /// text_line1_/text_line2_ are transcoded ONLY for a line whose raw text
  /// just changed, not unconditionally every call: today's sanitize()
  /// allocated two strings per frame at ~3.3 frames/s whether anything moved
  /// or not, so "compare raw, transcode on change" is a steady-state cost
  /// reduction, not an addition, per DISPLAY-REVIEW.md §5.
  void update(const std::string &raw_line1, const std::string &raw_line2, uint32_t now_ms);

  const std::string &raw_line1() const { return raw_line1_; }
  const std::string &raw_line2() const { return raw_line2_; }
  const std::string &text_line1() const { return text_line1_; }
  const std::string &text_line2() const { return text_line2_; }

  /// False until the first frame has been decoded. Distinguishes "the display
  /// is blank" from "we have never heard from the unit".
  bool have_frame() const { return have_frame_; }

  uint32_t line1_changed_at_ms() const { return line1_changed_at_ms_; }
  uint32_t line2_changed_at_ms() const { return line2_changed_at_ms_; }

  /// True while the unit has a value editor open.
  ///
  /// This is load-bearing and every later stage that navigates the menu
  /// depends on it, so the reasoning is spelled out in full: the protocol
  /// carries no explicit "editor open" flag anywhere. What it does instead is
  /// blink the value being edited by re-sending line2 on alternate frames
  /// (~300ms apart), so line2 changes roughly every ~350ms for as long as the
  /// editor is open. A settled, non-editor screen publishes its text once and
  /// then falls silent -- there is no further line2 change, ever, until the
  /// screen actually changes. Because publish-on-change means a static
  /// screen produces no events, "line2 stopped changing" cannot be observed
  /// as an edge; it can only be inferred from staleness. So: "editor open" is
  /// defined as "line2 changed recently", with recently = within settle_ms_
  /// (default 1200ms, comfortably more than one blink period so a single
  /// slow frame doesn't read as closed, comfortably less than the gap
  /// between unrelated status-loop screen changes).
  ///
  /// Before the first frame arrives the answer is "no", not "yes": the
  /// timestamp is still 0, so a naive staleness test would report an open
  /// editor for the whole first settle_ms_ after boot, when in fact nothing
  /// is known about the unit at all. Sequences gate on this, so guessing
  /// "open" there would have them wait out a phantom editor.
  bool editor_open(uint32_t now_ms) const {
    return have_frame_ && (now_ms - line2_changed_at_ms_) < settle_ms_;
  }

  // Reads the raw lane, same as every other decoder/predicate in this
  // class -- classify() only ever compares against fixed ASCII prefixes, so
  // there is no presentation concern here at all.
  screens::ScreenKind screen_kind() const { return screens::classify(raw_line1_); }

  void set_settle_ms(uint32_t settle_ms) { settle_ms_ = settle_ms; }
  void set_on_change(ChangeCallback callback) { on_change_ = std::move(callback); }

 private:
  std::string raw_line1_;
  std::string raw_line2_;
  std::string text_line1_;
  std::string text_line2_;
  uint32_t line1_changed_at_ms_{0};
  uint32_t line2_changed_at_ms_{0};
  bool have_frame_{false};
  uint32_t settle_ms_{1200};
  ChangeCallback on_change_;
};

}  // namespace vent_axia
}  // namespace esphome
