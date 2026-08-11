import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    DEVICE_CLASS_DURATION,
    DEVICE_CLASS_HUMIDITY,
    DEVICE_CLASS_TEMPERATURE,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
    UNIT_HOUR,
    UNIT_MINUTE,
    UNIT_PERCENT,
    UNIT_REVOLUTIONS_PER_MINUTE,
)

from . import CONF_VENT_AXIA_ID, VentAxiaHub, vent_axia_ns

DEPENDENCIES = ["vent_axia"]

SensorKey = vent_axia_ns.enum("SensorKey", is_class=True)

# The enum-keyed platform pattern (PLAN.md §5): one dict entry per sensor.
# Units, device classes, state classes and icons live here in the schema, not
# in YAML -- a later stage (the diagnostics table) extends this dict and
# nothing else.
#
# STATE_CLASS_MEASUREMENT decision (applies to every diagnostic-page sensor
# below, not just airflow/boost_time_remaining above them): these arrive from
# a once-a-day scrape or whenever a human happens to browse the unit's menu
# by hand (diagnostics.h), not a continuous feed. STATE_CLASS_MEASUREMENT is
# still the honest choice -- each value is a genuine instantaneous reading (a
# temperature, an RPM, hours remaining), never an accumulating total, so it
# really is a "measurement" in Home Assistant's sense. The cost is a sparse
# graph between updates, which is an accurate picture of the update cadence,
# not a state class lying about it. The alternative (no state_class at all)
# would just lose HA's statistics/history for numbers that are worth
# graphing even if only sampled once a day.
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
    # Diagnostic pages 0/1. NOTE: this is the COMMANDED airflow percentage,
    # not a measured flow -- see diagnostics.cpp for the evidence. Named
    # "(Commanded)" in mhrv.yaml so it is never mistaken for the live
    # `airflow` sensor above, which reads the same status-line percentage but
    # continuously.
    "supply_airflow_set": sensor.sensor_schema(
        unit_of_measurement=UNIT_PERCENT,
        accuracy_decimals=0,
        state_class=STATE_CLASS_MEASUREMENT,
        icon="mdi:fan",
    ),
    "extract_airflow_set": sensor.sensor_schema(
        unit_of_measurement=UNIT_PERCENT,
        accuracy_decimals=0,
        state_class=STATE_CLASS_MEASUREMENT,
        icon="mdi:fan",
    ),
    "supply_motor_pwm": sensor.sensor_schema(
        unit_of_measurement=UNIT_PERCENT,
        accuracy_decimals=0,
        state_class=STATE_CLASS_MEASUREMENT,
        icon="mdi:engine-outline",
    ),
    "extract_motor_pwm": sensor.sensor_schema(
        unit_of_measurement=UNIT_PERCENT,
        accuracy_decimals=0,
        state_class=STATE_CLASS_MEASUREMENT,
        icon="mdi:engine-outline",
    ),
    "supply_fan_rpm": sensor.sensor_schema(
        unit_of_measurement=UNIT_REVOLUTIONS_PER_MINUTE,
        accuracy_decimals=0,
        state_class=STATE_CLASS_MEASUREMENT,
        icon="mdi:fan",
    ),
    "extract_fan_rpm": sensor.sensor_schema(
        unit_of_measurement=UNIT_REVOLUTIONS_PER_MINUTE,
        accuracy_decimals=0,
        state_class=STATE_CLASS_MEASUREMENT,
        icon="mdi:fan",
    ),
    # Diagnostic pages 2/3.
    "supply_air_temp": sensor.sensor_schema(
        unit_of_measurement=UNIT_CELSIUS,
        device_class=DEVICE_CLASS_TEMPERATURE,
        accuracy_decimals=0,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    "extract_air_temp": sensor.sensor_schema(
        unit_of_measurement=UNIT_CELSIUS,
        device_class=DEVICE_CLASS_TEMPERATURE,
        accuracy_decimals=0,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    # Diagnostic page 4: the unit's own internal sensor. Distinct from the
    # `Indoor Temp` menu screen, which is a bypass setpoint, not a reading --
    # see vent-axia-esphome-project.md. Unpublished as a group when
    # diagnostics.cpp's "no sensor fitted" sentinel fires.
    "indoor_temp": sensor.sensor_schema(
        unit_of_measurement=UNIT_CELSIUS,
        device_class=DEVICE_CLASS_TEMPERATURE,
        accuracy_decimals=0,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    "indoor_humidity": sensor.sensor_schema(
        unit_of_measurement=UNIT_PERCENT,
        device_class=DEVICE_CLASS_HUMIDITY,
        accuracy_decimals=0,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    "indoor_humidity_avg": sensor.sensor_schema(
        unit_of_measurement=UNIT_PERCENT,
        device_class=DEVICE_CLASS_HUMIDITY,
        accuracy_decimals=0,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    # Diagnostic page 23. filter_change_due (binary_sensor.py) is derived
    # from this reaching zero.
    "filter_hours": sensor.sensor_schema(
        unit_of_measurement=UNIT_HOUR,
        device_class=DEVICE_CLASS_DURATION,
        accuracy_decimals=0,
        state_class=STATE_CLASS_MEASUREMENT,
        icon="mdi:air-filter",
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
