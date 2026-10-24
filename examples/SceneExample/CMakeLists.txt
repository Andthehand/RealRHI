add_executable(SceneExample SceneExample.cpp SceneRenderer.cpp)
target_link_libraries(SceneExample PRIVATE RealRHI)

rhi_copy_directory_to_target_output(
    SceneExample
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders
    shaders
)
