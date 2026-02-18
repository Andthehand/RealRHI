# Find SDL3 library
find_library(SDL_LIBRARY NAMES SDL3 HINTS "$ENV{VULKAN_SDK}/lib" REQUIRED)

# Add example subdirectories
add_subdirectory(BasicExample)
add_subdirectory(TriangleExample)
