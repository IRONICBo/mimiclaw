# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# MicroPython (component manager: mr9you/micropython-helper)
set(MICROPY_HELPER_DIR "${CMAKE_CURRENT_LIST_DIR}/managed_components/mr9you__micropython-helper")
if(NOT MICROPY_BOARD)
  set(MICROPY_BOARD "ESP32_GENERIC_S3")
endif()
if(NOT DEFINED MICROPY_PY_BTREE)
  set(MICROPY_PY_BTREE OFF)
endif()
if(EXISTS "${MICROPY_HELPER_DIR}/mpy.cmake")
  include(${MICROPY_HELPER_DIR}/mpy.cmake)
endif()
if(EXISTS "${CMAKE_BINARY_DIR}/sdkconfig.combined")
  file(READ "${CMAKE_BINARY_DIR}/sdkconfig.combined" _sc)
  string(REPLACE "CONFIG_MBEDTLS_SSL_ALPN=n" "CONFIG_MBEDTLS_SSL_ALPN=y" _sc "${_sc}")
  string(REPLACE "CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=n" "CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y" _sc "${_sc}")
  file(WRITE "${CMAKE_BINARY_DIR}/sdkconfig.combined" "${_sc}")
  file(APPEND "${CMAKE_BINARY_DIR}/sdkconfig.combined"
    "\nCONFIG_MBEDTLS_SSL_ALPN=y\nCONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y\n")
endif()
set(_mimi_sdkconfig_defaults
  "sdkconfig.defaults"
  "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.esp32s3"
  "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.micropython.override"
)
if(EXISTS "${CMAKE_BINARY_DIR}/sdkconfig.combined")
  list(INSERT _mimi_sdkconfig_defaults 1 "${CMAKE_BINARY_DIR}/sdkconfig.combined")
endif()
set(SDKCONFIG_DEFAULTS "${_mimi_sdkconfig_defaults}" CACHE STRING "Kconfig defaults" FORCE)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mimiclaw)

# Pre-flash a valid SPIFFS image so first boot does not need runtime formatting.
spiffs_create_partition_image(spiffs spiffs_data FLASH_IN_PROJECT)
