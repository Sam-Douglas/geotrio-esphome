"""ESPHome component for the GEO Trio II smart meter display.

ESPHome reads this file to know which YAML options the `geotrio:` block accepts (CONFIG_SCHEMA)
and to generate the C++ that creates and configures the component (to_code).
"""

import esphome.codegen as cg
from esphome.components import uart
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_TX_PIN, CONF_UART_ID
import esphome.final_validate as fv

DEPENDENCIES = ["uart"]
AUTO_LOAD = ["sensor", "binary_sensor"]

CONF_GEOTRIO_ID = "geotrio_id"
CONF_ELECTRICITY_COST_DECIMAL_PLACES = "electricity_cost_decimal_places"
CONF_GAS_COST_DECIMAL_PLACES = "gas_cost_decimal_places"
CONF_LOG_FRAMES = "log_frames"

geotrio_ns = cg.esphome_ns.namespace("geotrio")
GeoTrioComponent = geotrio_ns.class_("GeoTrioComponent", cg.Component, uart.UARTDevice)
Fuel = geotrio_ns.enum("Fuel", is_class=True)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(GeoTrioComponent),
            # Only needed if the automatic check against the tariff gets it wrong.
            cv.Optional(CONF_ELECTRICITY_COST_DECIMAL_PLACES): cv.int_range(min=0, max=7),
            cv.Optional(CONF_GAS_COST_DECIMAL_PLACES): cv.int_range(min=0, max=7),
            cv.Optional(CONF_LOG_FRAMES, default=False): cv.boolean,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA)
)


def _require_listen_only_uart(config):
    """The display's own WiFi module also uses this line, so we must never transmit on it."""
    full_config = fv.full_config.get()
    uart_path = full_config.get_path_for_id(config[CONF_UART_ID])[:-1]
    uart_config = full_config.get_config_for_path(uart_path)
    if CONF_TX_PIN in uart_config:
        raise cv.Invalid(
            "The uart used by geotrio must not have a tx_pin. The display's TX line is shared "
            "with its own WiFi module, so this device must only listen. Remove tx_pin."
        )
    return config


# The listen-only check must run first: the uart check replaces the uart id with its settings.
FINAL_VALIDATE_SCHEMA = cv.All(
    _require_listen_only_uart,
    uart.final_validate_device_schema("geotrio", baud_rate=115200, require_rx=True),
)


async def to_code(config):
    component = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(component, config)
    await uart.register_uart_device(component, config)

    cg.add(component.set_log_frames(config[CONF_LOG_FRAMES]))
    if CONF_ELECTRICITY_COST_DECIMAL_PLACES in config:
        cg.add(
            component.set_cost_decimal_places(
                Fuel.ELECTRICITY, config[CONF_ELECTRICITY_COST_DECIMAL_PLACES]
            )
        )
    if CONF_GAS_COST_DECIMAL_PLACES in config:
        cg.add(component.set_cost_decimal_places(Fuel.GAS, config[CONF_GAS_COST_DECIMAL_PLACES]))
