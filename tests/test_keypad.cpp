#include "test_framework.h"

#include <vector>

#include "keypad.h"

using namespace esphome::vent_axia;

namespace {

/// Records every frame handed to the sink, byte-for-byte, plus the key mask
/// each one carried (byte 5 of a key frame -- see protocol::build_key_frame)
/// so a test can assert on masks without re-deriving frame layout.
struct RecordingSink {
  std::vector<std::vector<uint8_t>> frames;

  Keypad::FrameSink as_frame_sink() {
    return [this](const uint8_t *data, size_t len) { this->frames.emplace_back(data, data + len); };
  }

  size_t count() const { return this->frames.size(); }
  protocol::KeyMask mask_of(size_t i) const { return this->frames.at(i).at(5); }
};

/// Records how many times each severity fired, and the last message at each
/// -- enough to assert "this warned" without pattern-matching log text.
struct RecordingLog {
  int info_count{0};
  int warn_count{0};
  int error_count{0};
  std::string last_warn;
  std::string last_error;

  Keypad::LogSink as_log_sink() {
    Keypad::LogSink sink;
    sink.info = [this](const std::string &) { this->info_count++; };
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

constexpr protocol::KeyMask UP = protocol::key_mask(protocol::Key::UP);
constexpr protocol::KeyMask DOWN = protocol::key_mask(protocol::Key::DOWN);
constexpr protocol::KeyMask SET = protocol::key_mask(protocol::Key::SET);
constexpr protocol::KeyMask MAIN = protocol::key_mask(protocol::Key::MAIN);

}  // namespace

// ------------------------------------------------------------- tap() --

TEST_CASE(tap_emits_its_first_frame_immediately) {
  RecordingSink sink;
  Keypad kp;
  kp.set_frame_sink(sink.as_frame_sink());

  kp.tap(UP, 50);
  CHECK_EQ(sink.count(), static_cast<size_t>(0));  // nothing sent until loop() runs

  kp.loop(1000);  // the very next tick, whatever now_ms happens to be
  CHECK_EQ(sink.count(), static_cast<size_t>(1));
  CHECK_EQ(sink.mask_of(0), UP);
}

TEST_CASE(tap_emits_roughly_duration_over_tx_interval_frames) {
  // 50ms tap, 20ms tx_interval: frames at t=0 (immediate), 20, 40 -- three
  // frames, matching PLAN.md's "one human press produces 6-13 frames over
  // 110-260ms" cadence scaled down to tap_duration.
  RecordingSink sink;
  Keypad kp;
  kp.set_frame_sink(sink.as_frame_sink());
  kp.tap(UP, 50);

  for (uint32_t t = 0; t <= 50; t += 5) {
    kp.loop(t);
  }

  CHECK_EQ(sink.count(), static_cast<size_t>(3));
}

TEST_CASE(no_frame_is_emitted_during_the_gap) {
  RecordingSink sink;
  Keypad kp;
  kp.set_frame_sink(sink.as_frame_sink());
  kp.tap(UP, 50);

  kp.loop(0);                          // first frame
  const size_t before_gap = sink.count();
  CHECK(before_gap > 0);

  // Step through the tap's end and the whole 400ms gap in small increments;
  // no new frame should appear once the mask has gone back to zero.
  for (uint32_t t = 50; t <= 450; t += 10) {
    kp.loop(t);
  }

  CHECK_EQ(sink.count(), before_gap);  // nothing new since the tap itself ended
}

TEST_CASE(a_second_queued_tap_does_not_start_until_the_gap_has_elapsed) {
  RecordingSink sink;
  Keypad kp;
  kp.set_frame_sink(sink.as_frame_sink());
  kp.set_key_gap_ms(400);
  kp.tap(UP, 50);
  kp.tap(DOWN, 50);

  kp.loop(0);  // UP's first frame

  // Walk up to (but not past) when UP's tap+gap should still be running:
  // tap ends at 50, gap ends at 50+400=450. Just before that, the second
  // tap must not have started -- i.e. no DOWN frame yet.
  for (uint32_t t = 10; t < 450; t += 10) {
    kp.loop(t);
    for (size_t i = 0; i < sink.count(); i++) {
      CHECK(sink.mask_of(i) != DOWN);
    }
  }

  kp.loop(450);  // gap has now elapsed
  kp.loop(451);  // next tick: the queued tap starts and sends immediately

  bool saw_down = false;
  for (size_t i = 0; i < sink.count(); i++) {
    if (sink.mask_of(i) == DOWN) {
      saw_down = true;
    }
  }
  CHECK(saw_down);
}

TEST_CASE(busy_is_true_for_a_tap_and_its_trailing_gap_false_once_idle) {
  Keypad kp;
  RecordingSink sink;
  kp.set_frame_sink(sink.as_frame_sink());
  kp.set_key_gap_ms(400);

  CHECK(!kp.busy());
  kp.tap(UP, 50);
  CHECK(kp.busy());  // true the instant it's queued, before loop() even runs

  kp.loop(0);
  CHECK(kp.busy());   // pressing
  kp.loop(60);
  CHECK(kp.busy());   // in the gap now, still busy
  kp.loop(460);
  CHECK(!kp.busy());  // gap elapsed, queue empty -- idle again
}

// ------------------------------------------------------------ press() --

TEST_CASE(a_hold_keeps_emitting_indefinitely_and_stops_dead_on_release) {
  RecordingSink sink;
  Keypad kp;
  kp.set_frame_sink(sink.as_frame_sink());
  kp.set_tx_interval_ms(20);

  kp.press(DOWN);
  kp.loop(0);
  CHECK_EQ(sink.count(), static_cast<size_t>(1));  // immediate send

  // Retransmits every tx_interval for as long as it's held -- well past a
  // single human press's duration, proving this doesn't self-terminate.
  for (uint32_t t = 20; t <= 2000; t += 20) {
    kp.loop(t);
  }
  const size_t frames_while_held = sink.count();
  CHECK(frames_while_held >= static_cast<size_t>(2000 / 20));

  kp.release();
  kp.loop(2001);  // release() takes effect on the next loop() call
  CHECK(!kp.busy());

  const size_t after_release = sink.count();
  for (uint32_t t = 2020; t <= 2200; t += 20) {
    kp.loop(t);
  }
  CHECK_EQ(sink.count(), after_release);  // stone dead -- no further frames, ever
}

TEST_CASE(watchdog_force_releases_a_hold_at_30_seconds) {
  RecordingSink sink;
  RecordingLog log;
  Keypad kp;
  kp.set_frame_sink(sink.as_frame_sink());
  kp.set_log_sink(log.as_log_sink());

  kp.press(UP);
  kp.loop(0);
  CHECK(kp.busy());
  CHECK_EQ(kp.watchdog_releases(), static_cast<uint32_t>(0));

  kp.loop(29999);
  CHECK(kp.busy());  // not yet -- one ms short of the default 30s watchdog

  kp.loop(30000);
  CHECK(!kp.busy());  // forced clear, independent of the caller ever calling release()
  CHECK_EQ(kp.watchdog_releases(), static_cast<uint32_t>(1));
  CHECK_EQ(log.error_count, 1);  // "log an error naming the mask"

  const size_t frames_at_release = sink.count();
  kp.loop(30500);
  CHECK_EQ(sink.count(), frames_at_release);  // and it stays released
}

TEST_CASE(watchdog_threshold_is_configurable) {
  Keypad kp;
  kp.set_key_watchdog_ms(1000);
  kp.press(DOWN);
  kp.loop(0);

  kp.loop(999);
  CHECK(kp.busy());
  kp.loop(1000);
  CHECK(!kp.busy());
  CHECK_EQ(kp.watchdog_releases(), static_cast<uint32_t>(1));
}

// --------------------------------------------------------- read_only() --

TEST_CASE(read_only_emits_nothing_while_the_state_machine_still_advances) {
  RecordingSink sink;
  RecordingLog log;
  Keypad kp;
  kp.set_frame_sink(sink.as_frame_sink());
  kp.set_log_sink(log.as_log_sink());
  kp.set_read_only(true);
  kp.set_key_gap_ms(400);

  kp.tap(UP, 50);
  CHECK(kp.busy());  // busy() behaves exactly as in the transmitting case

  for (uint32_t t = 0; t <= 500; t += 10) {
    kp.loop(t);
  }

  CHECK_EQ(sink.count(), static_cast<size_t>(0));  // never once reached the "wire"
  CHECK(!kp.busy());                               // but the tap+gap still ran to completion
  CHECK(log.info_count >= 1);                       // and it said so at least once
}

TEST_CASE(read_only_hold_also_advances_the_watchdog) {
  // The state machine runs exactly as normal in read_only mode (per its own
  // doc comment) -- including the watchdog, which must still be able to
  // clear a stuck read_only hold rather than leaving busy() stuck true
  // forever.
  Keypad kp;
  kp.set_read_only(true);
  kp.set_key_watchdog_ms(1000);

  kp.press(UP);
  kp.loop(0);
  CHECK(kp.busy());
  kp.loop(1000);
  CHECK(!kp.busy());
  CHECK_EQ(kp.watchdog_releases(), static_cast<uint32_t>(1));
}

// ------------------------------------------------- under-emit counter --

TEST_CASE(under_emitting_counter_increments_when_loop_is_called_too_sparsely) {
  // tap_duration 50ms, tx_interval 20ms: calling loop() only at the start
  // and the end of the tap -- as if a loop() stall swallowed everything in
  // between -- must fit only one frame (the immediate one) instead of the
  // ~3 a healthy cadence would produce, and that must be counted and
  // logged. This is PLAN.md risk 1's failure mode, made diagnosable.
  RecordingSink sink;
  RecordingLog log;
  Keypad kp;
  kp.set_frame_sink(sink.as_frame_sink());
  kp.set_log_sink(log.as_log_sink());
  kp.set_tx_interval_ms(20);

  CHECK_EQ(kp.under_emitting_presses(), static_cast<uint32_t>(0));

  kp.tap(UP, 50);
  kp.loop(0);    // immediate frame -- 1 total
  kp.loop(51);   // jumps straight past the tap's end: only 1 frame ever went out

  CHECK_EQ(sink.count(), static_cast<size_t>(1));
  CHECK_EQ(kp.under_emitting_presses(), static_cast<uint32_t>(1));
  CHECK(log.warn_count >= 1);
}

TEST_CASE(a_healthily_ticked_tap_does_not_count_as_under_emitting) {
  Keypad kp;
  RecordingSink sink;
  kp.set_frame_sink(sink.as_frame_sink());
  kp.set_tx_interval_ms(20);

  kp.tap(UP, 50);
  for (uint32_t t = 0; t <= 50; t += 5) {
    kp.loop(t);
  }

  CHECK(sink.count() >= static_cast<size_t>(2));
  CHECK_EQ(kp.under_emitting_presses(), static_cast<uint32_t>(0));
}

TEST_CASE(an_interrupted_tap_is_not_counted_as_under_emitting) {
  // press() pre-empting a queued/running tap is a deliberate interruption,
  // not a silently dropped press -- see loop()'s comment on why the counter
  // is only evaluated at a tap's natural end.
  Keypad kp;
  kp.set_tx_interval_ms(20);
  kp.tap(UP, 50);
  kp.loop(0);
  kp.press(DOWN);  // pre-empts the in-flight tap before it reaches 50ms
  kp.loop(10);

  CHECK_EQ(kp.under_emitting_presses(), static_cast<uint32_t>(0));
}

// --------------------------------------------------------- queue full --

TEST_CASE(queue_full_drops_the_newest_tap_and_logs_it) {
  RecordingLog log;
  Keypad kp;
  kp.set_log_sink(log.as_log_sink());
  // Never call loop(): every tap stays queued, so the 9th call finds the
  // 8-deep queue already full.
  for (int i = 0; i < Keypad::QUEUE_CAPACITY; i++) {
    kp.tap(UP, 50);
  }
  CHECK_EQ(kp.dropped_taps(), static_cast<uint32_t>(0));
  CHECK_EQ(log.warn_count, 0);

  kp.tap(DOWN, 50);  // 9th -- must be dropped, not silently ignored
  CHECK_EQ(kp.dropped_taps(), static_cast<uint32_t>(1));
  CHECK_EQ(log.warn_count, 1);

  kp.tap(SET, 50);  // 10th -- also dropped
  CHECK_EQ(kp.dropped_taps(), static_cast<uint32_t>(2));
  CHECK_EQ(log.warn_count, 2);
}

TEST_CASE(queue_full_drop_does_not_disturb_the_already_queued_taps) {
  // The dropped tap must be the *newest* one, not one already queued -- so
  // draining the queue afterwards must produce exactly the 8 masks that
  // were accepted, in order, none of them the one that got dropped.
  RecordingSink sink;
  Keypad kp;
  kp.set_frame_sink(sink.as_frame_sink());
  kp.set_key_gap_ms(0);  // drain the queue as fast as possible

  for (int i = 0; i < Keypad::QUEUE_CAPACITY; i++) {
    kp.tap(UP, 10);
  }
  kp.tap(SET, 10);  // dropped: queue was already full of UP taps
  CHECK_EQ(kp.dropped_taps(), static_cast<uint32_t>(1));

  uint32_t now = 0;
  int ups_seen = 0;
  bool saw_set = false;
  for (int i = 0; i < Keypad::QUEUE_CAPACITY + 2; i++) {
    kp.loop(now);
    now += 10;
    kp.loop(now);  // gap is 0ms, so this tick both ends the tap and starts the next
    now += 1;
  }
  for (size_t i = 0; i < sink.count(); i++) {
    if (sink.mask_of(i) == UP) {
      ups_seen++;
    }
    if (sink.mask_of(i) == SET) {
      saw_set = true;
    }
  }
  CHECK(ups_seen > 0);
  CHECK(!saw_set);
}

// -------------------------------------------------------- interactions --

TEST_CASE(press_preempts_a_queued_tap) {
  RecordingSink sink;
  Keypad kp;
  kp.set_frame_sink(sink.as_frame_sink());
  kp.tap(UP, 5000);  // long tap, still queued/pressing when press() below fires
  kp.loop(0);
  CHECK_EQ(sink.mask_of(0), UP);

  kp.press(SET);
  kp.loop(1);
  CHECK_EQ(sink.mask_of(sink.count() - 1), SET);

  kp.release();
  kp.loop(2);
  CHECK(!kp.busy());  // the pre-empted UP tap must not resume after release()
}

TEST_CASE(release_with_nothing_asserted_is_a_harmless_no_op) {
  Keypad kp;
  RecordingSink sink;
  kp.set_frame_sink(sink.as_frame_sink());
  kp.release();
  kp.loop(0);
  CHECK(!kp.busy());
  CHECK_EQ(sink.count(), static_cast<size_t>(0));
}

TEST_CASE(re_pressing_a_held_mask_does_not_restart_the_watchdog) {
  // "Hold Up until the screen changes" is naturally written as a poll loop
  // that calls press() every tick. If each call restarted the watchdog clock,
  // the backstop that guarantees a key cannot stick would never fire -- and
  // it would fail silently, in exactly the case it exists for.
  Keypad kp;
  RecordingSink sink;
  kp.set_frame_sink(sink.as_frame_sink());

  for (uint32_t t = 0; t < 30000; t += 20) {
    kp.press(UP);  // re-asserted every tick, as a hold-until loop would
    kp.loop(t);
  }
  CHECK_EQ(kp.watchdog_releases(), 0u);  // not yet: 30s has not fully elapsed

  kp.press(UP);
  kp.loop(30000);
  CHECK_EQ(kp.watchdog_releases(), 1u);
  CHECK(!kp.busy());
}

TEST_CASE(re_pressing_a_different_mask_does_restart_the_hold) {
  // The no-op above must be scoped to the *same* mask -- changing which keys
  // are held is a genuinely new press and gets a fresh watchdog window.
  Keypad kp;
  RecordingSink sink;
  kp.set_frame_sink(sink.as_frame_sink());

  kp.press(UP);
  kp.loop(0);
  kp.press(UP | MAIN);
  kp.loop(20);
  CHECK_EQ(sink.mask_of(sink.count() - 1), static_cast<protocol::KeyMask>(UP | MAIN));

  kp.loop(29999);
  CHECK_EQ(kp.watchdog_releases(), 0u);  // measured from the mask change at t=20
  kp.loop(30021);
  CHECK_EQ(kp.watchdog_releases(), 1u);
}

TEST_CASE(release_after_press_in_the_same_tick_wins) {
  // Both are resolved in loop(), which handles the release flag first, so
  // whichever call came last must win rather than whichever flag is read
  // first. A lost release leaves a key held.
  Keypad kp;
  RecordingSink sink;
  kp.set_frame_sink(sink.as_frame_sink());

  kp.press(UP);
  kp.release();
  kp.loop(0);

  CHECK(!kp.busy());
  CHECK_EQ(sink.count(), static_cast<size_t>(0));  // nothing was ever asserted
}

TEST_CASE(press_after_release_in_the_same_tick_wins) {
  Keypad kp;
  RecordingSink sink;
  kp.set_frame_sink(sink.as_frame_sink());

  kp.press(UP);
  kp.loop(0);
  kp.release();
  kp.press(DOWN);  // caller changed its mind again before the next tick
  kp.loop(20);

  CHECK(kp.busy());
  CHECK_EQ(sink.mask_of(sink.count() - 1), DOWN);
}
