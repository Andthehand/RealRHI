cmake_minimum_required(VERSION 3.15)

project(RealRHI VERSION 1.0.0 LANGUAGES CXX)

# Options
option(BUILD_EXAMPLES "Build example applications" ON)

find_package(Vulkan REQUIRED)

# Create RealRHI library
set(REALRHI_HEADERS
    "src/Vulkan/VulkanDevice.h"
    
 "src/Swapchain.h" "src/Texture.h" "src/Buffer.h" "src/Pipeline.h" "src/CommandList.h" "src/Device.h" "src/BufferDesc.h" "src/PipelineDesc.h" "src/TextureDesc.h" "src/DeviceDesc.h" "src/Vulkan/VulkanSwapchain.h" "src/SwapchainDesc.h" "src/glfw.h")

set(REALRHI_SOURCES
    "src/Vulkan/VulkanDevice.cpp"
    
  "src/Vulkan/VulkanSwapchain.cpp")
    
add_library(RealRHI ${REALRHI_SOURCES} ${REALRHI_HEADERS})
    
target_include_directories(RealRHI
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
    
target_link_libraries(RealRHI
        Vulkan::Vulkan
)
    
target_compile_features(RealRHI PUBLIC cxx_std_20)

# Define platform-specific Vulkan macros
if(WIN32)
    target_compile_definitions(RealRHI PUBLIC VK_USE_PLATFORM_WIN32_KHR NOMINMAX)
endif()
    
# Add examples directory if enabled
if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
