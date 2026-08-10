import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC

from . import CONF_VENT_AXIA_ID, VentAxiaHub, vent_axia_ns

DEPENDENCIES = ["vent_axia"]

TextKey = vent_axia_ns.enum("TextKey", is_class=True)

# The enum-keyed platform pattern (PLAN.md §5): one dict entry per text
# sensor. A later stage adding a text sensor extends this dict and nothing
# else -- CONFIG_SCHEMA and to_code are generic over its keys.
TEXT_SENSORS = {
    "display_line_1": text_sensor.text_sensor_schema(
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    ),
    "display_line_2": text_sensor.text_sensor_schema(
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    ),
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_VENT_AXIA_ID): cv.use_id(VentAxiaHub),
        **{cv.Optional(key): schema for key, schema in TEXT_SENSORS.items()},
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_VENT_AXIA_ID])
    for key in TEXT_SENSORS:
        if key not in config:
            continue
        sensor = await text_sensor.new_text_sensor(config[key])
        cg.add(hub.set_text_sensor(getattr(TextKey, key.upper()), sensor))
