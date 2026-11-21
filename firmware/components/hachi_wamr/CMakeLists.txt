if(NOT CMAKE_BUILD_EARLY_EXPANSION)
  include("${CMAKE_CURRENT_LIST_DIR}/wamr-options.cmake")
  set(WAMR_ROOT "$ENV{WAMR_ROOT}")
  file(TO_CMAKE_PATH "${WAMR_ROOT}" WAMR_ROOT)
  set(WAMR_ROOT_DIR "${WAMR_ROOT}")
  set(WAMR_BUILD_TARGET RISCV32_ILP32F)
  set(WAMR_BUILD_PLATFORM esp-idf)
  include("${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake")
  list(APPEND wamr_sources ${WAMR_RUNTIME_LIB_SOURCE} ${PLATFORM_SHARED_SOURCE})
  list(REMOVE_DUPLICATES wamr_sources)
endif()

idf_component_register(
  SRCS "hachi_runtime.c" ${wamr_sources}
  INCLUDE_DIRS "." "${IWASM_DIR}/include"
  REQUIRES pthread lwip esp_timer
)
target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-format" "-Os")
