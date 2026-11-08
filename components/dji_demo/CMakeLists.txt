set(DJI_DEMO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../vendor/dji_osmo_sdk")

# Compile the pinned upstream sources from the submodule in place. No DJI
# source files are copied or modified in this repository.
idf_component_register(
    SRCS
        "${DJI_DEMO_ROOT}/utils/crc/custom_crc16.c"
        "${DJI_DEMO_ROOT}/utils/crc/custom_crc32.c"
        "${DJI_DEMO_ROOT}/protocol/dji_protocol_parser.c"
        "${DJI_DEMO_ROOT}/protocol/dji_protocol_data_processor.c"
        "${DJI_DEMO_ROOT}/protocol/dji_protocol_data_descriptors.c"
        "${DJI_DEMO_ROOT}/protocol/dji_protocol_data_structures.c"
        "${DJI_DEMO_ROOT}/data/data.c"
        "${DJI_DEMO_ROOT}/logic/connect_logic.c"
        "${DJI_DEMO_ROOT}/logic/command_logic.c"
        "${DJI_DEMO_ROOT}/logic/status_logic.c"
        "${DJI_DEMO_ROOT}/logic/enums_logic.c"
    INCLUDE_DIRS
        "${DJI_DEMO_ROOT}/utils/crc"
        "${DJI_DEMO_ROOT}/protocol"
        "${DJI_DEMO_ROOT}/ble"
        "${DJI_DEMO_ROOT}/data"
        "${DJI_DEMO_ROOT}/logic"
    PRIV_REQUIRES bt nvs_flash
)
