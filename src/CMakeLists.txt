# Source-component registration for the arduino-as-component (espidf) build.
#
# PlatformIO's auto-generated template globs all of src/ and ignores the
# build_src_filter the regular (SCons) builds use, so it wrongly pulls in
# src/variants/ - per-board variant sources such as the nRF52 rak4630
# variant.cpp, which can't compile for the ESP32-S3 Supreme target. Exclude
# variants/ here; the Supreme envs select their board via the BOARD_MODEL
# macro, not variant source files. (The non-espidf envs ignore this file and
# use build_src_filter instead.)
#
# Committed deliberately so a clean checkout gets the correct source set -
# if absent, PlatformIO regenerates the unfiltered template that breaks the
# Supreme build.
FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)
list(FILTER app_sources EXCLUDE REGEX "/variants/")
idf_component_register(SRCS ${app_sources})
