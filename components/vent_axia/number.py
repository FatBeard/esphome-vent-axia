import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number
from esphome.const import DEVICE_CLASS_TEMPERATURE, ENTITY_CATEGORY_CONFIG, UNIT_CELSIUS

from . import CONF_VENT_AXIA_ID, VentAxiaHub, vent_axia_ns

DEPENDENCIES = ["vent_axia"]

NumberKey = vent_axia_ns.enum("NumberKey", is_class=True)
VentAxiaNumber = vent_axia_ns.class_("VentAxiaNumber", number.Number, cg.Parented.template(VentAxiaHub))

# The two bypass temperatures (PLAN.md §6), same "not optimistic" shape as
# switch.py's summer_mode: control() (vent_axia.h) only ever starts a
# WriteSetting run, it never publishes a value itself -- what Home Assistant
# shows comes solely from ReadSettings' own observations.
#
# Each entry pairs a number_schema() (icon/category/unit/device_class) with
# the min/max/step kwargs new_number() needs separately -- unlike
# sensor.sensor_schema(), number.number_schema() does not carry range at all
# (see esphome/components/number/__init__.py: number_schema() vs. new_number()/
# setup_number_core_()), so this dict's shape has to carry both.
NUMBERS = {
    "bypass_indoor_temp": (
        number.number_schema(
            VentAxiaNumber,
            icon="mdi:home-thermometer",
            entity_category=ENTITY_CATEGORY_CONFIG,
            unit_of_measurement=UNIT_CELSIUS,
            device_class=DEVICE_CLASS_TEMPERATURE,
        ),
        # 16-40 C -- the manual's own documented range for this screen
        # (mhrv_orig/summer_bypass.yaml).
        {"min_value": 16, "max_value": 40, "step": 1},
    ),
    "bypass_outdoor_temp": (
        number.number_schema(
            VentAxiaNumber,
            icon="mdi:snowflake-thermometer",
            entity_category=ENTITY_CATEGORY_CONFIG,
            unit_of_measurement=UNIT_CELSIUS,
            device_class=DEVICE_CLASS_TEMPERATURE,
        ),
        # 5-20 C -- CONFIRMED (PLAN.md risk 6, resolved 14 Aug 2026): this
        # screen is not in the manual at all and is only reachable through
        # Indoor Temp's editor, but the range itself is documented
        # elsewhere and is no longer a guess. A value the unit refuses looks
        # identical to a dropped keypress from here -- there is no separate
        # "rejected" signal -- so AdjustField's guard (40 taps, sequence.h)
        # is what eventually stops the loop either way, and the write then
        # fails with a clear log message rather than hanging.
        {"min_value": 5, "max_value": 20, "step": 1},
    ),
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_VENT_AXIA_ID): cv.use_id(VentAxiaHub),
        **{cv.Optional(key): schema for key, (schema, _range) in NUMBERS.items()},
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_VENT_AXIA_ID])
    for key, (_schema, range_kwargs) in NUMBERS.items():
        if key not in config:
            continue
        num = await number.new_number(config[key], **range_kwargs)
        # BOTH directions have to be wired, and each is a separate call:
        # register_parented()/set_key() give the entity the hub (the write
        # path, control() -> write_number()), while set_number() gives the hub
        # the entity (the read path, publish_number_()). Wiring only the first
        # is silent -- publish_number_() nullptr-checks its slot, so a write
        # still reached the unit but no value ever came back and Home
        # Assistant showed the slider with no state at all, forever.
        await cg.register_parented(num, config[CONF_VENT_AXIA_ID])
        cg.add(num.set_key(getattr(NumberKey, key.upper())))
        cg.add(hub.set_number(getattr(NumberKey, key.upper()), num))
