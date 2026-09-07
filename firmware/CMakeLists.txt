# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ebike)

target_sources(app PRIVATE
  src/main.c
  src/modules/geofence.c
  src/modules/assistance.c
  src/modules/location.c
  src/modules/storage.c
  src/modules/speed.c
  src/modules/sensor.c
  src/modules/connectivity.c
  src/events/channels.c
)

target_include_directories(app PRIVATE src)
