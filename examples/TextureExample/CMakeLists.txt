add_executable(TextureExample
    TextureExample.cpp
)

target_link_libraries(TextureExample
    PRIVATE
        RealRHI
        ${SDL_LIBRARY}
)

rhi_copy_directory_to_target_output(
  TextureExample
  ${CMAKE_SOURCE_DIR}/examples/TextureExample/shaders
  shaders
)
