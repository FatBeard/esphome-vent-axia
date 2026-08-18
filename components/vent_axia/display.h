#pragma once

// Display line state: text, sanitisation, change bookkeeping and screen
// classification. Plain C++17, no ESPHome headers -- see README "Portable
// core". The ESPHome hub owns the UART and the text_sensor objects; this
// class only decides *what* changed and *when*, via the on_change callback.

#include <cstdint>
#include <functional>
#include <string>

#include "screens.h"

namespace esphome {
namespace vent_axia {

/// Strips non-printable glyphs from a raw display field, replacing each with
/// '*'. The unit renders some symbols (notably an "Auto" glyph) as custom
/// HD44780 characters with no ASCII equivalent; passing those bytes through
/// to Home Assistant's string handling crashes it, so every field is
/// sanitised before anything sees it. isprint() takes an int in the "is it a
/// valid unsigned char, or EOF" range -- calling it on a plain (possibly
/// negative signed) char is undefined behaviour, hence the cast.
std::string sanitize(const std::string &raw);

/// sanitize()'s diagnostic counterpart: instead of destroying non-printable
/// bytes, describes them. For every byte in `raw` where
/// std::isprint(static_cast<unsigned char>(byte)) == 0, appends
/// "col N=0xXX" (N = the byte's index, 0xXX uppercase hex), joined with
/// ", " in column order. Returns "" when the whole line is already
/// printable ASCII, so a call site can skip cheaply via .empty() rather
/// than build and discard a string every frame.
///
/// Exists because sanitize() collapses 0x00-0x1F, 0x7F and 0x80-0xFF -- the
/// α annunciator, °, µ and all eight CGRAM custom glyphs among them -- onto
/// a single '*', which makes them indistinguishable from each other from
/// that point on. The instrumentation stage that calls this
/// (VentAxiaHub::log_raw_frame_bytes_(), vent_axia.cpp) reads the RAW frame
/// text, before sanitize() ever runs, specifically to recover what the
/// collapse throws away. See DISPLAY-REVIEW.md §6/§7 and
/// DISPLAY-INSTRUMENTATION-PLAN.md for why: α = 0xE0 is currently an
/// inference from the HD44780 A00 ROM, never measured on this unit.
std::string describe_unprintable(const std::string &raw);

/// Owns the two 16-character display lines plus per-line change tracking.
class Display {
 public:
  // Args: did line1 change, did line2 change. Fired at most once per
  // update(), and only when at least one of the two is true, so a caller can
  // publish exactly the lines that actually changed rather than re-publish
  // an unchanged line alongside a changed one.
  using ChangeCallback = std::function<void(bool line1_changed, bool line2_changed)>;

  /// Feeds one already CRC-validated frame's raw text. Sanitises both lines,
  /// then updates line1_/line2_ and their changed-at timestamps
  /// *independently* -- deduplicating on the sanitised text of each line,
  /// not on the raw 41-byte frame. The old component deduplicated on the
  /// whole frame via memcmp, so a change in one of the still-unparsed bytes
  /// (1..4, 5, 22) republished text that had not actually changed.
  void update(const std::string &raw_line1, const std::string &raw_line2, uint32_t now_ms);

  const std::string &line1() const { return line1_; }
  const std::string &line2() const { return line2_; }

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

  screens::ScreenKind screen_kind() const { return screens::classify(line1_); }

  void set_settle_ms(uint32_t settle_ms) { settle_ms_ = settle_ms; }
  void set_on_change(ChangeCallback callback) { on_change_ = std::move(callback); }

 private:
  std::string line1_;
  std::string line2_;
  uint32_t line1_changed_at_ms_{0};
  uint32_t line2_changed_at_ms_{0};
  bool have_frame_{false};
  uint32_t settle_ms_{1200};
  ChangeCallback on_change_;
};

}  // namespace vent_axia
}  // namespace esphome
