add_executable(MultiQueueExample MultiQueueExample.cpp)
target_link_libraries(MultiQueueExample PRIVATE RealRHI)
rhi_copy_directory_to_target_output(
    MultiQueueExample
    ${CMAKE_SOURCE_DIR}/examples/ComputeExample/shaders
    shaders
)
