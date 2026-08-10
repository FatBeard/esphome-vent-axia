import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import (
    DEVICE_CLASS_CONNECTIVITY,
    DEVICE_CLASS_PROBLEM,
    ENTITY_CATEGORY_DIAGNOSTIC,
)

from . import CONF_VENT_AXIA_ID, VentAxiaHub, vent_axia_ns

DEPENDENCIES = ["vent_axia"]

BinaryKey = vent_axia_ns.enum("BinaryKey", is_class=True)

# The enum-keyed platform pattern (PLAN.md §5): one dict entry per binary
# sensor. Units/device classes/state classes/icons live here in the schema,
# not in YAML -- a later stage (the diagnostics table) extends this dict and
# nothing else.
BINARY_SENSORS = {
    "summer_bypass": binary_sensor.binary_sensor_schema(
        icon="mdi:sun-snowflake-variant",
    ),
    "boosting": binary_sensor.binary_sensor_schema(
        icon="mdi:fan-plus",
    ),
    "purging": binary_sensor.binary_sensor_schema(
        icon="mdi:fan-chevron-up",
    ),
    "defrost_active": binary_sensor.binary_sensor_schema(
        icon="mdi:snowflake-melt",
    ),
    "dryout_active": binary_sensor.binary_sensor_schema(
        icon="mdi:water-percent",
    ),
    # Second source arrives in stage 3 (diagnostic page 23's filter hours
    # reaching zero) -- see entities.h. The two are expected to agree.
    "filter_change_due": binary_sensor.binary_sensor_schema(
        device_class=DEVICE_CLASS_PROBLEM,
    ),
    "link_up": binary_sensor.binary_sensor_schema(
        device_class=DEVICE_CLASS_CONNECTIVITY,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    ),
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_VENT_AXIA_ID): cv.use_id(VentAxiaHub),
        **{cv.Optional(key): schema for key, schema in BINARY_SENSORS.items()},
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_VENT_AXIA_ID])
    for key in BINARY_SENSORS:
        if key not in config:
            continue
        bs = await binary_sensor.new_binary_sensor(config[key])
        cg.add(hub.set_binary_sensor(getattr(BinaryKey, key.upper()), bs))
