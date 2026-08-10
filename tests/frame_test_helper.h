#pragma once

// Not a test_*.cpp itself (tests/CMakeLists.txt only globs those), just a
// helper shared by test_protocol.cpp and test_display.cpp so fixture frames
// read as "here is the screen the unit is showing" instead of raw offsets.

#include <array>
#include <cstdint>
#include <string>

#include "protocol.h"

namespace vatest {

/// Pads/truncates to the protocol's fixed 16-char line width, matching what
/// the real unit sends (space padded, never null-terminated).
inline std::string pad16(const std::string &s) {
  std::string out = s.substr(0, esphome::vent_axia::protocol::LINE_LEN);
  out.resize(esphome::vent_axia::protocol::LINE_LEN, ' ');
  return out;
}

/// Builds a valid, CRC-correct 41-byte RX frame from two display lines.
/// The header/address bytes are unparsed by this stage, so any plausible
/// fixed values do -- they're chosen to be non-zero and distinct so a test
/// that dumps a frame on failure can tell them apart from the text.
inline std::array<uint8_t, esphome::vent_axia::protocol::RX_FRAME_LEN> build_rx_frame(const std::string &line1_raw,
                                                                              const std::string &line2_raw) {
  using namespace esphome::vent_axia::protocol;  // NOLINT
  const std::string line1 = pad16(line1_raw);
  const std::string line2 = pad16(line2_raw);

  std::array<uint8_t, RX_FRAME_LEN> frame{};
  frame[0] = RX_SYNC_BYTE;
  frame[1] = 0x11;
  frame[2] = 0x22;
  frame[3] = 0x33;
  frame[4] = 0x44;
  frame[5] = 0x80;  // plausible HD44780 DDRAM address, row 1
  for (uint8_t i = 0; i < LINE_LEN; i++) {
    frame[6 + i] = static_cast<uint8_t>(line1[i]);
  }
  frame[22] = 0xC0;  // plausible HD44780 DDRAM address, row 2
  for (uint8_t i = 0; i < LINE_LEN; i++) {
    frame[23 + i] = static_cast<uint8_t>(line2[i]);
  }
  const uint16_t crc = running_crc(frame.data(), RX_FRAME_LEN - 2);
  frame[39] = static_cast<uint8_t>(crc >> 8);
  frame[40] = static_cast<uint8_t>(crc & 0xFF);
  return frame;
}

}  // namespace vatest
