add_executable(layer_data_flow_demo layer_data_flow_demo.cpp)
set_target_properties(layer_data_flow_demo PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/samples")

target_link_libraries(layer_data_flow_demo PRIVATE Threads::Threads)