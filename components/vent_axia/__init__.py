import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart
from esphome.const import CONF_ID

CODEOWNERS = ["@FatBeard"]
DEPENDENCIES = ["uart"]

vent_axia_ns = cg.esphome_ns.namespace("vent_axia")
VentAxiaHub = vent_axia_ns.class_("VentAxiaHub", cg.Component, uart.UARTDevice)

CONF_VENT_AXIA_ID = "vent_axia_id"
CONF_READ_ONLY = "read_only"

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(VentAxiaHub),
            # Mutes the keypad entirely: the component still receives, decodes and
            # publishes, but transmits nothing. Lets the production firmware be
            # soak-tested against a live unit with no risk of driving it.
            cv.Optional(CONF_READ_ONLY, default=False): cv.boolean,
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
