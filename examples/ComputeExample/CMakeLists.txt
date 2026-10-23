add_executable(ComputeExample ComputeExample.cpp)
target_link_libraries(ComputeExample PRIVATE RealRHI)
rhi_copy_directory_to_target_output(
    ComputeExample
    ${CMAKE_SOURCE_DIR}/examples/ComputeExample/shaders
    shaders
)
