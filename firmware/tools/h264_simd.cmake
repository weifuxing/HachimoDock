# Run after project()/component resolution: the dependency manager may restore
# managed_components. Only generated binary output and project sources are used.
if(IDF_TARGET STREQUAL "esp32p4" AND "$ENV{PET_P4_SYSTEM_WIFI}" STREQUAL "1")
  idf_component_get_property(h264_library espressif__esp_h264 COMPONENT_LIB)
  idf_component_get_property(h264_directory espressif__esp_h264 COMPONENT_DIR)
  get_target_property(h264_links ${h264_library} INTERFACE_LINK_LIBRARIES)
  if("$ENV{PET_P4_CHIP_FAMILY}" STREQUAL "v1")
    set(h264_original "${h264_directory}/sw/libs/esp32p4_less_v3/libtinyh264.a")
    set(h264_link_entry "${h264_original}")
  else()
    # The component's IMPORTED target is directory-local (not GLOBAL).
    # Resolve the reviewed P4 v3 archive without querying that hidden target.
    set(h264_original "${h264_directory}/sw/libs/esp32p4/libtinyh264.a")
    set(h264_link_entry tinyh264)
  endif()
  if(NOT h264_link_entry IN_LIST h264_links OR NOT EXISTS "${h264_original}")
    message(FATAL_ERROR "Cannot locate the selected tinyH264 archive for SIMD admission")
  endif()
  set(h264_patcher "${CMAKE_CURRENT_LIST_DIR}/patch_h264_simd.py")
  # PIO drops CMake libraries under its build directory as presumed component
  # outputs. Keep this private prebuilt in the ignored project output area,
  # outside both managed_components and .pio/build (also for staged builds).
  get_filename_component(h264_build_profile "${CMAKE_BINARY_DIR}" NAME)
  set(h264_output_dir "${CMAKE_CURRENT_LIST_DIR}/../.pio/h264-simd/${h264_build_profile}")
  file(MAKE_DIRECTORY "${h264_output_dir}")
  get_filename_component(h264_output_dir "${h264_output_dir}" REALPATH)
  set(h264_fixed "${h264_output_dir}/libtinyh264_simd.a")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${h264_patcher}" "${h264_original}")
  execute_process(COMMAND "${PYTHON}" "${h264_patcher}"
    --source "${h264_original}" --output "${h264_fixed}"
    --ar "${CMAKE_AR}" --nm "${CMAKE_NM}" --objcopy "${CMAKE_OBJCOPY}"
    RESULT_VARIABLE h264_simd_status)
  if(NOT h264_simd_status EQUAL 0)
    message(FATAL_ERROR "Cannot constrain tinyH264 allocations to SIMD-capable heap")
  endif()
  list(FIND h264_links "${h264_link_entry}" h264_link_index)
  list(REMOVE_AT h264_links ${h264_link_index})
  list(INSERT h264_links ${h264_link_index} "${h264_fixed}")
  set_property(TARGET ${h264_library} PROPERTY INTERFACE_LINK_LIBRARIES "${h264_links}")
  idf_component_get_property(h264_app main COMPONENT_LIB)
  target_sources(${h264_app} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../main/pet_p4_h264_simd_alloc.c")
  set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/../main/pet_p4_h264_simd_alloc.c"
    PROPERTIES COMPILE_OPTIONS "-Os")
  message(STATUS "tinyH264 SIMD heap admission: ${h264_original} -> ${h264_fixed}")
endif()
