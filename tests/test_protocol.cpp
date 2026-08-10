#include "test_framework.h"

#include <vector>

#include "frame_test_helper.h"
#include "protocol.h"

using namespace esphome::vent_axia::protocol;

TEST_CASE(alive_frame_matches_hardware_capture) {
  const std::array<uint8_t, TX_FRAME_LEN> expected = {0x04, 0x06, 0xFF, 0xFF, 0xFF, 0x10, 0xFC, 0xE8};
  CHECK(build_alive_frame() == expected);
}

TEST_CASE(key_frames_match_hardware_capture) {
  struct Case {
    KeyMask mask;
    std::array<uint8_t, TX_FRAME_LEN> expected;
  };
  const Case cases[] = {
      {key_mask(Key::DOWN), {0x04, 0x05, 0xAF, 0xEF, 0xFB, 0x01, 0xFD, 0x5C}},
      {key_mask(Key::UP), {0x04, 0x05, 0xAF, 0xEF, 0xFB, 0x02, 0xFD, 0x5B}},
      {static_cast<KeyMask>(key_mask(Key::UP) | key_mask(Key::DOWN)),
       {0x04, 0x05, 0xAF, 0xEF, 0xFB, 0x03, 0xFD, 0x5A}},
      {key_mask(Key::SET), {0x04, 0x05, 0xAF, 0xEF, 0xFB, 0x04, 0xFD, 0x59}},
      {key_mask(Key::MAIN), {0x04, 0x05, 0xAF, 0xEF, 0xFB, 0x08, 0xFD, 0x55}},
      {static_cast<KeyMask>(key_mask(Key::UP) | key_mask(Key::MAIN)),
       {0x04, 0x05, 0xAF, 0xEF, 0xFB, 0x0A, 0xFD, 0x53}},
  };
  for (const auto &c : cases) {
    CHECK(build_key_frame(c.mask) == c.expected);
  }
}

TEST_CASE(rx_crc_accepts_valid_frame) {
  const auto frame = vatest::build_rx_frame("Status", "18%             ");
  CHECK(rx_crc_valid(frame.data()));
}

TEST_CASE(rx_crc_rejects_corrupted_frame) {
  auto frame = vatest::build_rx_frame("Status", "18%             ");
  frame[10] ^= 0xFF;  // flip a byte inside line1's text
  CHECK(!rx_crc_valid(frame.data()));
}

TEST_CASE(framer_skips_junk_before_sync_and_produces_one_frame) {
  Framer framer;
  const auto good = vatest::build_rx_frame("Line One Screen", "Line Two Screen");

  std::vector<uint8_t> stream = {0xFF, 0x00, 0x9A};  // pre-sync junk, no embedded 0x02
  stream.insert(stream.end(), good.begin(), good.end());

  DisplayFrame out;
  DisplayFrame last;
  int frame_count = 0;
  for (uint8_t b : stream) {
    if (framer.feed(b, &out)) {
      frame_count++;
      last = out;
    }
  }

  CHECK_EQ(frame_count, 1);
  CHECK_EQ(last.line1, vatest::pad16("Line One Screen"));
  CHECK_EQ(last.line2, vatest::pad16("Line Two Screen"));
  CHECK_EQ(framer.frames_received(), static_cast<uint32_t>(1));
  CHECK_EQ(framer.frames_dropped(), static_cast<uint32_t>(0));
}

TEST_CASE(framer_resyncs_after_a_dropped_byte) {
  Framer framer;
  auto frame1 = vatest::build_rx_frame("First Screen", "Line Two One");
  const auto frame2 = vatest::build_rx_frame("Second Screen", "Line Two Two");

  std::vector<uint8_t> stream(frame1.begin(), frame1.end());
  stream.erase(stream.begin() + 20);  // simulate one byte lost mid-frame
  stream.insert(stream.end(), frame2.begin(), frame2.end());

  DisplayFrame out;
  DisplayFrame last;
  int frame_count = 0;
  for (uint8_t b : stream) {
    if (framer.feed(b, &out)) {
      frame_count++;
      last = out;
    }
  }

  CHECK_EQ(frame_count, 1);
  CHECK_EQ(last.line1, vatest::pad16("Second Screen"));
  CHECK_EQ(last.line2, vatest::pad16("Line Two Two"));
  CHECK_EQ(framer.frames_dropped(), static_cast<uint32_t>(1));
}

TEST_CASE(framer_parses_frame_with_sync_byte_embedded_in_text) {
  Framer framer;
  std::string line2 = std::string("Bo") + static_cast<char>(RX_SYNC_BYTE) + "st On";
  line2 = vatest::pad16(line2);
  const auto frame = vatest::build_rx_frame("Status", line2);

  DisplayFrame out;
  int frame_count = 0;
  for (uint8_t b : frame) {
    if (framer.feed(b, &out)) {
      frame_count++;
    }
  }

  CHECK_EQ(frame_count, 1);
  CHECK_EQ(out.line1, vatest::pad16("Status"));
  CHECK_EQ(out.line2, line2);
}

TEST_CASE(framer_counts_bad_frames_and_does_not_emit_them) {
  Framer framer;
  auto frame = vatest::build_rx_frame("Status", "18%             ");
  frame[10] ^= 0xFF;

  DisplayFrame out;
  int frame_count = 0;
  for (uint8_t b : frame) {
    if (framer.feed(b, &out)) {
      frame_count++;
    }
  }

  CHECK_EQ(frame_count, 0);
  CHECK_EQ(framer.frames_received(), static_cast<uint32_t>(0));
  CHECK_EQ(framer.frames_dropped(), static_cast<uint32_t>(1));
}
