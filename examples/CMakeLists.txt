# Find SDL3 library
# Try find_package first, then fall back to find_library with Vulkan SDK hint
find_package(SDL3 QUIET)
if(NOT SDL3_FOUND)
    find_library(SDL_LIBRARY NAMES SDL3 HINTS "$ENV{VULKAN_SDK}/lib" REQUIRED)
endif()

# Add example subdirectories
add_subdirectory(BasicExample)
add_subdirectory(TriangleExample)
