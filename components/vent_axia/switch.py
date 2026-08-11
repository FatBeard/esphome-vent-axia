import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from esphome.const import ENTITY_CATEGORY_CONFIG

from . import CONF_VENT_AXIA_ID, VentAxiaHub, vent_axia_ns

DEPENDENCIES = ["vent_axia"]

SwitchKey = vent_axia_ns.enum("SwitchKey", is_class=True)
VentAxiaSwitch = vent_axia_ns.class_("VentAxiaSwitch", switch.Switch, cg.Parented.template(VentAxiaHub))

# The one bypass switch (PLAN.md §6). Deliberately NOT optimistic:
# write_state() (vent_axia.h) only ever starts a WriteSetting run
# (sequence.h) through the hub -- it never calls publish_state() itself.
# What Home Assistant shows is whatever ReadSettings most recently confirmed
# was actually on the unit, so a toggle snaps back until that round trip
# completes (WriteSetting's own read-back, a few seconds) -- the honest
# behaviour for a remote control pressing buttons on a menu, not a bug to
# paper over.
#
# restore_mode is DISABLED for the same reason: the unit is the only source
# of truth for this value, and a value restored from flash at boot would
# just fight the first real read -- mhrv_orig/summer_bypass.yaml made
# exactly this choice (`restore_mode: DISABLED`).
SWITCHES = {
    "summer_mode": switch.switch_schema(
        VentAxiaSwitch,
        icon="mdi:sun-thermometer",
        entity_category=ENTITY_CATEGORY_CONFIG,
        default_restore_mode="DISABLED",
    ),
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_VENT_AXIA_ID): cv.use_id(VentAxiaHub),
        **{cv.Optional(key): schema for key, schema in SWITCHES.items()},
    }
)


async def to_code(config):
    for key in SWITCHES:
        if key not in config:
            continue
        sw = await switch.new_switch(config[key])
        await cg.register_parented(sw, config[CONF_VENT_AXIA_ID])
        cg.add(sw.set_key(getattr(SwitchKey, key.upper())))
