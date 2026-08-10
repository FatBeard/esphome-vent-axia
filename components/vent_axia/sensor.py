import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    DEVICE_CLASS_DURATION,
    STATE_CLASS_MEASUREMENT,
    UNIT_MINUTE,
    UNIT_PERCENT,
)

from . import CONF_VENT_AXIA_ID, VentAxiaHub, vent_axia_ns

DEPENDENCIES = ["vent_axia"]

SensorKey = vent_axia_ns.enum("SensorKey", is_class=True)

# The enum-keyed platform pattern (PLAN.md §5): one dict entry per sensor.
# Units, device classes, state classes and icons live here in the schema, not
# in YAML -- a later stage (the diagnostics table) extends this dict and
# nothing else.
SENSORS = {
    "airflow": sensor.sensor_schema(
        unit_of_measurement=UNIT_PERCENT,
        accuracy_decimals=0,
        state_class=STATE_CLASS_MEASUREMENT,
        icon="mdi:fan",
    ),
    # Unpublished outside a timed boost (30/60 min) or purge -- see
    # status.h's class comment on why continuous boost has no countdown.
    "boost_time_remaining": sensor.sensor_schema(
        unit_of_measurement=UNIT_MINUTE,
        device_class=DEVICE_CLASS_DURATION,
        accuracy_decimals=0,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_VENT_AXIA_ID): cv.use_id(VentAxiaHub),
        **{cv.Optional(key): schema for key, schema in SENSORS.items()},
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_VENT_AXIA_ID])
    for key in SENSORS:
        if key not in config:
            continue
        sens = await sensor.new_sensor(config[key])
        cg.add(hub.set_sensor(getattr(SensorKey, key.upper()), sens))
