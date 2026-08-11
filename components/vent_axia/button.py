import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button
from esphome.const import ENTITY_CATEGORY_CONFIG

from . import CONF_VENT_AXIA_ID, KEY_MASKS, VentAxiaHub, vent_axia_ns

DEPENDENCIES = ["vent_axia"]

KeypadButton = vent_axia_ns.class_("KeypadButton", button.Button)

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
BUTTONS = {
    f"key_{name}": button.button_schema(
        KeypadButton,
        icon="mdi:remote",
        entity_category=ENTITY_CATEGORY_CONFIG,
    )
    for name in KEY_MASKS
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_VENT_AXIA_ID): cv.use_id(VentAxiaHub),
        **{cv.Optional(key): schema for key, schema in BUTTONS.items()},
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
