#pragma once

// Frame constants, CRC, RX framing and TX frame construction for the
// Vent-Axia wired-remote link. Plain C++17, no ESPHome headers -- see
// README "Portable core". Only frame *construction* lives here for TX;
// nothing in this file transmits anything.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace esphome {
namespace vent_axia {
namespace protocol {

constexpr uint8_t RX_FRAME_LEN = 41;
constexpr uint8_t RX_SYNC_BYTE = 0x02;
constexpr uint8_t LINE_LEN = 16;

constexpr uint8_t TX_FRAME_LEN = 8;
constexpr uint8_t TX_SYNC_BYTE = 0x04;

/// The four physical keys. Values are the bit each asserts in a key frame's
/// data byte, so combinations are formed by ORing enumerators together (see
/// KeyMask) -- e.g. Up+Down for a filter reset, Up+Main for diagnostics entry.
enum class Key : uint8_t {
  DOWN = 0x01,
  UP = 0x02,
  SET = 0x04,
  MAIN = 0x08,
};

/// An OR of zero or more Key bits. A plain alias rather than a wrapper type:
/// the only operation needed is "OR some Keys together and hand the result to
/// build_key_frame", and std::underlying_type gymnastics for that would be
/// ceremony without payoff.
using KeyMask = uint8_t;

constexpr KeyMask key_mask(Key key) { return static_cast<KeyMask>(key); }

/// Parsed contents of one 41-byte display frame. `unknown_*` fields are
/// bytes whose meaning has not been reverse-engineered -- kept accessible
/// rather than discarded so a later stage can decode them without another
/// framing rewrite.
struct DisplayFrame {
  std::array<uint8_t, 4> unknown_header{};  // bytes 1..4
  uint8_t unknown_row1_addr{0};             // byte 5, probably an HD44780 DDRAM address
  std::string line1;                        // bytes 6..21, 16 chars, space padded
  uint8_t unknown_row2_addr{0};             // byte 22, probably an HD44780 DDRAM address
  std::string line2;                        // bytes 23..38, 16 chars, space padded
};

/// The running-subtraction checksum used by both RX and TX frames: start at
/// 0xFFFF and subtract every byte. Verified against captured frames from the
/// physical unit; RX checks the first 39 bytes against buffer[39..40], TX
/// checks the first 6 against buffer[6..7].
uint16_t running_crc(const uint8_t *data, size_t len);

/// True if `frame` (must point to RX_FRAME_LEN readable bytes) has a valid
/// trailing big-endian CRC.
bool rx_crc_valid(const uint8_t *frame);

/// Parses an already CRC-validated RX_FRAME_LEN-byte frame. Behaviour is
/// undefined if the frame has not been validated first.
DisplayFrame parse_display_frame(const uint8_t *frame);

/// The keep-alive frame the hub sends once at startup (unless read_only):
/// 04 06 FF FF FF 10 FC E8. Header/data verified byte-for-byte on hardware.
std::array<uint8_t, TX_FRAME_LEN> build_alive_frame();

/// The frame for one instant of a keypress. Sending it once is a tap; sending
/// it repeatedly for as long as the key is logically held is a hold -- there
/// is no key-up frame, so release is simply silence. That repetition is a
/// later stage's concern (the keypad); this only builds the bytes.
std::array<uint8_t, TX_FRAME_LEN> build_key_frame(KeyMask mask);

/// Byte-at-a-time RX state machine. Feed UART bytes one at a time; returns
/// true and fills `*out` whenever a complete, CRC-valid frame lands.
///
/// Framing rules, both earned by getting them wrong once:
///  - While hunting for sync, a non-0x02 byte is skipped, not treated as an
///    error -- the old implementation aborted the whole read on a stray byte,
///    which meant a single glitch stalled resync until the caller looped back
///    around and could overrun the UART's RX FIFO in the meantime.
///  - On a CRC failure, the buffered bytes are rescanned for the next
///    plausible 0x02 rather than discarded outright. A CRC failure often
///    means we locked onto a 0x02 that occurs inside a previous frame's
///    payload, not that garbage arrived; rescanning lets us resync within the
///    same frame period instead of waiting out an entire extra one.
class Framer {
 public:
  bool feed(uint8_t byte, DisplayFrame *out);

  uint32_t frames_received() const { return frames_received_; }

  /// Candidate frames rejected for a bad CRC -- not a count of physically
  /// corrupted frames. One burst of line noise can increment this several
  /// times as resync_() walks forward through the buffer trying successive
  /// 0x02 bytes. It is a link-health signal ("is this number climbing?"),
  /// not a precise loss figure.
  uint32_t frames_dropped() const { return frames_dropped_; }

 private:
  void resync_();

  std::array<uint8_t, RX_FRAME_LEN> buf_{};
  uint8_t len_{0};
  uint32_t frames_received_{0};
  uint32_t frames_dropped_{0};
};

}  // namespace protocol
}  // namespace vent_axia
}  // namespace esphome
