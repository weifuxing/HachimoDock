# ESP-IDF 5.5.1 predates the revision selector used by esp_h264 1.5.0.
# Select the R273 pre-v3 archives from the actual compiled chip range.
if(IDF_TARGET STREQUAL "esp32p4" AND "$ENV{PET_P4_SYSTEM_WIFI}" STREQUAL "1"
   AND "$ENV{PET_P4_CHIP_FAMILY}" STREQUAL "v1")
  if(NOT DEFINED CONFIG_ESP32P4_REV_MAX_FULL OR CONFIG_ESP32P4_REV_MAX_FULL GREATER_EQUAL 200)
    message(FATAL_ERROR "The Wi-Fi 6 v1 build must target pre-v3 P4 silicon")
  endif()
  idf_component_get_property(h264_library espressif__esp_h264 COMPONENT_LIB)
  idf_component_get_property(h264_directory espressif__esp_h264 COMPONENT_DIR)
  get_target_property(h264_links ${h264_library} INTERFACE_LINK_LIBRARIES)
  foreach(codec tinyh264 openh264)
    set(codec_archive "${h264_directory}/sw/libs/esp32p4_less_v3/lib${codec}.a")
    if(NOT codec IN_LIST h264_links OR NOT EXISTS "${codec_archive}")
      message(FATAL_ERROR "Cannot select the pre-v3 P4 ${codec} archive")
    endif()
    list(TRANSFORM h264_links REPLACE "^${codec}$" "${codec_archive}")
  endforeach()
  set_property(TARGET ${h264_library} PROPERTY INTERFACE_LINK_LIBRARIES "${h264_links}")
endif()
