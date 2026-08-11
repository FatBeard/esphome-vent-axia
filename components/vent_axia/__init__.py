from esphome import automation
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import time, uart
from esphome.const import CONF_DURATION, CONF_ID, CONF_KEY, CONF_TIME_ID, CONF_TRIGGER_ID

CODEOWNERS = ["@FatBeard"]
DEPENDENCIES = ["uart"]

vent_axia_ns = cg.esphome_ns.namespace("vent_axia")
VentAxiaHub = vent_axia_ns.class_("VentAxiaHub", cg.Component, uart.UARTDevice)
# (page, line2) -- see diagnostics.h and vent_axia.h's DiagnosticPageTrigger.
# Fires for every diagnostic page the display passes through, decoded by the
# table (diagnostics.cpp) or not, so a page nobody has taught the component
# to decode yet can still be acted on from YAML without a component change.
DiagnosticPageTrigger = vent_axia_ns.class_(
    "DiagnosticPageTrigger", automation.Trigger.template(cg.uint8, cg.std_string)
)
# (name) -- see sequence.h's Runner::FailureSink and vent_axia.h's
# SequenceFailedTrigger. Fires once a root sequence (PLAN.md §2) finishes as
# FAILED, naming which one -- the only way YAML learns that a scheduled
# fetch/sync/write did not complete.
SequenceFailedTrigger = vent_axia_ns.class_(
    "SequenceFailedTrigger", automation.Trigger.template(cg.std_string)
)
# vent_axia.tap_key / vent_axia.hold_key / vent_axia.release_keys /
# vent_axia.fetch_diagnostics / vent_axia.read_settings / vent_axia.sync_clock
# (PLAN.md §5). See vent_axia.h's class comments for why the first three are
# plain constructor args rather than TEMPLATABLE_VALUE;
# fetch_diagnostics/read_settings/sync_clock take no arguments at all.
TapKeyAction = vent_axia_ns.class_("TapKeyAction", automation.Action)
HoldKeyAction = vent_axia_ns.class_("HoldKeyAction", automation.Action)
ReleaseKeysAction = vent_axia_ns.class_("ReleaseKeysAction", automation.Action)
FetchDiagnosticsAction = vent_axia_ns.class_("FetchDiagnosticsAction", automation.Action)
ReadSettingsAction = vent_axia_ns.class_("ReadSettingsAction", automation.Action)
SyncClockAction = vent_axia_ns.class_("SyncClockAction", automation.Action)

CONF_VENT_AXIA_ID = "vent_axia_id"
CONF_READ_ONLY = "read_only"
CONF_ON_DIAGNOSTIC_PAGE = "on_diagnostic_page"
CONF_ON_SEQUENCE_FAILED = "on_sequence_failed"
CONF_TX_INTERVAL = "tx_interval"
CONF_TAP_DURATION = "tap_duration"
CONF_KEY_GAP = "key_gap"
CONF_KEY_WATCHDOG = "key_watchdog"

# Bit values match protocol::Key (protocol.h) exactly, and combinations OR
# together the same way -- Up+Down for a filter reset, Up+Main for
# diagnostics entry (PLAN.md §2). Kept as plain ints rather than exposed to
# codegen as an enum: the only thing Python needs to do with a key name is
# turn it into a uint8_t mask for a C++ setter, and duplicating protocol::Key
# on the Python side as a cg enum would be strictly more machinery for the
# same information.
KEY_MASKS = {
    "up": 0x02,
    "down": 0x01,
    "set": 0x04,
    "main": 0x08,
}


def validate_key_gap(value):
    value = cv.positive_time_period_milliseconds(value)
    if value.total_milliseconds < 400:
        # Not just documentation -- PLAN.md §2's table is explicit that this
        # was measured on the real unit: at 250ms roughly one press in ten
        # was dropped, and a dropped Set fails to open an editor, so the Up
        # presses that follow it walk *back up* the menu instead of
        # adjusting a value. At 50ms the unit sees the release but doesn't
        # register the next press at all. Catching this at compile time is
        # the same reasoning as the baud-rate check below.
        raise cv.Invalid(
            f"key_gap below 400ms drops keypresses on the real unit (got {value.total_milliseconds}ms) "
            "-- see PLAN.md §2's Keypad table for why this is enforced, not just documented"
        )
    return value


def validate_key_mask(value):
    """A single key name, or a list of them for a combination (Up+Down =
    filter reset, Up+Main = diagnostics entry). Returns the OR'd mask."""
    names = cv.ensure_list(cv.one_of(*KEY_MASKS, lower=True))(value)
    mask = 0
    for name in names:
        mask |= KEY_MASKS[name]
    return mask


CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(VentAxiaHub),
            # Mutes the keypad entirely: the component still receives, decodes and
            # publishes, but transmits nothing. Lets the production firmware be
            # soak-tested against a live unit with no risk of driving it.
            cv.Optional(CONF_READ_ONLY, default=False): cv.boolean,
            # Optional (PLAN.md §5): without it, FetchDiagnostics still runs and
            # publishes every diagnostic entity exactly as before, it just never
            # stamps diagnostics_updated -- see vent_axia.h's
            # stamp_diagnostics_updated_(). Not required at all until a sequence
            # that wants wall-clock time exists.
            cv.Optional(CONF_TIME_ID): cv.use_id(time.RealTimeClock),
            # Timing constants -- PLAN.md §2's table. Defaults are the values
            # proven on the physical unit; overridable per PLAN.md §5, but see
            # validate_key_gap's floor on key_gap specifically.
            cv.Optional(CONF_TX_INTERVAL, default="20ms"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_TAP_DURATION, default="50ms"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_KEY_GAP, default="400ms"): validate_key_gap,
            cv.Optional(CONF_KEY_WATCHDOG, default="30s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_ON_DIAGNOSTIC_PAGE): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(DiagnosticPageTrigger),
                }
            ),
            cv.Optional(CONF_ON_SEQUENCE_FAILED): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(SequenceFailedTrigger),
                }
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA)
)

# The wired-remote link is 9600 8N1. The upstream component only warned about a
# mismatch at runtime; catching it at compile time is free.
FINAL_VALIDATE_SCHEMA = uart.final_validate_device_schema(
    "vent_axia",
    baud_rate=9600,
    require_tx=True,
    require_rx=True,
    parity="NONE",
    stop_bits=1,
    data_bits=8,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    cg.add(var.set_read_only(config[CONF_READ_ONLY]))
    cg.add(var.set_tx_interval_ms(config[CONF_TX_INTERVAL].total_milliseconds))
    cg.add(var.set_tap_duration_ms(config[CONF_TAP_DURATION].total_milliseconds))
    cg.add(var.set_key_gap_ms(config[CONF_KEY_GAP].total_milliseconds))
    cg.add(var.set_key_watchdog_ms(config[CONF_KEY_WATCHDOG].total_milliseconds))

    if CONF_TIME_ID in config:
        rtc = await cg.get_variable(config[CONF_TIME_ID])
        cg.add(var.set_time_id(rtc))

    for conf in config.get(CONF_ON_DIAGNOSTIC_PAGE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID])
        cg.add(var.register_diagnostic_page_trigger(trigger))
        await automation.build_automation(trigger, [(cg.uint8, "page"), (cg.std_string, "line2")], conf)

    for conf in config.get(CONF_ON_SEQUENCE_FAILED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID])
        cg.add(var.register_sequence_failed_trigger(trigger))
        await automation.build_automation(trigger, [(cg.std_string, "name")], conf)


VENT_AXIA_TAP_KEY_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_VENT_AXIA_ID): cv.use_id(VentAxiaHub),
        cv.Required(CONF_KEY): validate_key_mask,
        # Left unset (rather than defaulted here) so tap_key() can fall back
        # to the hub's own configured tap_duration -- see TapKeyAction::play.
        cv.Optional(CONF_DURATION): cv.positive_time_period_milliseconds,
    }
)

VENT_AXIA_HOLD_KEY_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_VENT_AXIA_ID): cv.use_id(VentAxiaHub),
        cv.Required(CONF_KEY): validate_key_mask,
    }
)

VENT_AXIA_RELEASE_KEYS_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_VENT_AXIA_ID): cv.use_id(VentAxiaHub),
    }
)

VENT_AXIA_FETCH_DIAGNOSTICS_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_VENT_AXIA_ID): cv.use_id(VentAxiaHub),
    }
)

VENT_AXIA_READ_SETTINGS_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_VENT_AXIA_ID): cv.use_id(VentAxiaHub),
    }
)

VENT_AXIA_SYNC_CLOCK_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_VENT_AXIA_ID): cv.use_id(VentAxiaHub),
    }
)


# All of them are synchronous=True: play() only ever queues a request -- with
# the keypad, or with the Runner (fetch_diagnostics/read_settings/sync_clock)
# -- and returns. The
# tap/hold/sequence itself plays out over many future loop() ticks, but the
# automation's *next* action is never deferred waiting for that, so there is
# nothing here for play_next_() to defer.
@automation.register_action("vent_axia.tap_key", TapKeyAction, VENT_AXIA_TAP_KEY_SCHEMA, synchronous=True)
async def vent_axia_tap_key_to_code(config, action_id, template_arg, args):
    hub = await cg.get_variable(config[CONF_VENT_AXIA_ID])
    duration_ms = config[CONF_DURATION].total_milliseconds if CONF_DURATION in config else 0
    return cg.new_Pvariable(action_id, template_arg, hub, config[CONF_KEY], duration_ms)


@automation.register_action("vent_axia.hold_key", HoldKeyAction, VENT_AXIA_HOLD_KEY_SCHEMA, synchronous=True)
async def vent_axia_hold_key_to_code(config, action_id, template_arg, args):
    hub = await cg.get_variable(config[CONF_VENT_AXIA_ID])
    return cg.new_Pvariable(action_id, template_arg, hub, config[CONF_KEY])


@automation.register_action(
    "vent_axia.release_keys", ReleaseKeysAction, VENT_AXIA_RELEASE_KEYS_SCHEMA, synchronous=True
)
async def vent_axia_release_keys_to_code(config, action_id, template_arg, args):
    hub = await cg.get_variable(config[CONF_VENT_AXIA_ID])
    return cg.new_Pvariable(action_id, template_arg, hub)


@automation.register_action(
    "vent_axia.fetch_diagnostics", FetchDiagnosticsAction, VENT_AXIA_FETCH_DIAGNOSTICS_SCHEMA, synchronous=True
)
async def vent_axia_fetch_diagnostics_to_code(config, action_id, template_arg, args):
    hub = await cg.get_variable(config[CONF_VENT_AXIA_ID])
    return cg.new_Pvariable(action_id, template_arg, hub)


@automation.register_action(
    "vent_axia.read_settings", ReadSettingsAction, VENT_AXIA_READ_SETTINGS_SCHEMA, synchronous=True
)
async def vent_axia_read_settings_to_code(config, action_id, template_arg, args):
    hub = await cg.get_variable(config[CONF_VENT_AXIA_ID])
    return cg.new_Pvariable(action_id, template_arg, hub)


@automation.register_action("vent_axia.sync_clock", SyncClockAction, VENT_AXIA_SYNC_CLOCK_SCHEMA, synchronous=True)
async def vent_axia_sync_clock_to_code(config, action_id, template_arg, args):
    hub = await cg.get_variable(config[CONF_VENT_AXIA_ID])
    return cg.new_Pvariable(action_id, template_arg, hub)
