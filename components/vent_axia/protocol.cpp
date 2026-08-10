#include "protocol.h"

namespace esphome {
namespace vent_axia {
namespace protocol {

uint16_t running_crc(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc = static_cast<uint16_t>(crc - data[i]);
  }
  return crc;
}

bool rx_crc_valid(const uint8_t *frame) {
  const uint16_t computed = running_crc(frame, RX_FRAME_LEN - 2);
  const uint16_t expected =
      static_cast<uint16_t>((static_cast<uint16_t>(frame[RX_FRAME_LEN - 2]) << 8) | frame[RX_FRAME_LEN - 1]);
  return computed == expected;
}

DisplayFrame parse_display_frame(const uint8_t *frame) {
  DisplayFrame out;
  for (size_t i = 0; i < out.unknown_header.size(); i++) {
    out.unknown_header[i] = frame[1 + i];
  }
  out.unknown_row1_addr = frame[5];
  out.line1.assign(reinterpret_cast<const char *>(frame + 6), LINE_LEN);
  out.unknown_row2_addr = frame[22];
  out.line2.assign(reinterpret_cast<const char *>(frame + 23), LINE_LEN);
  return out;
}

namespace {

std::array<uint8_t, TX_FRAME_LEN> build_frame(const std::array<uint8_t, 4> &header, uint8_t data) {
  std::array<uint8_t, TX_FRAME_LEN> frame{};
  frame[0] = TX_SYNC_BYTE;
  frame[1] = header[0];
  frame[2] = header[1];
  frame[3] = header[2];
  frame[4] = header[3];
  frame[5] = data;
  const uint16_t crc = running_crc(frame.data(), 6);
  frame[6] = static_cast<uint8_t>(crc >> 8);
  frame[7] = static_cast<uint8_t>(crc & 0xFF);
  return frame;
}

}  // namespace

std::array<uint8_t, TX_FRAME_LEN> build_alive_frame() {
  return build_frame({0x06, 0xFF, 0xFF, 0xFF}, 0x10);
}

std::array<uint8_t, TX_FRAME_LEN> build_key_frame(KeyMask mask) {
  return build_frame({0x05, 0xAF, 0xEF, 0xFB}, mask);
}

bool Framer::feed(uint8_t byte, DisplayFrame *out) {
  if (len_ == 0) {
    if (byte != RX_SYNC_BYTE) {
      return false;  // hunting for sync: skip, don't abort
    }
    buf_[len_++] = byte;
    return false;
  }

  buf_[len_++] = byte;
  if (len_ < RX_FRAME_LEN) {
    return false;
  }

  // Buffer is full: this is a complete candidate frame.
  if (rx_crc_valid(buf_.data())) {
    *out = parse_display_frame(buf_.data());
    frames_received_++;
    len_ = 0;
    return true;
  }

  frames_dropped_++;
  resync_();
  return false;
}

void Framer::resync_() {
  // buf_[0] is always 0x02 (that's the only way len_ becomes nonzero), so
  // start the rescan at index 1: it already failed as a frame start.
  for (uint8_t i = 1; i < len_; i++) {
    if (buf_[i] == RX_SYNC_BYTE) {
      const uint8_t new_len = static_cast<uint8_t>(len_ - i);
      for (uint8_t j = 0; j < new_len; j++) {
        buf_[j] = buf_[i + j];
      }
      len_ = new_len;
      return;
    }
  }
  len_ = 0;  // nothing plausible buffered; go back to hunting on the wire
}

}  // namespace protocol
}  // namespace vent_axia
}  // namespace esphome
