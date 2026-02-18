# Find SDL3 library
# Try find_package first, then fall back to find_library with Vulkan SDK hint
find_package(SDL3 QUIET)
if(NOT SDL3_FOUND)
    find_library(SDL_LIBRARY NAMES SDL3 HINTS "$ENV{VULKAN_SDK}/lib")
    if(NOT SDL_LIBRARY)
        if(DEFINED ENV{VULKAN_SDK})
            message(FATAL_ERROR "SDL3 not found. Checked in $ENV{VULKAN_SDK}/lib. Please install SDL3 or ensure it's available via system package manager.")
        else()
            message(FATAL_ERROR "SDL3 not found. Please install SDL3 or set the VULKAN_SDK environment variable if SDL3 is bundled with your Vulkan SDK.")
        endif()
    endif()
endif()


# Add example subdirectories
add_subdirectory(BasicExample)
add_subdirectory(TriangleExample)
