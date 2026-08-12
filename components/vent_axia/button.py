import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button
from esphome.const import ENTITY_CATEGORY_CONFIG

from . import CONF_VENT_AXIA_ID, KEY_MASKS, VentAxiaHub, vent_axia_ns

DEPENDENCIES = ["vent_axia"]

KeypadButton = vent_axia_ns.class_("KeypadButton", button.Button)
FetchDiagnosticsButton = vent_axia_ns.class_("FetchDiagnosticsButton", button.Button)
ReadSettingsButton = vent_axia_ns.class_("ReadSettingsButton", button.Button)
SyncClockButton = vent_axia_ns.class_("SyncClockButton", button.Button)
ResetFilterButton = vent_axia_ns.class_("ResetFilterButton", button.Button)

# The manual key buttons (PLAN.md §6): key_up/key_down/key_set/key_main, each
# a momentary tap_duration tap of the matching bit in KEY_MASKS (__init__.py)
# arbitrated through the hub the same as everything else that presses a key.
#
# One dict entry per button, same shape as every other platform's
# enum-keyed pattern (sensor.py, binary_sensor.py, text_sensor.py) -- but
# there is no entities.h enum here and no hub-side std::array to index into,
# because a button is not a data sink the hub publishes through: it is an
# action, and the one piece of per-instance data it needs (which mask to
# tap) is set directly on the KeypadButton instance below rather than looked
# up by key at publish time.
#
# No entity_category: these are the escape-hatch controls a human presses
# directly (CLAUDE.md's device invariants), so they belong in HA's top-level
# Controls section next to fetch_diagnostics, not filed under Configuration
# where they're easy to miss.
BUTTONS = {
    f"key_{name}": button.button_schema(
        KeypadButton,
        icon="mdi:remote",
    )
    for name in KEY_MASKS
}

# Sequence-triggering buttons (PLAN.md §3/§6): each one starts a Runner root
# sequence (sequence.h) rather than tapping a raw key, so pressing it while
# another sequence is already running is refused (and logged), not queued.
# Stage 5 added fetch_diagnostics; stage 6 adds read_settings the same way;
# stage 7 adds sync_clock and reset_filter, the last of the four -- none of
# them need any per-instance data either, see to_code() below.
SEQUENCE_BUTTONS = {
    "fetch_diagnostics": button.button_schema(
        FetchDiagnosticsButton,
        icon="mdi:file-search-outline",
    ),
    # No entity_category: like fetch_diagnostics above, this is a control a
    # human presses directly (the same "not filed under Diagnostic where it's
    # easy to miss" reasoning as the raw key buttons), not a passive reading.
    "read_settings": button.button_schema(
        ReadSettingsButton,
        icon="mdi:sun-clock",
    ),
    # "Sync MVHR Clock" in the old YAML -- same icon, same entity_category
    # (mhrv_orig/controls.yaml's button block): a clock sync is a
    # configuration action, not a diagnostic read.
    "sync_clock": button.button_schema(
        SyncClockButton,
        icon="mdi:clock-check-outline",
        entity_category=ENTITY_CATEGORY_CONFIG,
    ),
    # "Reset Filter Timer" in the old YAML -- same icon, same entity_category
    # (mhrv_orig/controls.yaml's own comment: "restarts the service countdown
    # at the full interval and there is no way to put it back"). config, not
    # diagnostic, per PLAN.md §6/§7: this changes the unit's own state, it
    # does not merely read it. Deliberately NO vent_axia.reset_filter action
    # to go with this button -- unlike fetch_diagnostics/sync_clock, this is
    # not something mhrv.yaml's on_time schedule should ever call
    # unattended; a button a human must deliberately press is the only entry
    # point PLAN.md §7 describes for the one irreversible operation in this
    # component, the same "manual only" choice already made for
    # read_settings above.
    "reset_filter": button.button_schema(
        ResetFilterButton,
        icon="mdi:air-filter",
        entity_category=ENTITY_CATEGORY_CONFIG,
    ),
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_VENT_AXIA_ID): cv.use_id(VentAxiaHub),
        **{cv.Optional(key): schema for key, schema in BUTTONS.items()},
        **{cv.Optional(key): schema for key, schema in SEQUENCE_BUTTONS.items()},
    }
)


async def to_code(config):
    for key in BUTTONS:
        if key not in config:
            continue
        b = await button.new_button(config[key])
        await cg.register_parented(b, config[CONF_VENT_AXIA_ID])
        # key == "key_up"/"key_down"/"key_set"/"key_main" -> KEY_MASKS["up"]/...
        cg.add(b.set_mask(KEY_MASKS[key.removeprefix("key_")]))

    for key in SEQUENCE_BUTTONS:
        if key not in config:
            continue
        b = await button.new_button(config[key])
        await cg.register_parented(b, config[CONF_VENT_AXIA_ID])
