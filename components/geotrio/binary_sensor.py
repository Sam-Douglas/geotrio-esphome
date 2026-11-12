import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import DEVICE_CLASS_CONNECTIVITY, ENTITY_CATEGORY_DIAGNOSTIC

from . import CONF_GEOTRIO_ID, GeoTrioComponent

CONF_METER_CONNECTION = "meter_connection"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_GEOTRIO_ID): cv.use_id(GeoTrioComponent),
        cv.Optional(CONF_METER_CONNECTION): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_CONNECTIVITY,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_GEOTRIO_ID])
    if CONF_METER_CONNECTION in config:
        connection = await binary_sensor.new_binary_sensor(config[CONF_METER_CONNECTION])
        cg.add(hub.set_meter_connection_sensor(connection))
