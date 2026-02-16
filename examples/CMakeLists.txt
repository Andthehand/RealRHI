# Add GLFW submodule (disable unnecessary GLFW features)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "Build the GLFW documentation" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "Build the GLFW test programs" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "Build the GLFW example programs" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../vendor/glfw ${CMAKE_CURRENT_BINARY_DIR}/glfw)

# Add example subdirectories
add_subdirectory(BasicExample)
add_subdirectory(TriangleExample)
