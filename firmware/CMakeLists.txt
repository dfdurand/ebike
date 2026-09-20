# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ebike)

target_sources(app PRIVATE
  src/main.c
  src/modules/cloud.c
  src/modules/wdt.c
  src/modules/diag.c
  src/modules/geofence.c
  src/modules/assistance.c
  src/modules/location.c
  src/modules/storage.c
  src/modules/speed.c
  src/modules/status_led.c
  src/modules/sensor.c
  src/modules/connectivity.c
  src/events/channels.c
)

target_include_directories(app PRIVATE src)

# AWS credentials embedded for boot-time provisioning (development only).
# The PEM files live in certs/ (not versioned) and are converted into C arrays
# included by cloud.c.
if(CONFIG_EBIKE_PROVISION_CERTS)
  foreach(cert AmazonRootCA1.pem device-cert.pem private.key)
    if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/certs/${cert})
      message(FATAL_ERROR
        "certs/${cert} is missing: required by CONFIG_EBIKE_PROVISION_CERTS "
        "(see firmware/certs/README.md)")
    endif()
    generate_inc_file_for_target(app
      ${CMAKE_CURRENT_SOURCE_DIR}/certs/${cert}
      ${ZEPHYR_BINARY_DIR}/include/generated/${cert}.inc)
  endforeach()
endif()
