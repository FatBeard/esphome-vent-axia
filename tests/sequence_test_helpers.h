#pragma once

// Shared fixtures for the sequence engine's test files (test_sequence.cpp,
// test_sync_clock.cpp): a fake keypad/display driven by an explicit clock,
// plus recording sinks for transmitted frames and log severities. Not a
// test_*.cpp itself (tests/CMakeLists.txt only globs those) -- pulled out of
// test_sequence.cpp once a second file (test_sync_clock.cpp) needed the same
// harness, rather than duplicating it.

#include <string>
#include <utility>
#include <vector>

#include "frame_test_helper.h"
#include "sequence.h"

namespace vatest {

using esphome::vent_axia::Keypad;
using esphome::vent_axia::protocol::KeyMask;

constexpr KeyMask UP = esphome::vent_axia::protocol::key_mask(esphome::vent_axia::protocol::Key::UP);
constexpr KeyMask DOWN = esphome::vent_axia::protocol::key_mask(esphome::vent_axia::protocol::Key::DOWN);
constexpr KeyMask SET = esphome::vent_axia::protocol::key_mask(esphome::vent_axia::protocol::Key::SET);
constexpr KeyMask MAIN = esphome::vent_axia::protocol::key_mask(esphome::vent_axia::protocol::Key::MAIN);

/// Records every frame's mask alongside the `now_ms` it was sent at (via
/// `current_now`, set by the test's own tick loop) so a test can tell a
/// single held-down episode's retransmits apart from a genuinely new press
/// -- see episodes_from() below. Same shape as test_keypad.cpp's
/// RecordingSink, plus the timestamp GotoMenu/LeaveMenu/FetchDiagnostics/
/// SyncClock tests need and test_keypad.cpp's don't.
struct RecordingSink {
  std::vector<std::pair<uint32_t, KeyMask>> frames;
  const uint32_t *current_now{nullptr};

  Keypad::FrameSink as_frame_sink() {
    return [this](const uint8_t *data, size_t len) {
      (void) len;
      this->frames.emplace_back(*this->current_now, data[5]);
    };
  }
};

/// Collapses a RecordingSink's raw frames (several per press, retransmitted
/// every tx_interval) into one entry per press EPISODE: a run of frames of
/// the same mask is one episode, a new episode starts only after a gap
/// bigger than one tx_interval could explain (100ms sits comfortably
/// between the 20ms retransmit cadence and the 400ms mandatory key_gap, so
/// it can only mean "the key actually went up and came back down", never a
/// slow retransmit). This is what lets a test assert "exactly 5 Up taps",
/// not just "some Up frames appeared".
inline std::vector<KeyMask> episodes_from(const RecordingSink &sink) {
  std::vector<KeyMask> episodes;
  uint32_t last_ts = 0;
  bool have_last = false;
  for (const auto &f : sink.frames) {
    if (!have_last || f.first - last_ts > 100) {
      episodes.push_back(f.second);
    }
    last_ts = f.first;
    have_last = true;
  }
  return episodes;
}

/// Records severities the same way test_keypad.cpp's RecordingLog does.
/// Reused for both Runner::LogSink and Keypad::LogSink -- the same type
/// (`using LogSink = Keypad::LogSink`, see sequence.h), so one fixture
/// covers both. Every Sequence reaches the sink through its Runner (see
/// Sequence::log()), so setting it on the Runner is enough to observe what
/// any sequence under test logs.
struct RecordingLog {
  int info_count{0};
  int warn_count{0};
  int error_count{0};
  std::string last_info;
  std::string last_warn;
  std::string last_error;

  Keypad::LogSink as_log_sink() {
    Keypad::LogSink sink;
    sink.info = [this](const std::string &m) {
      this->info_count++;
      this->last_info = m;
    };
    sink.warn = [this](const std::string &m) {
      this->warn_count++;
      this->last_warn = m;
    };
    sink.error = [this](const std::string &m) {
      this->error_count++;
      this->last_error = m;
    };
    return sink;
  }
};

/// Drives kp and runner together, exactly as VentAxiaHub::loop() does --
/// both pumped independently every tick with the same now_ms (vent_axia.cpp:
/// "Runner sits alongside Keypad, not on top of it").
struct Clock {
  Keypad &kp;
  esphome::vent_axia::Runner &runner;
  uint32_t now{0};

  void tick() {
    this->now += 20;
    this->kp.loop(this->now);
    this->runner.loop(this->now);
  }
  void advance(uint32_t ms) {
    const uint32_t target = this->now + ms;
    while (this->now < target) {
      this->tick();
    }
  }
};

}  // namespace vatest
