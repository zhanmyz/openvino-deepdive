find_package(Threads REQUIRED)
find_package(OpenCV REQUIRED
    HINTS /usr/lib/x86_64-linux-gnu/cmake/opencv4)

# Auto-discover all sample sources: every *.cpp under this directory becomes
# a standalone executable named after the file (no extension).
# To add a new sample: drop a new .cpp file here and re-run CMake configure.
file(GLOB SAMPLE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

foreach(SAMPLE_SRC ${SAMPLE_SOURCES})
    get_filename_component(SAMPLE_NAME ${SAMPLE_SRC} NAME_WE)
    add_executable(${SAMPLE_NAME} ${SAMPLE_SRC})
    set_target_properties(${SAMPLE_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/samples")
    target_link_libraries(${SAMPLE_NAME} PRIVATE
        ${OpenCV_LIBS}
        Threads::Threads)
    message(STATUS "Sample registered: ${SAMPLE_NAME}")
endforeach()

# Subdirectories: thread-pool related demos and tests
add_subdirectory(thread_pool)
add_subdirectory(tests)

# Debug: Depend on OpenVINO Runtime to avoid "undefined reference" errors when linking
# add_subdirectory(object_detect)

# Subdirectory: pure C++ demo explaining data flow between layers
add_subdirectory(layer_data_flow_demo)
