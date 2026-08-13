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
# to sequence.h's AirflowTarget (NORMAL=0, BOOST_30=1, BOOST_60=2,
# BOOST_CONTINUOUS=3, PURGE=4) with no separate lookup table, so this order
# must keep matching that enum exactly. Continuous boost was excluded here
# until 13 Aug 2026, on the grounds it had no reliable evidence on the
# display -- reopened against live evidence from 192.168.1.200 (see the plan
# this shipped under): StatusTracker::continuous_boost() (status.h) decodes
# it from the same "Boost Airflow" line1 signal boosting() already trusts,
# confirmed only after CONTINUOUS_CONFIRM_MS (20s, comfortably over
# ALTERNATION_TIMEOUT_MS's 12s sticky window -- see that constant's own
# comment for the live measurement behind the margin) to rule out a timed
# boost's own trailing edge.
AIRFLOW_MODE_OPTIONS = ["Normal", "Boost 30 min", "Boost 60 min", "Boost Continuous", "Purge"]

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
    hub = await cg.get_variable(config[CONF_VENT_AXIA_ID])
    for key in SELECTS:
        if key not in config:
            continue
        sel = await select.new_select(config[key], options=AIRFLOW_MODE_OPTIONS)
        # Both directions, two separate calls -- see number.py's to_code().
        # This one bites harder than the other two: airflow_mode is published
        # from the passive status decode on every frame, so without
        # set_select() the entity sat unknown even though the hub was working
        # out the right answer ~3 times a second.
        await cg.register_parented(sel, config[CONF_VENT_AXIA_ID])
        cg.add(sel.set_key(getattr(SelectKey, key.upper())))
        cg.add(hub.set_select(getattr(SelectKey, key.upper()), sel))
