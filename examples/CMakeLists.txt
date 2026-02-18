# Find SDL3 library
# Try find_package first, then fall back to find_library with Vulkan SDK hint
find_package(SDL3 QUIET)
if(NOT SDL3_FOUND)
    find_library(SDL_LIBRARY NAMES SDL3 HINTS "$ENV{VULKAN_SDK}/lib")
    if(NOT SDL_LIBRARY)
        message(FATAL_ERROR "SDL3 not found. Please install SDL3 or ensure VULKAN_SDK environment variable is set correctly. SDL3 should be in $ENV{VULKAN_SDK}/lib or available via system package manager.")
    endif()
endif()


# Add example subdirectories
add_subdirectory(BasicExample)
add_subdirectory(TriangleExample)
