cmake_minimum_required(VERSION 3.22)
set(EXTRA_COMPONENT_DIRS "../../components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(local_fabric_demo)

# rc0.2's DAC revocation parser ignores nodiscard CHIP errors (IDF 6.2/GCC 16).
# Limit the diagnostic exception to that upstream translation unit.
idf_component_get_property(matter_dir sped0n__esp_matter COMPONENT_DIR)
idf_component_get_property(matter_lib sped0n__esp_matter COMPONENT_LIB)
set_property(SOURCE "${matter_dir}/components/esp_matter_controller/attestation_verification/esp_matter_da_revocation_check.cpp"
             TARGET_DIRECTORY ${matter_lib} APPEND PROPERTY COMPILE_OPTIONS -Wno-error=unused-result)
set(chip_src "${matter_dir}/connectedhomeip/connectedhomeip/src")
set_property(SOURCE "${chip_src}/controller/AutoCommissioner.cpp" "${chip_src}/controller/CHIPDeviceController.cpp"
             TARGET_DIRECTORY ${matter_lib} APPEND PROPERTY COMPILE_OPTIONS -Wno-error=format)
set_property(SOURCE "${chip_src}/platform/ESP32/ESP32Utils.cpp"
             TARGET_DIRECTORY ${matter_lib} APPEND PROPERTY COMPILE_OPTIONS -Wno-error=stringop-overread)
