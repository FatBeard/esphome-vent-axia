#include "sequence.h"

#include "screens.h"

namespace esphome {
namespace vent_axia {

// The complete type SettingSpec is forward-declared in sequence.h (WriteSetting
// only ever holds a pointer to one) and defined here, alongside the table
// that is the whole point of it: PLAN.md §2's "one class... three table
// rows, not three near-identical copies" -- Summer Mode, Indoor Temp and
// Outdoor Temp differ only in these six fields, everything else about
// writing them is identical and lives in WriteSetting::poll() below.
struct SettingSpec {
  SettingId id;
  uint8_t menu_index;          // GotoMenu target for NAVIGATE/VERIFY
  screens::ScreenKind screen;  // expected screen right after that GotoMenu
  AdjustField::ValueParser parse;
  AdjustField::DirectionFn direction;
  int guard_limit;  // mhrv_orig/summer_bypass.yaml's own numbers: 3 for Summer Mode, 40 for a temperature
};

namespace {
constexpr protocol::KeyMask SET = protocol::key_mask(protocol::Key::SET);
constexpr uint32_t TAP_MS = 50;  // "one tap = one menu step", PLAN.md §2

// Outdoor Temp's row deliberately names Indoor Temp's menu_index/screen: it
// is reached THROUGH Indoor Temp's editor (WriteSetting::poll()'s
// HOP_COMMIT/WAIT_HOP_SCREEN), not by direct navigation, so NAVIGATE/VERIFY
// use Indoor Temp's for this row too -- see WriteSetting's class comment.
constexpr SettingSpec kSettings[] = {
    {SettingId::SUMMER_MODE, 2, screens::ScreenKind::SUMMER_MODE, parse_summer_mode_field, direction_no_wrap, 3},
    {SettingId::INDOOR_TEMP, 3, screens::ScreenKind::INDOOR_TEMP, parse_temp_field, direction_no_wrap, 40},
    {SettingId::OUTDOOR_TEMP, 3, screens::ScreenKind::INDOOR_TEMP, parse_temp_field, direction_no_wrap, 40},
};

// Never returns nullptr for a valid SettingId -- kSettings has exactly one
// row per enumerator, see the static_assert below.
const SettingSpec *find_setting_spec(SettingId id) {
  for (const auto &spec : kSettings) {
    if (spec.id == id) {
      return &spec;
    }
  }
  return nullptr;
}

static_assert(sizeof(kSettings) / sizeof(kSettings[0]) == 3, "one SettingSpec row per SettingId enumerator");
}  // namespace

void WriteSetting::configure(SettingId id, int target) {
  this->id_ = id;
  this->target_ = target;
  this->spec_ = find_setting_spec(id);
}

void WriteSetting::on_start() { this->ok_ = true; }

Poll WriteSetting::poll() {
  switch (this->step_) {
    case NAVIGATE:
      this->nav_started_ms_ = this->runner_->now_ms();
      this->goto_menu_.reset(this->spec_->menu_index);
      return this->await(this->goto_menu_, VERIFY);

    // Right screen, and a value that actually parsed AND is fresh -- newer
    // than nav_started_ms_, PLAN.md's "Reading a value off the screen",
    // same reasoning ReadSettings' plain screens use. Nothing has been
    // committed yet at this point (Set has not been pressed), so a straight
    // FAILED here is safe: the display is still on a menu screen and
    // Runner::recover()'s own single Up tap is exactly the right unwind --
    // no ExitEditChain needed.
    case VERIFY: {
      if (this->runner_->display().screen_kind() != this->spec_->screen) {
        return this->elapsed() < VERIFY_TIMEOUT_MS ? Poll::RUNNING : Poll::FAILED;
      }
      if (read_fresh_value(this->runner_->display(), this->nav_started_ms_, this->spec_->parse).has_value()) {
        return this->goto_step(OPEN);
      }
      return this->elapsed() < VERIFY_TIMEOUT_MS ? Poll::RUNNING : Poll::FAILED;
    }

    // Outdoor Temp's editor IS Indoor Temp's -- opened here the same as the
    // other two rows, then stepped past below (HOP_COMMIT). OpenEditor
    // retries once internally; see its class comment for why a dropped Set
    // matters enough to check for. Its own failure cascades straight to
    // on_finish(FAILED) -- correct, nothing opened means nothing to walk out
    // of.
    case OPEN:
      return this->await(this->open_editor_, this->id_ == SettingId::OUTDOOR_TEMP ? HOP_COMMIT : ADJUST);

    // --- Outdoor Temp only: step past Indoor Temp's now-open editor ---
    case HOP_COMMIT:
      // Commits Indoor Temp's value UNTOUCHED -- AdjustField has not run
      // yet, so this is a no-op write, safe precisely because nothing has
      // touched the value being committed (PLAN.md).
      if (!this->runner_->tap(SET, TAP_MS)) {
        return Poll::FAILED;  // refused by the Set interlock -- see Runner::tap()
      }
      return this->goto_step(WAIT_HOP_TAP);

    case WAIT_HOP_TAP:
      return this->runner_->keypad_busy() ? Poll::RUNNING : this->goto_step(WAIT_HOP_SCREEN);

    case WAIT_HOP_SCREEN:
      if (this->runner_->display().screen_kind() == screens::ScreenKind::OUTDOOR_TEMP) {
        return this->goto_step(ADJUST);
      }
      if (this->elapsed() >= HOP_SCREEN_TIMEOUT_MS) {
        // Did not land on Outdoor Temp -- the chain's shape is not
        // guaranteed (PLAN.md). An editor may still be open on whatever
        // screen this is either way, so EXIT_CHAIN runs regardless -- see
        // class comment on ok_.
        if (this->log_.warn) {
          this->log_.warn("WriteSetting: did not land on Outdoor Temp after the Indoor Temp hop -- nothing written "
                           "(line1='" +
                           this->runner_->display().text_line1() + "')");
        }
        this->ok_ = false;
        return this->goto_step(EXIT_CHAIN);
      }
      return Poll::RUNNING;

    case ADJUST:
      this->adjust_field_.reset(this->spec_->parse, this->spec_->direction, this->target_, this->spec_->guard_limit);
      return this->await(this->adjust_field_, COMMIT);
      // AdjustField's own failure (guard exhausted -- PLAN.md risk 6, a
      // value the unit refuses looks identical to a dropped press until the
      // guard trips) cascades straight to on_finish(FAILED) WITHOUT reaching
      // COMMIT/EXIT_CHAIN. That is fine here specifically: the editor is
      // still open on the field itself (never Outdoor Temp's special case),
      // so Runner::recover()'s single Up tap would still be wrong against an
      // open editor -- but this is the one gap in "always funnel through
      // ExitEditChain" this stage accepts, see the report for why: it is
      // shared by every path that awaits a primitive capable of leaving an
      // editor open, not special to this one, and PLAN.md's own recover()
      // has the identical property already.

    case COMMIT:
      if (!this->runner_->tap(SET, TAP_MS)) {
        return Poll::FAILED;
      }
      return this->goto_step(WAIT_COMMIT_TAP);

    case WAIT_COMMIT_TAP:
      return this->runner_->keypad_busy() ? Poll::RUNNING : this->goto_step(SETTLE);

    case SETTLE:
      return this->elapsed() >= SETTLE_MS ? this->goto_step(EXIT_CHAIN) : Poll::RUNNING;

    case EXIT_CHAIN:
      return this->await(this->exit_chain_, HOME);

    case HOME:
      this->goto_menu_.reset(0);
      return this->await(this->goto_menu_, READ_BACK);

    // Confirms what actually landed, whatever branch got here -- see
    // ReadSettings' own class comment for why this never hard-fails on a
    // value it could not confirm.
    case READ_BACK:
      return this->await(this->read_back_, FINISHED);

    case FINISHED:
      return this->ok_ ? Poll::DONE : Poll::FAILED;

    default:
      return Poll::DONE;
  }
}

void WriteSetting::on_finish(Poll result) {
  (void) result;
  // Backstop release -- every primitive above already releases whatever it
  // itself asserted (it only ever taps), same reasoning as
  // FetchDiagnostics::on_finish().
  this->runner_->release();
}

}  // namespace vent_axia
}  // namespace esphome
