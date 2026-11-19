if(CONFIG_FABRIC_AUTOMATION)
    set(automation_dir automation)
endif()

idf_component_register(
    SRC_DIRS core manager ${automation_dir} data_model
             data_model/clusters/boolean_state
             data_model/clusters/color_control
             data_model/clusters/fan_control
             data_model/clusters/illuminance_measurement
             data_model/clusters/level_control
             data_model/clusters/occupancy_sensing
             data_model/clusters/on_off
             data_model/clusters/power_source
             data_model/clusters/relative_humidity_measurement
             data_model/clusters/rvc_mode
             data_model/clusters/rvc_operational_state
             data_model/clusters/temperature_measurement
             data_model/clusters/thermostat
             data_model/clusters/pressure_measurement
    INCLUDE_DIRS include
    PRIV_INCLUDE_DIRS private
    PRIV_REQUIRES nvs_flash esp_timer
)
