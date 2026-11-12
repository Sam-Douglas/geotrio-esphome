"""The `sensor:` platform for geotrio. Every sensor is optional; include the ones you want."""

import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import (
    DEVICE_CLASS_ENERGY,
    DEVICE_CLASS_GAS,
    DEVICE_CLASS_POWER,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_CUBIC_METER,
    UNIT_KILOWATT_HOURS,
    UNIT_WATT,
)

from . import CONF_GEOTRIO_ID, GeoTrioComponent, geotrio_ns

SensorId = geotrio_ns.enum("SensorId", is_class=True)

POWER = dict(
    unit_of_measurement=UNIT_WATT,
    accuracy_decimals=0,
    device_class=DEVICE_CLASS_POWER,
    state_class=STATE_CLASS_MEASUREMENT,
)
ELECTRICITY_METER = dict(
    unit_of_measurement=UNIT_KILOWATT_HOURS,
    accuracy_decimals=3,
    device_class=DEVICE_CLASS_ENERGY,
    state_class=STATE_CLASS_TOTAL_INCREASING,
)
GAS_METER = dict(
    unit_of_measurement=UNIT_CUBIC_METER,
    accuracy_decimals=3,
    device_class=DEVICE_CLASS_GAS,
    state_class=STATE_CLASS_TOTAL_INCREASING,
)
# Usage restarts from zero each day/week/month. Home Assistant treats a drop in a
# total_increasing sensor as a reset, which is exactly what happens.
USAGE = dict(
    unit_of_measurement=UNIT_KILOWATT_HOURS,
    accuracy_decimals=3,
    device_class=DEVICE_CLASS_ENERGY,
    state_class=STATE_CLASS_TOTAL_INCREASING,
)
# Home Assistant's "monetary" device class only allows state_class "total", which would count each
# midnight reset as negative spending. Leaving the device class off allows total_increasing.
COST = dict(
    unit_of_measurement="GBP",
    accuracy_decimals=2,
    state_class=STATE_CLASS_TOTAL_INCREASING,
    icon="mdi:currency-gbp",
)
UNIT_RATE = dict(
    unit_of_measurement="GBP/kWh",
    accuracy_decimals=4,
    state_class=STATE_CLASS_MEASUREMENT,
    icon="mdi:currency-gbp",
)
UNIT_RATE_PER_CUBIC_METRE = dict(
    unit_of_measurement="GBP/m³",
    accuracy_decimals=4,
    state_class=STATE_CLASS_MEASUREMENT,
    icon="mdi:currency-gbp",
)
STANDING_CHARGE = dict(
    unit_of_measurement="GBP/d",
    accuracy_decimals=4,
    state_class=STATE_CLASS_MEASUREMENT,
    icon="mdi:currency-gbp",
)
CALORIFIC_VALUE = dict(
    unit_of_measurement="MJ/m³",
    accuracy_decimals=2,
    state_class=STATE_CLASS_MEASUREMENT,
    entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    icon="mdi:fire",
)
VOLUME_CORRECTION = dict(
    accuracy_decimals=5,
    state_class=STATE_CLASS_MEASUREMENT,
    entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    icon="mdi:gauge",
)

# YAML key -> (C++ sensor id, default sensor settings)
SENSORS = {
    "electricity_power": (SensorId.ELECTRICITY_POWER, POWER),
    "electricity_meter": (SensorId.ELECTRICITY_METER, ELECTRICITY_METER),
    "electricity_usage_today": (SensorId.ELECTRICITY_USAGE_TODAY, USAGE),
    "electricity_usage_this_week": (SensorId.ELECTRICITY_USAGE_THIS_WEEK, USAGE),
    "electricity_usage_this_month": (SensorId.ELECTRICITY_USAGE_THIS_MONTH, USAGE),
    "electricity_cost_today": (SensorId.ELECTRICITY_COST_TODAY, COST),
    "electricity_cost_this_week": (SensorId.ELECTRICITY_COST_THIS_WEEK, COST),
    "electricity_cost_this_month": (SensorId.ELECTRICITY_COST_THIS_MONTH, COST),
    "electricity_unit_rate": (SensorId.ELECTRICITY_UNIT_RATE, UNIT_RATE),
    "electricity_standing_charge": (SensorId.ELECTRICITY_STANDING_CHARGE, STANDING_CHARGE),
    "gas_meter": (SensorId.GAS_METER, GAS_METER),
    "gas_usage_today": (SensorId.GAS_USAGE_TODAY, USAGE),
    "gas_usage_this_week": (SensorId.GAS_USAGE_THIS_WEEK, USAGE),
    "gas_usage_this_month": (SensorId.GAS_USAGE_THIS_MONTH, USAGE),
    "gas_cost_today": (SensorId.GAS_COST_TODAY, COST),
    "gas_cost_this_week": (SensorId.GAS_COST_THIS_WEEK, COST),
    "gas_cost_this_month": (SensorId.GAS_COST_THIS_MONTH, COST),
    "gas_unit_rate": (SensorId.GAS_UNIT_RATE, UNIT_RATE),
    "gas_unit_rate_per_cubic_metre": (
        SensorId.GAS_UNIT_RATE_PER_CUBIC_METRE,
        UNIT_RATE_PER_CUBIC_METRE,
    ),
    "gas_standing_charge": (SensorId.GAS_STANDING_CHARGE, STANDING_CHARGE),
    "gas_calorific_value": (SensorId.GAS_CALORIFIC_VALUE, CALORIFIC_VALUE),
    "gas_volume_correction": (SensorId.GAS_VOLUME_CORRECTION, VOLUME_CORRECTION),
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_GEOTRIO_ID): cv.use_id(GeoTrioComponent),
        **{
            cv.Optional(key): sensor.sensor_schema(**settings)
            for key, (_, settings) in SENSORS.items()
        },
    }
)


async def to_code(config):
    component = await cg.get_variable(config[CONF_GEOTRIO_ID])
    for key, (sensor_id, _) in SENSORS.items():
        if key in config:
            new_sensor = await sensor.new_sensor(config[key])
            cg.add(component.set_sensor(sensor_id, new_sensor))
