#include "keypad.h"

#include <cstdio>

namespace esphome {
namespace vent_axia {

namespace {

/// Renders a mask as key names ("UP+MAIN") rather than a bare hex byte, for
/// the log lines below -- this is meant to be read by a human debugging the
/// real unit, and "UP+MAIN" says a lot more at a glance than "0x0A".
std::string describe_mask(protocol::KeyMask mask) {
  if (mask == 0) {
    return "none";
  }
  std::string out;
  auto add = [&](protocol::Key key, const char *name) {
    if ((mask & protocol::key_mask(key)) != 0) {
      if (!out.empty()) {
        out += "+";
      }
      out += name;
    }
  };
  add(protocol::Key::UP, "UP");
  add(protocol::Key::DOWN, "DOWN");
  add(protocol::Key::SET, "SET");
  add(protocol::Key::MAIN, "MAIN");
  return out;
}

}  // namespace

void Keypad::press(protocol::KeyMask mask) {
  // Re-asserting a hold already in effect is a no-op, NOT a restart.
  //
  // "Keep holding Up until the screen changes" is a natural thing to write as
  // a poll loop calling press() every tick, and the sequence engine's long
  // holds (8s Down to scroll the diagnostic menu, 5.5s Main for purge) are
  // exactly that shape. If every call reset press_started_ms_, the watchdog's
  // 30s clock would restart on every tick and the one backstop guaranteeing a
  // key cannot stick would never fire -- silently, and precisely in the case
  // it exists to catch.
  if (this->state_ == State::HOLD && this->asserted_mask_ == mask && !this->pending_release_) {
    return;
  }
  this->have_pending_hold_ = true;
  this->pending_hold_mask_ = mask;
  // A hold asked for after a release within the same tick supersedes it --
  // see release() for the other half of this.
  this->pending_release_ = false;
}

void Keypad::release() {
  this->pending_release_ = true;
  // Cancels a hold requested earlier in the same tick. Both requests are
  // resolved in loop(), which handles the release flag first, so without this
  // a press() followed by a release() before the next tick would leave the key
  // *held* -- the opposite of what the caller last asked for.
  this->have_pending_hold_ = false;
}

void Keypad::tap(protocol::KeyMask mask, uint32_t duration_ms) {
  if (this->queue_count_ >= QUEUE_CAPACITY) {
    this->dropped_taps_++;
    if (this->log_.warn) {
      this->log_.warn("keypad: tap queue full (" + std::to_string(QUEUE_CAPACITY) + "), dropping newest tap (" +
                       describe_mask(mask) + ")");
    }
    return;
  }
  this->queue_[(this->queue_head_ + this->queue_count_) % QUEUE_CAPACITY] = QueuedTap{mask, duration_ms};
  this->queue_count_++;
}

bool Keypad::busy() const {
  return this->state_ != State::IDLE || this->queue_count_ > 0 || this->have_pending_hold_;
}

void Keypad::hard_release_() {
  this->state_ = State::IDLE;
  this->asserted_mask_ = 0;
  this->queue_head_ = 0;
  this->queue_count_ = 0;
  this->have_tx_mask_ = false;
}

Keypad::QueuedTap Keypad::pop_queued_tap_() {
  const QueuedTap next = this->queue_[this->queue_head_];
  this->queue_head_ = static_cast<uint8_t>((this->queue_head_ + 1) % QUEUE_CAPACITY);
  this->queue_count_--;
  return next;
}

void Keypad::loop(uint32_t now_ms) {
  // Watchdog first, independent of everything below: no matter what state
  // this tick's caller believes it left things in, a mask asserted
  // continuously for key_watchdog_ms_ gets force-cleared. This is the
  // backstop the old setup needed four separate YAML watchdog scripts for
  // (PLAN.md §2's "Watchdog") -- nothing here should be able to leave a key
  // stuck, including a bug in this very class.
  if (this->asserted_mask_ != 0 && (now_ms - this->press_started_ms_) >= this->key_watchdog_ms_) {
    if (this->log_.error) {
      this->log_.error("keypad: watchdog force-releasing " + describe_mask(this->asserted_mask_) + " after " +
                        std::to_string(this->key_watchdog_ms_) + "ms held continuously");
    }
    this->watchdog_releases_++;
    this->hard_release_();
  }

  if (this->pending_release_) {
    this->pending_release_ = false;
    this->hard_release_();
  }

  if (this->have_pending_hold_) {
    this->have_pending_hold_ = false;
    this->hard_release_();  // a fresh hold pre-empts whatever was in progress or queued
    this->state_ = State::HOLD;
    this->asserted_mask_ = this->pending_hold_mask_;
    this->press_started_ms_ = now_ms;
    this->frames_this_episode_ = 0;
  }

  switch (this->state_) {
    case State::TAP_PRESS:
      if (now_ms - this->phase_started_ms_ >= this->current_tap_duration_ms_) {
        // Evaluated only here, at a tap's natural end -- PLAN.md risk 1's
        // diagnostic. A press cut short by press()/release() pre-empting it
        // did not run to completion, so counting it here would conflate
        // "interrupted on purpose" with "silently dropped by a stall".
        if (this->frames_this_episode_ < 2) {
          this->under_emitting_presses_++;
          if (this->log_.warn) {
            this->log_.warn("keypad: tap of " + describe_mask(this->asserted_mask_) + " emitted only " +
                             std::to_string(this->frames_this_episode_) +
                             " frame(s) -- a loop() stall may have landed inside it; consider raising "
                             "tap_duration if this keeps happening");
          }
        }
        this->state_ = State::TAP_GAP;
        this->asserted_mask_ = 0;
        this->phase_started_ms_ = now_ms;
      }
      break;
    case State::TAP_GAP:
      if (now_ms - this->phase_started_ms_ >= this->key_gap_ms_) {
        this->state_ = State::IDLE;
      }
      break;
    case State::IDLE:
    case State::HOLD:
      break;
  }

  if (this->state_ == State::IDLE && this->queue_count_ > 0) {
    const QueuedTap next = this->pop_queued_tap_();
    this->state_ = State::TAP_PRESS;
    this->asserted_mask_ = next.mask;
    this->current_tap_duration_ms_ = next.duration_ms;
    this->phase_started_ms_ = now_ms;
    this->press_started_ms_ = now_ms;
    this->frames_this_episode_ = 0;
  }

  this->maybe_transmit_(now_ms);
}

void Keypad::maybe_transmit_(uint32_t now_ms) {
  if (this->asserted_mask_ == 0) {
    this->have_tx_mask_ = false;  // next assertion must send immediately, whatever it is
    return;                       // silence is the release -- nothing to send, ever
  }

  const bool mask_changed = !this->have_tx_mask_ || this->asserted_mask_ != this->last_tx_mask_;
  const bool interval_elapsed = (now_ms - this->last_tx_ms_) >= this->tx_interval_ms_;
  // Send immediately whenever the asserted mask changes to something
  // non-zero, then every tx_interval_ms_ while it stays asserted (PLAN.md
  // risk 1): ESPHome's loop() can stall for tens of ms during Wi-Fi
  // reconnects, API bursts and OTA, and without the immediate send a stall
  // landing inside a short tap could emit zero frames -- a silently dropped
  // keypress, exactly what key_gap was raised to eliminate.
  if (!mask_changed && !interval_elapsed) {
    return;
  }

  if (this->read_only_) {
    // "At least once", not once per retransmit: logging every tx_interval_ms_
    // tick for a held key would flood the log for no extra information, so
    // this only fires on the mask actually changing -- the same edge that
    // would have triggered the immediate send above.
    if (mask_changed && this->log_.info) {
      this->log_.info("keypad: read_only, suppressing " + describe_mask(this->asserted_mask_) +
                       " (would have transmitted)");
    }
  } else if (this->frame_sink_) {
    const auto frame = protocol::build_key_frame(this->asserted_mask_);
    this->frame_sink_(frame.data(), frame.size());
  }

  this->last_tx_mask_ = this->asserted_mask_;
  this->have_tx_mask_ = true;
  this->last_tx_ms_ = now_ms;
  // Counted the same way whether or not read_only_ suppressed the actual
  // send: this counts transmission *events* the state machine decided to
  // take, which is what the under-emit diagnostic above needs, not bytes
  // that reached a UART.
  this->frames_this_episode_++;
}

}  // namespace vent_axia
}  // namespace esphome
