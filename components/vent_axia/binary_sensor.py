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
    # Diagnostic pages 2/3/19 -- the three fault binaries, all DEVICE_CLASS_PROBLEM
    # per the brief. rail_24v_fault is the INVERTED one (diagnostics.cpp):
    # the wire's own field is 1 == ok, 0 == fault.
    "supply_temp_fault": binary_sensor.binary_sensor_schema(
        device_class=DEVICE_CLASS_PROBLEM,
    ),
    "extract_temp_fault": binary_sensor.binary_sensor_schema(
        device_class=DEVICE_CLASS_PROBLEM,
    ),
    "rail_24v_fault": binary_sensor.binary_sensor_schema(
        device_class=DEVICE_CLASS_PROBLEM,
    ),
    # Diagnostic-ish informational flags -- present/fitted state rather than
    # a problem, and not something a user changes, so ENTITY_CATEGORY_DIAGNOSTIC
    # rather than a plain sensor per the brief.
    "wireless_fitted": binary_sensor.binary_sensor_schema(
        icon="mdi:access-point",
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    ),
    "switch_line_1": binary_sensor.binary_sensor_schema(
        icon="mdi:electric-switch",
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    ),
    "switch_line_2": binary_sensor.binary_sensor_schema(
        icon="mdi:electric-switch",
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    ),
    "switch_line_3": binary_sensor.binary_sensor_schema(
        icon="mdi:electric-switch",
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    ),
    # Diagnostic page 24. A once-a-day snapshot of the defrost-bypass state
    # machine, not the comfort bypass (`summer_bypass` above) -- same
    # actuator, different reason for opening it, kept as separate entities
    # deliberately (see vent-axia-esphome-project.md, "Summer bypass").
    "antifrost_active": binary_sensor.binary_sensor_schema(
        icon="mdi:snowflake-alert",
    ),
    # Stage 4: true while Keypad::busy() is -- a tap (including its trailing
    # gap) or a hold in progress. PLAN.md risk 3: airflow_mode transitions
    # can take ~25-30s, so this is what lets a dashboard show that a slow
    # operation is under way rather than looking stalled.
    "busy": binary_sensor.binary_sensor_schema(
        icon="mdi:progress-clock",
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
