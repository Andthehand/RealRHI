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
    add_subdirectory(examples)
endif()
