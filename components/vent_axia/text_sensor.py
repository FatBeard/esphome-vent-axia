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
    # No entity_category: these are the two lines the unit's own screen is
    # showing right now, meant to be glanced at directly rather than filed
    # under Diagnostic -- same "top-level, not easy to miss" reasoning as the
    # raw key buttons in button.py.
    "display_line_1": text_sensor.text_sensor_schema(),
    "display_line_2": text_sensor.text_sensor_schema(),
    # The current status-loop message (see status.h), e.g. "Normal Airflow"
    # or "Summer Bypass On" -- a friendlier, non-diagnostic-category sibling
    # of display_line_1 for the one line that is actually meant to be read.
    "status_message": text_sensor.text_sensor_schema(),
    # Diagnostic page 24's mode, spelled out -- see diagnostics.cpp for the
    # mode -> text table. antifrost_active (binary_sensor.py) is the same
    # field as a plain boolean.
    "antifrost_mode": text_sensor.text_sensor_schema(
        icon="mdi:snowflake-thermometer",
    ),
    # Diagnostic pages 25/26, both ENTITY_CATEGORY_DIAGNOSTIC per the brief --
    # identifying information about the unit, not something a user watches.
    "serial_number": text_sensor.text_sensor_schema(
        icon="mdi:identifier",
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    ),
    "firmware_version": text_sensor.text_sensor_schema(
        icon="mdi:chip",
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    ),
    # Optional escape hatch (diagnostics.h): "NN: <line2>" for whichever
    # diagnostic page was last seen, decoded by the table or not -- a page
    # nobody has taught this component to decode yet is still visible to a
    # human this way, with no component change. Like every entity in this
    # dict it is opt-in -- it only exists if mhrv.yaml declares the block --
    # so there is no publish cost for a config that leaves it out.
    "raw_diagnostic_page": text_sensor.text_sensor_schema(
        icon="mdi:code-tags",
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    ),
    # Stage 5: wall-clock timestamp of the last successful FetchDiagnostics
    # run (entities.h's TextKey::DIAGNOSTICS_UPDATED). Stays unknown until a
    # run actually completes -- and, with no time_id configured, forever,
    # which is a deliberate no-op rather than a failure (vent_axia.h's
    # stamp_diagnostics_updated_()).
    "diagnostics_updated": text_sensor.text_sensor_schema(
        icon="mdi:clock-check-outline",
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
