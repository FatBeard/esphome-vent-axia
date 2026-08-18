#include "vent_axia.h"

#include <cmath>

#include "esphome/core/log.h"

#include "diagnostics.h"
#include "parser.h"

namespace esphome {
namespace vent_axia {

static const char *const TAG = "vent_axia";

void VentAxiaHub::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Vent-Axia hub...");

  // The hub does the publishing (per architecture: Display never touches
  // text_sensor::TextSensor). The callback only fires for lines that
  // actually changed, so a change to line1 alone never re-publishes line2.
  this->display_.set_on_change([this](bool line1_changed, bool line2_changed) {
    if (line1_changed) {
      // Presentation lane: this becomes an HA text sensor's state, which
      // ESPHome's native API encodes as protobuf UTF-8 -- never the raw
      // lane (DISPLAY-REVIEW.md §4/§5).
      this->publish_text_(TextKey::DISPLAY_LINE_1, this->display_.text_line1());
      // status_message is a trimmed, friendlier sibling of display_line_1
      // for the status loop specifically -- see text_sensor.py. Gated on
      // screen_kind() rather than published unconditionally so a menu
      // screen's line1 (e.g. "Set Clock") never overwrites it; the entity
      // simply holds its last status-loop value while a sequence is
      // elsewhere, same as every other status-derived entity. parser::trim
      // strips only ASCII 0x20, which stays correct against the UTF-8
      // presentation lane (DISPLAY-REVIEW.md §7).
      if (this->display_.screen_kind() == screens::ScreenKind::STATUS) {
        this->publish_text_(TextKey::STATUS_MESSAGE, parser::trim(this->display_.text_line1()));
      }
    }
    if (line2_changed) {
      this->publish_text_(TextKey::DISPLAY_LINE_2, this->display_.text_line2());
    }

    // Diagnostic decode is entirely passive (PLAN.md §4): whatever page the
    // display happens to be showing -- browsed by hand, or later scrolled
    // by a fetch sequence -- gets fed through here. Gated on either line
    // actually changing (not fired unconditionally every ~300ms frame) so
    // sitting still on one page does not spam identical publishes and
    // trigger firings; `line1_changed` alone would miss a page whose
    // content changes without its line1 changing (not expected for the
    // decoded pages, but cheap to cover), so both are checked. Both reads
    // below are the RAW lane -- diagnostic_page() is a byte-offset decoder,
    // and publish_diagnostic_page_() itself does the one necessary
    // transcode for its two presentation uses (see its own comment).
    if ((line1_changed || line2_changed) && this->display_.screen_kind() == screens::ScreenKind::DIAGNOSTIC) {
      if (const auto page = screens::diagnostic_page(this->display_.raw_line1())) {
        this->publish_diagnostic_page_(static_cast<uint8_t>(*page), this->display_.raw_line2());
      }
    }
  });

  if (this->read_only_) {
    ESP_LOGI(TAG, "read_only is set: not sending the alive frame");
  } else {
    const auto frame = protocol::build_alive_frame();
    this->write_array(frame.data(), frame.size());
    ESP_LOGI(TAG, "Sent alive frame");
  }

  // Keypad is portable core (no ESPHome headers -- see keypad.h), so it
  // reaches the UART and the logger only through these two injected sinks.
  // read_only_ is forwarded here rather than duplicated: one flag, one
  // meaning, checked in exactly one place (Keypad::maybe_transmit_).
  this->keypad_.set_frame_sink([this](const uint8_t *data, size_t len) { this->write_array(data, len); });
  this->keypad_.set_log_sink({
      [](const std::string &msg) { ESP_LOGI(TAG, "%s", msg.c_str()); },
      [](const std::string &msg) { ESP_LOGW(TAG, "%s", msg.c_str()); },
      [](const std::string &msg) { ESP_LOGE(TAG, "%s", msg.c_str()); },
  });
  this->keypad_.set_read_only(this->read_only_);

  // Runner is portable core too (sequence.h), same LogSink shape as Keypad
  // above -- reused rather than redeclared, see Runner::LogSink.
  this->runner_.set_log_sink({
      [](const std::string &msg) { ESP_LOGI(TAG, "%s", msg.c_str()); },
      [](const std::string &msg) { ESP_LOGW(TAG, "%s", msg.c_str()); },
      [](const std::string &msg) { ESP_LOGE(TAG, "%s", msg.c_str()); },
  });
  this->runner_.set_on_sequence_failed([this](const std::string &name) {
    for (auto *trig : this->sequence_failed_triggers_) {
      trig->trigger(name);
    }
  });

  // frame_logger_ is portable core too (frame_logger.h). ESP_LOGD, not
  // ESP_LOGI/W/E like keypad_/runner_ above -- these lines are deliberately
  // chatty (a raw frame arrives ~3.3 times a second, PLAN.md §8 stage 15)
  // and were ESP_LOGD before this instrumentation moved out of here; wiring
  // Keypad::LogSink::debug to anything louder would promote debug spam to a
  // severity mhrv.yaml's default log level actually shows.
  Keypad::LogSink frame_logger_sink;
  frame_logger_sink.debug = [](const std::string &msg) { ESP_LOGD(TAG, "%s", msg.c_str()); };
  this->frame_logger_.set_log_sink(frame_logger_sink);

  this->fetch_diagnostics_.set_on_success([this] { this->stamp_diagnostics_updated_(); });

  // Stage 6: read_settings_ (the button's own instance) and write_setting_
  // (shared by write_switch()/write_number(), whose own read-back step is a
  // SEPARATE ReadSettings member -- see sequence.h) both publish through the
  // exact same two sinks, so a value read by either path reaches the same
  // entities the same way.
  auto publish_switch = [this](SwitchKey key, bool value) { this->publish_switch_(key, value); };
  auto publish_number = [this](NumberKey key, int value) { this->publish_number_(key, value); };

  this->read_settings_.set_on_switch(publish_switch);
  this->read_settings_.set_on_number(publish_number);

  this->write_setting_.set_on_switch(publish_switch);
  this->write_setting_.set_on_number(publish_number);

  // Stage 7: sync_clock_ gets wall-clock time through this sink rather than
  // touching time::RealTimeClock itself -- sequence.h/seq_sync_clock.cpp are
  // portable core (README "Portable core"). Must compile with USE_TIME
  // undefined (the ESP32-IDF example declares no `time:` platform at all):
  // the #else branch below is what makes that true, returning "unavailable"
  // rather than failing to build.
  this->sync_clock_.set_time_source([this](int &dow_display, int &hour, int &minute) -> bool {
#ifdef USE_TIME
    if (this->time_ == nullptr) {
      return false;  // time_id left out of the hub config -- optional, see __init__.py
    }
    // Not const: ESPTime::strftime() is a non-const member (see
    // stamp_diagnostics_updated_() above) -- this callback never calls it,
    // but `now` is left non-const anyway so a future edit that does add one
    // doesn't have to remember the rule.
    auto now = this->time_->now();
    if (!now.is_valid()) {
      // Not synced yet -- see SyncClock::TimeSource's own comment: this
      // counts as "unavailable" the same as no time source at all, because
      // syncing the unit against an unsynced clock would write a WRONG time.
      return false;
    }
    dow_display = parser::dow_to_display(now.day_of_week);
    hour = now.hour;
    minute = now.minute;
    return true;
#else
    (void) dow_display;
    (void) hour;
    (void) minute;
    return false;  // no `time:` platform in this build at all
#endif
  });

  // Finding 2 (Opus review of this stage): the alternation-aware, sticky
  // purging() this sequence's CHECK_CURRENT needs to answer "is the unit
  // currently purging" reliably -- the exact same tracker every other
  // status-derived entity already reads from, not a second, separate purge
  // decode. status_ outlives set_airflow_mode_ (both are hub members,
  // declared above in this class -- sequence.h), so this pointer stays
  // valid for the hub's whole lifetime.
  this->set_airflow_mode_.set_status(&this->status_);

  // Stage 7's other deliverable, and the last sequence in this component by
  // design (PLAN.md §8): the one irreversible operation. reset_filter_'s
  // OWN dedicated FetchDiagnostics child (sequence.h's diagnostics_scan_,
  // NOT fetch_diagnostics_ above -- see that member's own comment for why)
  // needs the same on_success stamp every other diagnostic scrape gets.
  // A genuine full diagnostic scrape happens inside this sequence's own
  // chained scan exactly as it does for the button/schedule path, so the
  // timestamp should reflect it the same way -- see
  // stamp_diagnostics_updated_()'s own comment.
  this->reset_filter_.set_on_diagnostics_success([this] { this->stamp_diagnostics_updated_(); });
  // Least invasive route to page 23's reading (see ResetFilter::
  // FilterHoursSource's own comment, sequence.h, for the full reasoning and
  // its one carried-forward limitation): reuses the SAME "last published"
  // cache publish_sensor_ already keeps for its own dedup, rather than
  // inventing a second, parallel cache just for this one check.
  this->reset_filter_.set_filter_hours_source([this]() -> std::optional<int> {
    return this->last_sensor_value_[static_cast<size_t>(SensorKey::FILTER_HOURS)];
  });
}

void VentAxiaHub::loop() {
  // Byte-at-a-time: drains whatever the UART FIFO has this tick, one frame
  // worth of state machine per byte, so nothing is buffered on our own side
  // waiting for a "full frame" read that a mid-stream glitch could stall.
  uint8_t byte;
  protocol::DisplayFrame frame;
  while (this->available() != 0 && this->read_byte(&byte)) {
    if (this->framer_.feed(byte, &frame)) {
      const uint32_t now = millis();
      this->have_frame_ = true;
      this->last_frame_at_ms_ = now;
      // Reads frame.line1/line2 directly rather than display_'s raw lane,
      // even though stage 16 gave that lane the same bytes: this call runs
      // BEFORE display_.update() below, so display_'s raw lane still holds
      // the PREVIOUS frame at this point, not the one being logged. Placed
      // first to read that way, not because update() mutates the frame: it
      // takes const refs, so `frame` is untouched either side of it.
      this->frame_logger_.log(frame, now);
      this->display_.update(frame.line1, frame.line2, now);
      // Fed every frame, not just changed ones: StatusTracker's aging clock
      // (status.h) needs a regular heartbeat to notice time passing, and a
      // repeated identical frame is exactly the case where "nothing changed,
      // still true" has to be confirmed rather than silently skipped. Raw
      // lane: every status:: decoder is a fixed-offset byte reader, never a
      // presentation consumer (DISPLAY-REVIEW.md §5).
      this->status_.update(this->display_.raw_line1(), this->display_.raw_line2(),
                            this->display_.screen_kind() == screens::ScreenKind::STATUS, now);
      this->publish_status_();
    }
  }

  // Checked every tick, independently of whether a frame arrived this tick:
  // a dead link produces no frames at all, so link_up must be reevaluated on
  // the clock, not on an event that has stopped happening.
  const uint32_t now = millis();
  this->publish_link_up_(now);

  // Drives every timing decision in Keypad -- see keypad.h's class comment.
  // Also checked every tick regardless of whether a frame arrived, same
  // reasoning as link_up above: a held key needs retransmitting on the
  // clock, not on an event tied to the MVHR's own ~300ms display refresh.
  this->keypad_.loop(now);

  // Runner sits alongside Keypad, not on top of it: it directs the keypad
  // (via HoldUntil/Tap et al.) rather than owning its timing loop, so both
  // get pumped independently every tick with the same now_ms. Fed link_up_
  // fresh each tick too -- see Runner::request()'s refusal, PLAN.md §7.
  this->runner_.set_link_up(this->link_up_);
  this->runner_.loop(now);

  // Stage 5: a sequence can be between keypresses (settling, waiting on a
  // predicate) and still be busy as far as a dashboard is concerned -- see
  // entities.h's BinaryKey::BUSY comment.
  this->publish_binary_(BinaryKey::BUSY, this->keypad_.busy() || this->runner_.busy());
}

void VentAxiaHub::dump_config() {
  ESP_LOGCONFIG(TAG, "Vent-Axia:");
  ESP_LOGCONFIG(TAG, "  Read-only: %s", YESNO(this->read_only_));
  ESP_LOGCONFIG(TAG, "  Frames received: %u", this->framer_.frames_received());
  ESP_LOGCONFIG(TAG, "  Frames dropped (bad CRC): %u", this->framer_.frames_dropped());
  // The timing constants in force (PLAN.md §2's table -- every one of these
  // was paid for with debugging on the live unit) and the diagnostic PLAN.md
  // risk 1 calls for: presses that emitted fewer than 2 frames. Reported
  // here so a soak test can catch a creeping problem without instrumenting
  // anything -- see keypad.h's under_emitting_presses() comment for what to
  // do if this is ever nonzero.
  ESP_LOGCONFIG(TAG, "  Key TX interval: %ums", this->keypad_.tx_interval_ms());
  ESP_LOGCONFIG(TAG, "  Key tap duration: %ums", this->tap_duration_ms_);
  ESP_LOGCONFIG(TAG, "  Key gap: %ums", this->keypad_.key_gap_ms());
  ESP_LOGCONFIG(TAG, "  Key watchdog: %ums", this->keypad_.key_watchdog_ms());
  ESP_LOGCONFIG(TAG, "  Under-emitting key presses: %u", this->keypad_.under_emitting_presses());
  ESP_LOGCONFIG(TAG, "  Sequence running: %s", this->runner_.busy() ? this->runner_.running_name() : "none");
  this->check_uart_settings(9600, 1, uart::UART_CONFIG_PARITY_NONE, 8);
}

void VentAxiaHub::tap_key(protocol::KeyMask mask, uint32_t duration_ms) {
  // Runner::tap() is the interlock choke point now (PLAN.md §7) -- its
  // return value is intentionally ignored here: a refusal is already logged
  // there (runner_'s LogSink, wired in setup()), and there is nothing more
  // for a fire-and-forget caller like a button press to do about it.
  this->runner_.tap(mask, duration_ms);
}

void VentAxiaHub::hold_key(protocol::KeyMask mask) { this->runner_.press(mask); }

void VentAxiaHub::write_switch(SwitchKey key, bool state) {
  // The mapping itself lives in the portable core (setting_for(), sequence.h)
  // so a forgotten key is caught by the host suite's -Werror rather than
  // only warned about in a firmware build log -- see its own comment.
  const std::optional<SettingId> id = setting_for(key);
  if (!id.has_value()) {
    ESP_LOGE(TAG, "write_switch: no WriteSetting mapping for this SwitchKey");
    return;
  }
  this->start_write_(*id, state ? 1 : 0);
}

void VentAxiaHub::write_number(NumberKey key, int value) {
  const std::optional<SettingId> id = setting_for(key);
  if (!id.has_value()) {
    ESP_LOGE(TAG, "write_number: no WriteSetting mapping for this NumberKey");
    return;
  }
  this->start_write_(*id, value);
}

void VentAxiaHub::write_select(SelectKey key, size_t index) {
  // index is the AirflowTarget ordinal directly -- select.py's
  // AIRFLOW_MODE_OPTIONS list is deliberately ordered to match, so no lookup
  // table is needed, only the bounds check airflow_target_for() does.
  const std::optional<AirflowTarget> target = airflow_target_for(key, index);
  if (!target.has_value()) {
    ESP_LOGE(TAG, "write_select: no SetAirflowMode mapping for this SelectKey/index");
    return;
  }
  this->set_airflow_mode_.configure(*target);
  this->runner_.request(this->set_airflow_mode_);
}

// Each publish_ helper compiles to a no-op when its platform is absent from
// the build, so the decode path calls them unconditionally and stays free of
// preprocessor noise. The (void) casts keep unused parameters quiet.
void VentAxiaHub::publish_text_(TextKey key, const std::string &value) {
#ifndef USE_TEXT_SENSOR
  (void) key;
  (void) value;
#else
  text_sensor::TextSensor *sensor = this->text_sensors_[static_cast<size_t>(key)];
  if (sensor != nullptr) {
    sensor->publish_state(value);
  }
#endif
}

void VentAxiaHub::publish_sensor_(SensorKey key, std::optional<int> value) {
#ifndef USE_SENSOR
  (void) key;
  (void) value;
#else
  const size_t idx = static_cast<size_t>(key);
  sensor::Sensor *sens = this->sensors_[idx];

  if (!value.has_value()) {
    // Two different situations, and they need different answers. If nothing
    // was ever published, the tracker simply doesn't know yet: publish
    // nothing, so the entity stays unknown rather than becoming a guess.
    //
    // But if a value *was* published and has now gone away, the last one is
    // stale and actively misleading -- boost_time_remaining is the case that
    // matters: when a 30-minute boost ends, the countdown disappears from
    // line2 and Home Assistant would otherwise go on showing "30 min"
    // indefinitely. There is no way to un-publish, so publish NaN, which HA
    // renders as unknown.
    if (this->last_sensor_value_[idx].has_value()) {
      this->last_sensor_value_[idx].reset();
      if (sens != nullptr) {
        sens->publish_state(NAN);
      }
    }
    return;
  }

  if (this->last_sensor_value_[idx].has_value() && *this->last_sensor_value_[idx] == *value) {
    return;  // unchanged since the last publish -- don't spam the API at ~3 Hz
  }
  this->last_sensor_value_[idx] = value;
  if (sens != nullptr) {
    sens->publish_state(static_cast<float>(*value));
  }
#endif
}

void VentAxiaHub::publish_binary_(BinaryKey key, std::optional<bool> value) {
#ifndef USE_BINARY_SENSOR
  (void) key;
  (void) value;
#else
  if (!value.has_value()) {
    return;  // tracker doesn't know yet -- leave the entity unpublished, not a guess
  }
  const size_t idx = static_cast<size_t>(key);
  if (this->last_binary_value_[idx].has_value() && *this->last_binary_value_[idx] == *value) {
    return;  // unchanged since the last publish -- don't spam the API at ~3 Hz
  }
  this->last_binary_value_[idx] = value;
  binary_sensor::BinarySensor *sens = this->binary_sensors_[idx];
  if (sens != nullptr) {
    sens->publish_state(*value);
  }
#endif
}

void VentAxiaHub::publish_switch_(SwitchKey key, bool value) {
#ifndef USE_SWITCH
  (void) key;
  (void) value;
#else
  const size_t idx = static_cast<size_t>(key);
  if (this->last_switch_value_[idx].has_value() && *this->last_switch_value_[idx] == value) {
    return;  // unchanged since the last read -- nothing to republish
  }
  this->last_switch_value_[idx] = value;
  switch_::Switch *sw = this->switches_[idx];
  if (sw != nullptr) {
    sw->publish_state(value);
  }
#endif
}

void VentAxiaHub::publish_number_(NumberKey key, int value) {
#ifndef USE_NUMBER
  (void) key;
  (void) value;
#else
  const size_t idx = static_cast<size_t>(key);
  if (this->last_number_value_[idx].has_value() && *this->last_number_value_[idx] == value) {
    return;  // unchanged since the last read -- nothing to republish
  }
  this->last_number_value_[idx] = value;
  number::Number *num = this->numbers_[idx];
  if (num != nullptr) {
    num->publish_state(static_cast<float>(value));
  }
#endif
}

void VentAxiaHub::publish_status_() {
  this->publish_binary_(BinaryKey::SUMMER_BYPASS, this->status_.summer_bypass());
  this->publish_binary_(BinaryKey::BOOSTING, this->status_.boosting());
  this->publish_binary_(BinaryKey::PURGING, this->status_.purging());
  this->publish_binary_(BinaryKey::DEFROST_ACTIVE, this->status_.defrost_active());
  this->publish_binary_(BinaryKey::DRYOUT_ACTIVE, this->status_.dryout_active());
  this->publish_binary_(BinaryKey::FILTER_CHANGE_DUE, this->status_.filter_change_due());
  this->publish_binary_(BinaryKey::HUMIDITY_BOOST, this->status_.humidity_boost());

  this->publish_sensor_(SensorKey::AIRFLOW, this->status_.airflow_percent());
  this->publish_sensor_(SensorKey::BOOST_TIME_REMAINING, this->status_.boost_time_remaining());

  this->publish_airflow_mode_();
}

void VentAxiaHub::publish_select_(SelectKey key, const std::string &value) {
#ifndef USE_SELECT
  (void) key;
  (void) value;
#else
  const size_t idx = static_cast<size_t>(key);
  if (this->last_select_value_[idx].has_value() && *this->last_select_value_[idx] == value) {
    return;  // unchanged since the last publish -- don't spam the API at ~3 Hz
  }
  this->last_select_value_[idx] = value;
  select::Select *sel = this->selects_[idx];
  if (sel != nullptr) {
    sel->publish_state(value);
  }
#endif
}

void VentAxiaHub::publish_airflow_mode_() {
#ifndef USE_SELECT
  return;
#else
  // Suppressed ENTIRELY -- no publish, and no airflow_mode_.update() call
  // either -- while SetAirflowMode is the running root sequence. See this
  // function's own comment (vent_axia.h) for why: normalising deliberately
  // passes through boost states nobody chose, and letting the tracker's
  // latch absorb that transient noise would corrupt the very evidence it
  // exists to remember for whatever state the run actually settles on.
  // Pointer identity (Runner::is_running()), not a name comparison: two
  // sequences sharing a name() could otherwise defeat a string check, and
  // this way there is no risk of drifting out of sync with a duplicated
  // literal either.
  if (this->runner_.is_running(&this->set_airflow_mode_)) {
    return;
  }

  const std::optional<status::AirflowMode> mode = this->airflow_mode_.update(this->status_);
  if (!mode.has_value()) {
    return;
  }
  this->publish_select_(SelectKey::AIRFLOW_MODE, status::to_string(*mode));
#endif
}

void VentAxiaHub::publish_diagnostic_page_(uint8_t page, const std::string &line2) {
  // `line2` here is the RAW lane (the caller passes display_.raw_line2()) --
  // transcoded exactly ONCE, for the two presentation uses immediately
  // below, so the cost is one to_utf8() call per page-change rather than
  // two. diagnostics::decode_page() near the bottom of this function keeps
  // reading the RAW bytes: every diagnostic page ever observed on this unit
  // is pure ASCII, so today the two representations are byte-identical, but
  // the decoder must stay correct if a page ever isn't.
  const std::string text_line2 = to_utf8(line2);

  // Raw escape hatch and trigger fire for every page seen, decoded by the
  // table or not -- this is what lets a page nobody has taught
  // diagnostics.cpp to understand yet (or a nonexistent page 28, or
  // anything else) stay visible from YAML without a component change. See
  // diagnostics.h's comment on why this is the hub's job and not
  // decode_page()'s. Both are presentation uses (a text sensor state and a
  // YAML automation trigger's std::string argument), hence text_line2.
  this->publish_text_(TextKey::RAW_DIAGNOSTIC_PAGE, diagnostics::format_raw_page(page, text_line2));
  for (auto *trig : this->diagnostic_page_triggers_) {
    trig->trigger(page, text_line2);
  }

  // Adapts diagnostics::Sink's plain-value callbacks onto this hub's own
  // optional-based publish_sensor_/publish_binary_/publish_text_ -- the
  // same dedup-by-last-value and platform-absent-is-a-no-op behaviour those
  // already have applies here for free, so diagnostics.cpp does not need to
  // reimplement any of it.
  diagnostics::Sink sink;
  sink.publish_sensor = [this](SensorKey key, int value) { this->publish_sensor_(key, value); };
  sink.publish_binary = [this](BinaryKey key, bool value) { this->publish_binary_(key, value); };
  sink.publish_text = [this](TextKey key, const std::string &value) { this->publish_text_(key, value); };
  sink.report_filter_change_due = [this](bool due) { this->reconcile_filter_change_due_(due); };
  diagnostics::decode_page(page, line2, sink);  // RAW -- decode_page() is a byte-offset decoder
}

void VentAxiaHub::reconcile_filter_change_due_(bool due_from_page23) {
  // Two independent sources for the same fact: status_ derives
  // filter_change_due live off the status line's "Check Filter" message,
  // refreshed roughly 3x/second; diagnostics only sees page 23 once a day
  // (the scheduled fetch, PLAN.md §6) or whenever a human happens to browse
  // the menu by hand. "Live wins by recency" (PLAN.md risk 7): the
  // diagnostic-derived value is only published when the live tracker has no
  // opinion at all yet -- e.g. straight after boot, before the first
  // status-screen frame -- otherwise this purely cross-checks and logs a
  // disagreement, because a disagreement means one of the two decodes is
  // wrong, not that the filter state is ambiguous.
  const std::optional<bool> live = this->status_.filter_change_due();
  if (live.has_value()) {
    if (*live != due_from_page23) {
      ESP_LOGW(TAG,
               "filter_change_due disagreement: live status line says %s, diagnostic page 23 says %s -- one of "
               "the two decodes is wrong",
               YESNO(*live), YESNO(due_from_page23));
    }
    return;
  }
  this->publish_binary_(BinaryKey::FILTER_CHANGE_DUE, due_from_page23);
}

void VentAxiaHub::publish_link_up_(uint32_t now_ms) {
  // False before the first frame (have_frame_ starts false) and false again
  // once LINK_TIMEOUT_MS has passed without a new one -- replaces the old
  // setup's "infer liveness from line2 having stopped republishing" (PLAN.md
  // §7).
  const bool up = this->have_frame_ && (now_ms - this->last_frame_at_ms_) < LINK_TIMEOUT_MS;
  // Cached for loop() to hand to runner_.set_link_up() -- Runner::request()
  // refuses to start anything while the link is down (PLAN.md §7), so it
  // needs the same answer this publishes, not a second computation of it.
  this->link_up_ = up;
  this->publish_binary_(BinaryKey::LINK_UP, up);
}

void VentAxiaHub::stamp_diagnostics_updated_() {
#ifdef USE_TIME
  if (this->time_ == nullptr) {
    return;  // time_id left out of the hub config -- optional, see __init__.py
  }
  // Not const: ESPTime::strftime() is a non-const member.
  auto now = this->time_->now();
  if (!now.is_valid()) {
    // Not logged as a warning: this is routine for the first run or two
    // after boot, before HA's `time: homeassistant` platform has completed
    // its first sync -- a scheduled 04:30 fetch on a long-running device
    // will essentially never see this.
    return;
  }
  this->publish_text_(TextKey::DIAGNOSTICS_UPDATED, now.strftime("%Y-%m-%d %H:%M:%S"));
#endif
}

}  // namespace vent_axia
}  // namespace esphome
