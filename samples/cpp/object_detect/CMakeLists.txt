# object_detect/ subdirectory: test-related code

find_package(OpenVINO REQUIRED COMPONENTS Runtime)

file(GLOB OBJECT_DETECT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

foreach(SRC ${OBJECT_DETECT_SOURCES})
    get_filename_component(SAMPLE_NAME ${SRC} NAME_WE)
    add_executable(${SAMPLE_NAME} ${SRC})
    set_target_properties(${SAMPLE_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/samples/objectDetect")
    target_link_libraries(${SAMPLE_NAME} PRIVATE
        openvino::runtime
        ${OpenCV_LIBS}
        Threads::Threads)
    message(STATUS "Test registered: ${SAMPLE_NAME}")
endforeach()
