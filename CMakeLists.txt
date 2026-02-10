cmake_minimum_required(VERSION 3.15)

project(RealRHI VERSION 1.0.0 LANGUAGES CXX)

# Options
option(BUILD_EXAMPLES "Build example applications" ON)

find_package(Vulkan REQUIRED)

# Create RealRHI library
set(REALRHI_HEADERS
    src/Device.h
    src/CommandBuffer.h
)

set(REALRHI_SOURCES
    src/Device.cpp
    src/CommandBuffer.cpp
)
    
add_library(RealRHI ${REALRHI_SOURCES} ${REALRHI_HEADERS})
    
target_include_directories(RealRHI
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
    
target_link_libraries(RealRHI
        Vulkan::Vulkan
)

message(STATUS "Vulkan include directories: ${Vulkan_INCLUDE_DIRS}")
    
target_compile_features(RealRHI PUBLIC cxx_std_20)
    
# Add examples directory if enabled
if(BUILD_EXAMPLES)
    # Add GLFW submodule (disable unnecessary GLFW features)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "Build the GLFW documentation" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "Build the GLFW test programs" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "Build the GLFW example programs" FORCE)
    add_subdirectory(external/glfw)
    
    add_subdirectory(examples)
endif()
