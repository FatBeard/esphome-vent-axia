import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import select

from . import CONF_VENT_AXIA_ID, VentAxiaHub, vent_axia_ns

DEPENDENCIES = ["vent_axia"]

SelectKey = vent_axia_ns.enum("SelectKey", is_class=True)
VentAxiaSelect = vent_axia_ns.class_("VentAxiaSelect", select.Select, cg.Parented.template(VentAxiaHub))

# Boost, as an absolute set-point (PLAN.md §3/§6) rather than four separate
# "press N times" buttons -- see PLAN.md §6's "Modelling choices": the
# unit's Main key is a cumulative counter, so "boost for 60 minutes" is
# inherently a set-ABSOLUTE operation that must normalise first, which is
# what makes this a select rather than a button.
#
# The list order is LOAD-BEARING, not cosmetic: vent_axia.h's
# VentAxiaSelect::control(size_t index) hands the raw index straight through
# to sequence.h's AirflowTarget (NORMAL=0, BOOST_30=1, BOOST_60=2, PURGE=3)
# with no separate lookup table, so this order must keep matching that enum
# exactly. Continuous boost is deliberately NOT a fifth option -- PLAN.md §4
# and CLAUDE.md's device invariants are explicit that this is a decision,
# not an oversight: it is the one boost mode with no reliable evidence on
# the display (a plain airflow percentage indistinguishable from a high
# Normal rate), so it is neither selectable nor decoded.
AIRFLOW_MODE_OPTIONS = ["Normal", "Boost 30 min", "Boost 60 min", "Purge"]

# Deliberately NOT optimistic (PLAN.md §6), same reasoning as switch.py's
# summer_mode: control() (vent_axia.h) only ever starts a SetAirflowMode run
# (sequence.h) -- it never calls publish_state() itself. What Home Assistant
# shows is derived entirely from the hub's own passive status-line decode
# (VentAxiaHub::publish_airflow_mode_()), not from a dedicated read-back
# sequence, since status.cpp already tracks boosting/purging/
# boost_time_remaining for the generic sensors -- see that function's own
# comment for the two documented approximations (continuous boost, and a
# countdown of 30 minutes or less) this reuse inherits.
#
# No restore_mode override, unlike switch.py's summer_mode: ESPHome's select
# entity has no restore-on-boot behaviour to disable in the first place (it
# is not a Switch), so there is nothing here that could come back holding a
# stale command the way a restored switch could.
SELECTS = {
    "airflow_mode": select.select_schema(
        VentAxiaSelect,
        icon="mdi:fan-auto",
    ),
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_VENT_AXIA_ID): cv.use_id(VentAxiaHub),
        **{cv.Optional(key): schema for key, schema in SELECTS.items()},
    }
)


async def to_code(config):
    for key in SELECTS:
        if key not in config:
            continue
        sel = await select.new_select(config[key], options=AIRFLOW_MODE_OPTIONS)
        await cg.register_parented(sel, config[CONF_VENT_AXIA_ID])
        cg.add(sel.set_key(getattr(SelectKey, key.upper())))
