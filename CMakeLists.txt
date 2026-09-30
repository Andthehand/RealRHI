cmake_minimum_required(VERSION 3.15)

project(RealRHI VERSION 1.0.0 LANGUAGES CXX)

include(FetchContent)

# Options
option(BUILD_EXAMPLES "Build example applications" ON)

FetchContent_Declare(volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_TAG master
)
FetchContent_MakeAvailable(volk)

find_library(Slang_LIBRARY NAMES slang slang-compiler HINTS "$ENV{VULKAN_SDK}/lib" REQUIRED)
find_library(SDL_LIBRARY NAMES SDL3 HINTS "$ENV{VULKAN_SDK}/lib" REQUIRED)

# Create RealRHI library
set(REALRHI_HEADERS
    "src/Vulkan/VulkanDevice.h"
    
 "src/Swapchain.h" "src/Texture.h" "src/Buffer.h" "src/Pipeline.h" "src/CommandList.h" "src/Device.h" "src/BufferDesc.h" "src/PipelineDesc.h" "src/TextureDesc.h" "src/DeviceDesc.h" "src/Vulkan/VulkanSwapchain.h" "src/SwapchainDesc.h"   "src/Shader.h" "src/Vulkan/VulkanShader.h" "src/Window.h" "src/Vulkan/VulkanWindow.h" "src/WindowDesc.h" "src/Vulkan/VulkanPipeline.h"  "src/Vulkan/VulkanConvertions.h" "src/TextureFormat.h" "src/ShaderDesc.h" "src/Vulkan/VulkanBuffer.h" "src/RenderingInfo.h" "src/Vulkan/VulkanCommandList.h" "src/TextureView.h" "src/Vulkan/VulkanTexture.h" "src/Vulkan/VulkanTextureView.h" "src/TextureViewDesc.h" "src/FrameContext.h" "src/RefCounted.h" "src/Vulkan/DescriptorsDesc.h" "src/Vulkan/VulkanDescriptorHeaps.h")

set(REALRHI_SOURCES
    "src/Vulkan/VulkanDevice.cpp"
    
  "src/Vulkan/VulkanSwapchain.cpp" "src/Vulkan/VulkanShader.cpp" "src/Vulkan/VulkanWindow.cpp" "src/Vulkan/VulkanPipeline.cpp" "src/Vulkan/VulkanBuffer.cpp" "src/Vulkan/VulkanCommandList.cpp" "src/Vulkan/VulkanTexture.cpp" "src/Vulkan/VulkanTextureView.cpp" "src/Vulkan/VulkanDescriptorHeaps.cpp")
    
add_library(RealRHI ${REALRHI_SOURCES} ${REALRHI_HEADERS})
    
target_include_directories(RealRHI
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
    
target_link_libraries(RealRHI
        volk
        ${SDL_LIBRARY}
        ${Slang_LIBRARY}
)
    
target_compile_features(RealRHI PUBLIC cxx_std_20)

# Define platform-specific Vulkan macros
if(WIN32)
    set(VOLK_STATIC_DEFINES VK_USE_PLATFORM_WIN32_KHR)
endif()
    
# Add examples directory if enabled
if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
