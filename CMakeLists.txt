cmake_minimum_required(VERSION 3.15)

project(RealRHI VERSION 1.0.0 LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Find Vulkan (optional for build verification)
find_package(Vulkan)

if(Vulkan_FOUND)
    message(STATUS "Vulkan SDK found - building with Vulkan support")
    # Add source directory
    add_subdirectory(src)
    
    # Add examples directory
    add_subdirectory(examples)
else()
    message(WARNING "Vulkan SDK not found - skipping build. Please install Vulkan SDK to build the project.")
    message(WARNING "Download from: https://vulkan.lunarg.com/sdk/home")
endif()
