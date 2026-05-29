
cmake_minimum_required(VERSION 3.20)
project(openvino-deepdive CXX)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/bin)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# How to set CMAKE_BUILD_TYPE (pick one of the two approaches):
#
# Approach 1: hard-code it in CMakeLists.txt (just uncomment the line below):
#   set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
#
# Approach 2: pass it from the command line via -D (this project uses this for flexibility):
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug ...   -> debug build (with -g symbols, no optimization)
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release ... -> release build (-O3 optimization, no debug symbols)
#
# If neither is set, CMAKE_BUILD_TYPE is an empty string (no optimization, no debug symbols).
# The tasks.json of this project always passes -DCMAKE_BUILD_TYPE=Debug.

# Log level: Debug builds show all (DEBUG+), Release builds show INFO+
# How to override (pick one):
#   Approach A: pass it from the command line:  cmake -DLOG_MIN_LEVEL=2 ...
#   Approach B: hard-code it here (uncomment the line below):
#     set(LOG_MIN_LEVEL 2)   # 0=DEBUG, 1=INFO, 2=WARN, 3=ERROR
#
# As long as LOG_MIN_LEVEL is defined (either via -D or set()), the if-block below is skipped.
if(NOT DEFINED LOG_MIN_LEVEL)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        set(LOG_MIN_LEVEL 1)
    else()
        set(LOG_MIN_LEVEL 0)
    endif()
endif()
add_compile_definitions(LOG_MIN_LEVEL=${LOG_MIN_LEVEL})
message(STATUS "Log level: LOG_MIN_LEVEL=${LOG_MIN_LEVEL} (0=DEBUG,1=INFO,2=WARN,3=ERROR)")

# OpenVINO_DIR must be passed via cmake -DOpenVINO_DIR=<path> when using
# a source-built OpenVINO (not installed system-wide).
# Example: cmake -S . -B build -DOpenVINO_DIR=/path/to/openvino/build

add_subdirectory(samples)

