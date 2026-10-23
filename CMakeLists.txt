cmake_minimum_required(VERSION 3.15)

project(RealRHI VERSION 1.0.0 LANGUAGES CXX)

include(FetchContent)

# Options
option(BUILD_EXAMPLES "Build example applications" ON)
option(REALRHI_BUILD_TESTS "Build RealRHI tests" ON)
if(REALRHI_BUILD_TESTS)
    enable_testing()
endif()

if(WIN32)
    set(VOLK_STATIC_DEFINES VK_USE_PLATFORM_WIN32_KHR)
endif()

FetchContent_Declare(volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_TAG 0dc3ce00bf98b9f0b6fe708ca0f7eb74e2830173
)
FetchContent_MakeAvailable(volk)

find_library(Slang_LIBRARY NAMES slang slang-compiler HINTS "$ENV{VULKAN_SDK}/lib" REQUIRED)
find_library(SDL_LIBRARY NAMES SDL3 HINTS "$ENV{VULKAN_SDK}/lib" REQUIRED)

# Create RealRHI library
set(REALRHI_HEADERS
    "src/CommandAllocator.h"
    "src/BarrierDesc.h"
    "src/DeferredDeletionQueue.h"
    "src/FrameResources.h"
    "src/Vulkan/VulkanCommandAllocator.h"
    "src/Vulkan/VulkanDevice.h"
    "src/Queue.h"

  "src/Swapchain.h" "src/Texture.h" "src/Buffer.h" "src/Pipeline.h" "src/CommandList.h" "src/Device.h" "src/BufferDesc.h" "src/PipelineDesc.h" "src/TextureDesc.h" "src/DeviceDesc.h" "src/Vulkan/VulkanSwapchain.h" "src/SwapchainDesc.h"   "src/Shader.h" "src/Vulkan/VulkanShader.h" "src/Window.h" "src/Vulkan/VulkanWindow.h" "src/WindowDesc.h" "src/Vulkan/VulkanPipeline.h"  "src/Vulkan/VulkanConvertions.h" "src/TextureFormat.h" "src/ShaderDesc.h" "src/Vulkan/VulkanBuffer.h" "src/RenderingInfo.h" "src/Vulkan/VulkanCommandList.h" "src/Vulkan/VulkanCommandQueue.h" "src/TextureView.h" "src/Vulkan/VulkanTexture.h" "src/Vulkan/VulkanTextureView.h" "src/TextureViewDesc.h" "src/FrameContext.h" "src/RefCounted.h" "src/Vulkan/DescriptorsDesc.h" "src/Vulkan/VulkanDescriptorHeaps.h" "src/Vulkan/VulkanDescriptorManager.h")

set(REALRHI_SOURCES
    "src/Vulkan/VulkanCommandAllocator.cpp"
    "src/Vulkan/VulkanDevice.cpp"
    "src/Vulkan/VulkanCommandQueue.cpp"

  "src/Vulkan/VulkanSwapchain.cpp" "src/Vulkan/VulkanShader.cpp" "src/Vulkan/VulkanWindow.cpp" "src/Vulkan/VulkanPipeline.cpp" "src/Vulkan/VulkanBuffer.cpp" "src/Vulkan/VulkanCommandList.cpp" "src/Vulkan/VulkanTexture.cpp" "src/Vulkan/VulkanTextureView.cpp" "src/Vulkan/VulkanDescriptorHeaps.cpp" "src/Vulkan/VulkanDescriptorManager.cpp")
    
add_library(RealRHI ${REALRHI_SOURCES} ${REALRHI_HEADERS})
if(WIN32)
    target_compile_definitions(RealRHI PUBLIC NOMINMAX)
endif()
    
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

if(REALRHI_BUILD_TESTS)
    add_executable(CoreTests tests/CoreTests.cpp)
    target_link_libraries(CoreTests PRIVATE RealRHI)
    add_test(NAME CoreTests COMMAND CoreTests)

    add_executable(ResourceSafetyTests tests/ResourceSafetyTests.cpp)
    target_link_libraries(ResourceSafetyTests PRIVATE RealRHI)
    target_compile_definitions(ResourceSafetyTests PRIVATE
        REALRHI_TEST_SHADER_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/examples/TriangleExample/shaders")
    add_test(NAME ResourceSafetyTests COMMAND ResourceSafetyTests)

    add_executable(QueueSubmissionTests tests/QueueSubmissionTests.cpp)
    target_link_libraries(QueueSubmissionTests PRIVATE RealRHI)
    target_include_directories(QueueSubmissionTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME QueueSubmissionTests COMMAND QueueSubmissionTests)
endif()

# Add examples directory if enabled
if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
