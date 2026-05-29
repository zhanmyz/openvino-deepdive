# thread_pool/ subdirectory: thread-pool related demos
# Reuses the dependencies already resolved by the parent directory's find_package
# (Threads, OpenCV, OpenVINO)

file(GLOB THREAD_POOL_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

foreach(SRC ${THREAD_POOL_SOURCES})
    get_filename_component(SAMPLE_NAME ${SRC} NAME_WE)
    add_executable(${SAMPLE_NAME} ${SRC})
    set_target_properties(${SAMPLE_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/samples/thread_pool")
    target_link_libraries(${SAMPLE_NAME} PRIVATE
        openvino::runtime
        ${OpenCV_LIBS}
        Threads::Threads)
    message(STATUS "Sample registered: ${SAMPLE_NAME}")
endforeach()
