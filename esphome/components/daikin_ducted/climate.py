import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import binary_sensor, climate, esp32, fan, sensor
from esphome.const import (
    CONF_ID,
    UNIT_CELSIUS,
    ICON_THERMOMETER,
    DEVICE_CLASS_TEMPERATURE,
    DEVICE_CLASS_PROBLEM,
    STATE_CLASS_MEASUREMENT,
)

CONF_HOMEKIT_CLIMATE = "homekit_climate"
CONF_HOMEKIT_FAN = "homekit_fan"
CONF_FILTER_CLEAN = "filter_clean"
CONF_RX_PIN = "rx_pin"
CONF_TX_PIN = "tx_pin"
CONF_ENABLE_PIN = "enable_pin"
CONF_SNIFFER_ONLY = "sniffer_only"

# Observed EKHBH/EWYQ packet profile (read-only).
ALTHERMA_TEMPERATURE_SENSORS = {
    "altherma_heating_setpoint": "mdi:thermometer-chevron-up",
    "altherma_dhw_setpoint": "mdi:water-thermometer",
    "altherma_cooling_setpoint": "mdi:thermometer-chevron-down",
    "altherma_flow_temperature": "mdi:thermometer-water",
    "altherma_dhw_temperature": "mdi:water-thermometer",
    "altherma_outdoor_temperature": "mdi:thermometer",
}
# Numeric raw diagnostic bytes: not decoded modes or heater-stage states.
ALTHERMA_DIAGNOSTIC_SENSORS = {
    "altherma_heating_control_mode_raw": "mdi:weather-partly-cloudy",
    "altherma_status_flags_raw": "mdi:code-braces",
}
ALTHERMA_BINARY_SENSORS = {
    "altherma_circulation_pump": "mdi:water-pump",
    "altherma_compressor": "mdi:engine",
    "altherma_dhw_electric_heater": "mdi:water-boiler",
    "altherma_heating_enabled": "mdi:radiator",
    "altherma_dhw_enabled": "mdi:water-boiler",
    "altherma_quiet_mode": "mdi:volume-off",
}


AUTO_LOAD = ["binary_sensor", "climate", "fan", "sensor"]

daikin_ns = cg.esphome_ns.namespace("daikin_ducted")
DaikinClimate = daikin_ns.class_("DaikinClimate", climate.Climate, cg.Component)
DaikinClimateHomeKit = daikin_ns.class_("DaikinClimateHomeKit", climate.Climate, cg.Component)
DaikinFan = daikin_ns.class_("DaikinFan", fan.Fan, cg.Component)

CONFIG_SCHEMA = climate.climate_schema(DaikinClimate).extend(
    {
        cv.Optional(CONF_RX_PIN, default=2): pins.internal_gpio_input_pin_number,
        cv.Optional(CONF_TX_PIN, default=3): pins.internal_gpio_output_pin_number,
        cv.Optional(CONF_ENABLE_PIN, default=10): pins.internal_gpio_output_pin_number,
        cv.Optional(CONF_SNIFFER_ONLY, default=True): cv.boolean,
        **{
            cv.Optional(key): sensor.sensor_schema(
                unit_of_measurement=UNIT_CELSIUS,
                accuracy_decimals=1,
                device_class=DEVICE_CLASS_TEMPERATURE,
                state_class=STATE_CLASS_MEASUREMENT,
                icon=icon,
            )
            for key, icon in ALTHERMA_TEMPERATURE_SENSORS.items()
        },
        **{
            cv.Optional(key): sensor.sensor_schema(
                accuracy_decimals=0,
                icon=icon,
            )
            for key, icon in ALTHERMA_DIAGNOSTIC_SENSORS.items()
        },
        **{
            cv.Optional(key): binary_sensor.binary_sensor_schema(icon=icon)
            for key, icon in ALTHERMA_BINARY_SENSORS.items()
        },
        cv.Optional("outdoor_intake_temperature"): sensor.sensor_schema(
            unit_of_measurement=UNIT_CELSIUS,
            icon=ICON_THERMOMETER,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_TEMPERATURE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional("coolant_temperature"): sensor.sensor_schema(
            unit_of_measurement=UNIT_CELSIUS,
            icon="mdi:coolant-temperature",
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_TEMPERATURE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional("indoor_temperature"): sensor.sensor_schema(
            unit_of_measurement=UNIT_CELSIUS,
            icon="mdi:home-thermometer",
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_TEMPERATURE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_HOMEKIT_CLIMATE): climate.climate_schema(DaikinClimateHomeKit),
        cv.Optional(CONF_HOMEKIT_FAN): fan.fan_schema(DaikinFan),
        cv.Optional(CONF_FILTER_CLEAN): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_PROBLEM,
        ),
    }
)

SENSOR_TYPES = {
    "outdoor_intake_temperature": "set_outdoor_intake_temperature_sensor",
    "coolant_temperature": "set_coolant_temperature_sensor",
    "indoor_temperature": "set_indoor_temperature_sensor",
}

async def to_code(config):
    esp32.include_builtin_idf_component("esp_driver_rmt")

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await climate.register_climate(var, config)

    cg.add(var.set_rx_pin(config[CONF_RX_PIN]))
    cg.add(var.set_tx_pin(config[CONF_TX_PIN]))
    cg.add(var.set_enable_pin(config[CONF_ENABLE_PIN]))
    cg.add(var.set_sniffer_only(config[CONF_SNIFFER_ONLY]))

    for key, funcName in SENSOR_TYPES.items():
        if key in config:
            sens = await sensor.new_sensor(config[key])
            cg.add(getattr(var, funcName)(sens))

    for key in ALTHERMA_TEMPERATURE_SENSORS:
        if key in config:
            sens = await sensor.new_sensor(config[key])
            cg.add(getattr(var, "set_" + key + "_sensor")(sens))

    for key in ALTHERMA_DIAGNOSTIC_SENSORS:
        if key in config:
            sens = await sensor.new_sensor(config[key])
            cg.add(getattr(var, "set_" + key + "_sensor")(sens))

    for key in ALTHERMA_BINARY_SENSORS:
        if key in config:
            bsens = await binary_sensor.new_binary_sensor(config[key])
            cg.add(getattr(var, "set_" + key + "_binary_sensor")(bsens))

    if CONF_FILTER_CLEAN in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_FILTER_CLEAN])
        cg.add(var.set_filter_clean_binary_sensor(sens))

    if CONF_HOMEKIT_CLIMATE in config:
        hk_config = config[CONF_HOMEKIT_CLIMATE]
        hk_var = cg.new_Pvariable(hk_config[CONF_ID])
        await cg.register_component(hk_var, hk_config)
        await climate.register_climate(hk_var, hk_config)
        cg.add(hk_var.set_parent(var))
        cg.add(var.set_auxiliary_climate(hk_var))

    if CONF_HOMEKIT_FAN in config:
        fan_config = config[CONF_HOMEKIT_FAN]
        fan_var = cg.new_Pvariable(fan_config[CONF_ID])
        await cg.register_component(fan_var, fan_config)
        await fan.register_fan(fan_var, fan_config)
        cg.add(fan_var.set_parent(var))
        cg.add(var.set_fan(fan_var))
