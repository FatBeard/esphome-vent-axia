from esphome import automation
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart
from esphome.const import CONF_ID, CONF_TRIGGER_ID

CODEOWNERS = ["@FatBeard"]
DEPENDENCIES = ["uart"]

vent_axia_ns = cg.esphome_ns.namespace("vent_axia")
VentAxiaHub = vent_axia_ns.class_("VentAxiaHub", cg.Component, uart.UARTDevice)
# (page, line2) -- see diagnostics.h and vent_axia.h's DiagnosticPageTrigger.
# Fires for every diagnostic page the display passes through, decoded by the
# table (diagnostics.cpp) or not, so a page nobody has taught the component
# to decode yet can still be acted on from YAML without a component change.
DiagnosticPageTrigger = vent_axia_ns.class_(
    "DiagnosticPageTrigger", automation.Trigger.template(cg.uint8, cg.std_string)
)

CONF_VENT_AXIA_ID = "vent_axia_id"
CONF_READ_ONLY = "read_only"
CONF_ON_DIAGNOSTIC_PAGE = "on_diagnostic_page"

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(VentAxiaHub),
            # Mutes the keypad entirely: the component still receives, decodes and
            # publishes, but transmits nothing. Lets the production firmware be
            # soak-tested against a live unit with no risk of driving it.
            cv.Optional(CONF_READ_ONLY, default=False): cv.boolean,
            cv.Optional(CONF_ON_DIAGNOSTIC_PAGE): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(DiagnosticPageTrigger),
                }
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA)
)

# The wired-remote link is 9600 8N1. The upstream component only warned about a
# mismatch at runtime; catching it at compile time is free.
FINAL_VALIDATE_SCHEMA = uart.final_validate_device_schema(
    "vent_axia",
    baud_rate=9600,
    require_tx=True,
    require_rx=True,
    parity="NONE",
    stop_bits=1,
    data_bits=8,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    cg.add(var.set_read_only(config[CONF_READ_ONLY]))

    for conf in config.get(CONF_ON_DIAGNOSTIC_PAGE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID])
        cg.add(var.register_diagnostic_page_trigger(trigger))
        await automation.build_automation(trigger, [(cg.uint8, "page"), (cg.std_string, "line2")], conf)
