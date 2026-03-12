add_executable(TriangleExample
    TriangleExample.cpp
)

target_link_libraries(TriangleExample
    PRIVATE
        RealRHI
        ${SDL_LIBRARY}
)

# Copy the entire shaders directory to the output dir for TriangleExample
rhi_copy_directory_to_target_output(
  TriangleExample
  ${CMAKE_SOURCE_DIR}/examples/TriangleExample/shaders
  shaders
)