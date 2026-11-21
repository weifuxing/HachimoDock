# [Input] ESP-IDF environment, project sources, optional target-specific version override, and Git build identity.
# [Output] Versioned ESP32-P4 firmware project with target-isolated protocol/build metadata.
# [Pos] Firmware build entry for firmware.
# [Sync] If this file changes, update `firmware/.folder.md` and README.md.

cmake_minimum_required(VERSION 3.16)
# Export the real ESP-IDF compilation context for SDK object checks.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if("$ENV{PET_P4_CHIP_FAMILY}" STREQUAL "v3")
  set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.v3.defaults")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
if("$ENV{PET_P4_CHIP_FAMILY}" STREQUAL "v3")
  idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/dependencies.v3.lock")
endif()
if("$ENV{PET_P4_SYSTEM_WIFI}" STREQUAL "1")
  if(NOT SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
  endif()
  list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.wifi-settings.defaults")
  list(APPEND SDKCONFIG_DEFAULTS "config/wifi6-ble.defaults")
  idf_build_set_property(DEPENDENCIES_LOCK
    "${CMAKE_CURRENT_LIST_DIR}/dependencies.wifi-$ENV{PET_P4_CHIP_FAMILY}.lock")
endif()
set(PROJECT_VER "0.7.103-p4-r284")
if(DEFINED ENV{PET_P4_PROJECT_VER_OVERRIDE} AND NOT "$ENV{PET_P4_PROJECT_VER_OVERRIDE}" STREQUAL "")
  set(PROJECT_VER "$ENV{PET_P4_PROJECT_VER_OVERRIDE}")
endif()
set(PET_P4_UART_BAUD 4000000)
if(DEFINED ENV{PET_P4_UART_BAUD_OVERRIDE} AND NOT "$ENV{PET_P4_UART_BAUD_OVERRIDE}" STREQUAL "")
  set(PET_P4_UART_BAUD "$ENV{PET_P4_UART_BAUD_OVERRIDE}")
endif()
set(PET_P4_PROTOCOL_SCHEMA 7)
set(PET_P4_GIT_SHA "unknown")
set(PET_P4_BUILD_DIRTY 0)
find_program(PET_P4_GIT_EXECUTABLE git)
if(PET_P4_GIT_EXECUTABLE)
  execute_process(
    COMMAND ${PET_P4_GIT_EXECUTABLE} -C ${CMAKE_CURRENT_LIST_DIR} rev-parse --short=12 HEAD
    RESULT_VARIABLE PET_P4_GIT_SHA_RESULT
    OUTPUT_VARIABLE PET_P4_GIT_SHA_OUTPUT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  if(PET_P4_GIT_SHA_RESULT EQUAL 0 AND NOT PET_P4_GIT_SHA_OUTPUT STREQUAL "")
    set(PET_P4_GIT_SHA "${PET_P4_GIT_SHA_OUTPUT}")
  endif()
  execute_process(
    COMMAND ${PET_P4_GIT_EXECUTABLE} -C ${CMAKE_CURRENT_LIST_DIR} status --porcelain --untracked-files=normal
    RESULT_VARIABLE PET_P4_GIT_STATUS_RESULT
    OUTPUT_VARIABLE PET_P4_GIT_STATUS_OUTPUT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  if(PET_P4_GIT_STATUS_RESULT EQUAL 0 AND NOT PET_P4_GIT_STATUS_OUTPUT STREQUAL "")
    set(PET_P4_BUILD_DIRTY 1)
  endif()
endif()
if(DEFINED ENV{PET_P4_SOURCE_GIT_SHA})
  set(PET_P4_GIT_SHA "$ENV{PET_P4_SOURCE_GIT_SHA}")
  if(NOT PET_P4_GIT_SHA MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "Invalid staged source Git identity")
  endif()
  set(PET_P4_BUILD_DIRTY "$ENV{PET_P4_SOURCE_DIRTY}")
endif()
set(PET_P4_BUILD_ID "${PROJECT_VER}+${PET_P4_GIT_SHA}")
if(PET_P4_BUILD_DIRTY)
  string(APPEND PET_P4_BUILD_ID "-dirty")
endif()
message(STATUS "P4 build identity: ${PET_P4_BUILD_ID}, protocol schema ${PET_P4_PROTOCOL_SCHEMA}, UART ${PET_P4_UART_BAUD}")
project(pet_manager_p4_runtime)

include("${CMAKE_CURRENT_LIST_DIR}/tools/h264_pre_v3.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/tools/h264_simd.cmake")

if("$ENV{PET_P4_SYSTEM_WIFI}" STREQUAL "1")
  # Size-optimize network/configuration and peripheral control libraries.
  # Audio DSP, media decoders, pixel loops and VM execution keep their profile.
  foreach(component esp-tls lwip espressif__esp_hosted nvs_flash json protobuf-c protocomm esp_netif
      esp_lcd esp_driver_i2c esp_driver_gpio
      esp_http_client esp_http_server esp_https_ota http_parser)
    idf_component_get_property(network_target ${component} COMPONENT_LIB)
    target_compile_options(${network_target} PRIVATE -Oz)
  endforeach()
  foreach(network_target mbedtls mbedcrypto mbedx509)
    target_compile_options(${network_target} PRIVATE -Oz)
  endforeach()

  # Keep the SDIO pools in PSRAM. ESP-Hosted's 64-byte allocation alignment
  # can fall short of the P4's 128-byte SDMMC DMA requirement.
  idf_component_get_property(hosted_lib espressif__esp_hosted COMPONENT_LIB)
  idf_component_get_property(hosted_dir espressif__esp_hosted COMPONENT_DIR)
  set(hosted_sdio "${hosted_dir}/host/port/esp/freertos/src/port_esp_hosted_host_sdio.c")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${hosted_sdio}")
  file(SHA256 "${hosted_sdio}" hosted_sdio_sha)
  if(NOT hosted_sdio_sha STREQUAL "34bc88b72951857c0f0179d18c90847219da1a42f777991942279d959281aa3a")
    message(FATAL_ERROR "Review the P4 SDIO DMA compatibility fix for this ESP-Hosted source")
  endif()
  file(READ "${hosted_sdio}" hosted_sdio_code)
  foreach(direction read write)
    set(copy_before "")
    set(copy_after "")
    if(direction STREQUAL "write")
      set(copy_before "memcpy(bounce, ptr, size);")
    else()
      set(copy_after "if (res == ESP_OK) memcpy(ptr, bounce, size);")
    endif()
    set(direct "res = sdmmc_io_${direction}_blocks(card, function, addr, ptr, size);")
    string(REPLACE "${direct}"
"if (card->host.check_buffer_alignment(card->host.slot, ptr, size)) {
			${direct}
		} else {
			// The P4 card initialization supplies one internal DMA block.
			// The caller holds the same SDIO lock used by CMD53 byte mode.
			uint8_t *bounce = card->host.dma_aligned_buffer;
			if (!bounce) return ESP_ERR_INVALID_ARG;
			size = ESP_BLOCK_SIZE;
			${copy_before}
			res = sdmmc_io_${direction}_blocks(card, function, addr, bounce, size);
			${copy_after}
		}" hosted_sdio_code "${hosted_sdio_code}")
  endforeach()
  set(hosted_sdio_fixed "${CMAKE_BINARY_DIR}/pet_p4_hosted_sdio.c")
  file(WRITE "${hosted_sdio_fixed}"
    "#line 1 \"esp_hosted/port_esp_hosted_host_sdio.c\"\n${hosted_sdio_code}")
  set(hosted_transport "${hosted_dir}/host/drivers/transport/transport_drv.c")
  set(hosted_transport_fixed "${CMAKE_BINARY_DIR}/pet_p4_hosted_transport.c")
  set(hosted_radio_patcher "${CMAKE_CURRENT_LIST_DIR}/tools/patch_hosted_radio.py")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${hosted_radio_patcher}" "${hosted_transport}")
  execute_process(COMMAND "${PYTHON}" "${hosted_radio_patcher}"
    "${hosted_transport}" "${hosted_transport_fixed}" RESULT_VARIABLE hosted_radio_status)
  if(NOT hosted_radio_status EQUAL 0)
    message(FATAL_ERROR "Unreviewed ESP-Hosted radio transport source")
  endif()
  get_target_property(hosted_sources ${hosted_lib} SOURCES)
  set(hosted_sdio_replaced 0)
  set(hosted_transport_replaced 0)
  set(hosted_fixed_sources "")
  foreach(hosted_source IN LISTS hosted_sources)
    get_filename_component(hosted_absolute "${hosted_source}" REALPATH BASE_DIR "${hosted_dir}")
    if(hosted_absolute STREQUAL hosted_sdio)
      list(APPEND hosted_fixed_sources "${hosted_sdio_fixed}")
      math(EXPR hosted_sdio_replaced "${hosted_sdio_replaced}+1")
    elseif(hosted_absolute STREQUAL hosted_transport)
      list(APPEND hosted_fixed_sources "${hosted_transport_fixed}")
      math(EXPR hosted_transport_replaced "${hosted_transport_replaced}+1")
    else()
      list(APPEND hosted_fixed_sources "${hosted_source}")
    endif()
  endforeach()
  if(NOT hosted_sdio_replaced EQUAL 1 OR NOT hosted_transport_replaced EQUAL 1)
    message(FATAL_ERROR "P4 SDIO fixes must replace the two pinned ESP-Hosted sources")
  endif()
  set_property(TARGET ${hosted_lib} PROPERTY SOURCES "${hosted_fixed_sources}")
endif()

# V1's pinned NimBLE HID source lacks two OR operators at security level 3.
# Generate only the corrected translation unit; never mutate installed SDKs.
if(CONFIG_BT_NIMBLE_ENABLED AND "$ENV{PET_P4_SYSTEM_WIFI}" STREQUAL "1")
  set(hid_patcher "${CMAKE_CURRENT_LIST_DIR}/tools/patch_nimble_hid.py")
  set(hid_original "${IDF_PATH}/components/bt/host/nimble/nimble/nimble/host/services/hid/src/ble_svc_hid.c")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${hid_patcher}" "${hid_original}")
  execute_process(COMMAND "${PYTHON}" "${hid_patcher}" --sdk-dir "${IDF_PATH}" --build-dir "${CMAKE_BINARY_DIR}"
    RESULT_VARIABLE hid_status OUTPUT_VARIABLE hid_selected OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT hid_status EQUAL 0)
    message(FATAL_ERROR "Unreviewed NimBLE HID source")
  endif()
  idf_component_get_property(hid_bt_lib bt COMPONENT_LIB)
  idf_component_get_property(hid_bt_dir bt COMPONENT_DIR)
  get_target_property(hid_sources ${hid_bt_lib} SOURCES)
  set(hid_fixed_sources "")
  set(hid_replaced 0)
  foreach(hid_source IN LISTS hid_sources)
    get_filename_component(hid_absolute "${hid_source}" REALPATH BASE_DIR "${hid_bt_dir}")
    if(hid_absolute STREQUAL hid_original)
      list(APPEND hid_fixed_sources "${hid_selected}")
      math(EXPR hid_replaced "${hid_replaced}+1")
    else()
      list(APPEND hid_fixed_sources "${hid_source}")
    endif()
  endforeach()
  if(NOT hid_replaced EQUAL 1)
    message(FATAL_ERROR "NimBLE HID fix must select exactly one source")
  endif()
  set_property(TARGET ${hid_bt_lib} PROPERTY SOURCES "${hid_fixed_sources}")
endif()
