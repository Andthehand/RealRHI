cmake_minimum_required(VERSION 3.15)

project(RealRHI VERSION 1.0.0 LANGUAGES CXX)

include(FetchContent)

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

set(REALRHI_HEADERS
    "src/DeviceCapabilities.h"
    "src/VertexLayout.h"
    "src/Slang/SlangShaderCompiler.h"
    "src/Slang/SlangShaderReflection.h"
    "src/Profiling.h"
    "src/Vulkan/VulkanCpuTimer.h"
    "src/Sampler.h"
    "src/SamplerDesc.h"
    "src/Vulkan/VulkanSampler.h"
    "src/CommandAllocator.h"
    "src/BarrierDesc.h"
    "src/DeferredDeletionQueue.h"
    "src/FrameResources.h"
    "src/UploadContext.h"
    "src/Vulkan/VulkanUploadContext.h"
    "src/Vulkan/VulkanCommandAllocator.h"
    "src/Vulkan/VulkanDevice.h"
    "src/Vulkan/VulkanResourceState.h"
    "src/Queue.h"
    "src/Swapchain.h"
    "src/Texture.h"
    "src/Buffer.h"
    "src/Pipeline.h"
    "src/CommandList.h"
    "src/Device.h"
    "src/BufferDesc.h"
    "src/PipelineDesc.h"
    "src/TextureDesc.h"
    "src/DeviceDesc.h"
    "src/Vulkan/VulkanSwapchain.h"
    "src/SwapchainDesc.h"
    "src/Shader.h"
    "src/Vulkan/VulkanShader.h"
    "src/Window.h"
    "src/Vulkan/VulkanWindow.h"
    "src/WindowDesc.h"
    "src/Vulkan/VulkanPipeline.h"
    "src/Vulkan/VulkanConversions.h"
    "src/Vulkan/VulkanBarrierConversions.h"
    "src/TextureFormat.h"
    "src/ShaderDesc.h"
    "src/Vulkan/VulkanBuffer.h"
    "src/RenderingInfo.h"
    "src/Vulkan/VulkanCommandList.h"
    "src/Vulkan/VulkanCommandQueue.h"
    "src/TextureView.h"
    "src/Vulkan/VulkanTexture.h"
    "src/Vulkan/VulkanTextureView.h"
    "src/TextureViewDesc.h"
    "src/FrameContext.h"
    "src/RefCounted.h"
    "src/Vulkan/VulkanDescriptorHeaps.h"
    "src/Vulkan/VulkanDescriptorManager.h"
)

set(REALRHI_SOURCES
    "src/Slang/SlangShaderCompiler.cpp"
    "src/Slang/SlangShaderReflection.cpp"
    "src/Vulkan/VulkanSampler.cpp"
    "src/Vulkan/VulkanUploadContext.cpp"
    "src/Vulkan/VulkanCommandAllocator.cpp"
    "src/Vulkan/VulkanDevice.cpp"
    "src/Vulkan/VulkanCommandQueue.cpp"
    "src/Vulkan/VulkanSwapchain.cpp"
    "src/Vulkan/VulkanShader.cpp"
    "src/Vulkan/VulkanWindow.cpp"
    "src/Vulkan/VulkanPipeline.cpp"
    "src/Vulkan/VulkanBuffer.cpp"
    "src/Vulkan/VulkanCommandList.cpp"
    "src/Vulkan/VulkanTexture.cpp"
    "src/Vulkan/VulkanTextureView.cpp"
    "src/Vulkan/VulkanDescriptorHeaps.cpp"
    "src/Vulkan/VulkanDescriptorManager.cpp"
)

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
    function(realrhi_add_test name)
        add_executable(${name} ${ARGN})
        target_link_libraries(${name} PRIVATE RealRHI)
        add_test(NAME ${name} COMMAND ${name})
    endfunction()

    realrhi_add_test(IndirectTests examples/CullingExample/CullingExample.cpp)
    target_compile_definitions(IndirectTests PRIVATE REALRHI_INDIRECT_TESTS
        REALRHI_CULLING_SHADER_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/examples/CullingExample/shaders")
    realrhi_add_test(CapabilitiesTests tests/CapabilitiesTests.cpp)
    realrhi_add_test(PipelineStateTests tests/PipelineStateTests.cpp)
    realrhi_add_test(ShaderPipelineTests tests/ShaderPipelineTests.cpp)
    realrhi_add_test(ShaderTests tests/ShaderTests.cpp)
    target_compile_definitions(ShaderTests PRIVATE
        REALRHI_TEST_SHADER_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders"
        REALRHI_OTHER_SHADER_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/examples/TriangleExample/shaders")

    realrhi_add_test(ProfilingTests tests/ProfilingTests.cpp)
    realrhi_add_test(TextureTests tests/TextureTests.cpp)
    realrhi_add_test(CoreTests tests/CoreTests.cpp)
    realrhi_add_test(ResourceSafetyTests tests/ResourceSafetyTests.cpp)
    realrhi_add_test(QueueSubmissionTests tests/QueueSubmissionTests.cpp)
    realrhi_add_test(QueueOwnershipTests tests/QueueOwnershipTests.cpp)
    realrhi_add_test(UploadTests tests/UploadTests.cpp)
    realrhi_add_test(SceneTests tests/SceneTests.cpp examples/SceneExample/SceneRenderer.cpp)
    realrhi_add_test(DrawTests tests/DrawTests.cpp)

    foreach(name IN ITEMS PipelineStateTests ShaderPipelineTests ProfilingTests TextureTests QueueOwnershipTests DrawTests)
        target_compile_definitions(${name} PRIVATE
            REALRHI_TEST_SHADER_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders")
    endforeach()
    target_compile_definitions(ResourceSafetyTests PRIVATE
        REALRHI_TEST_SHADER_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/examples/TriangleExample/shaders")
    target_compile_definitions(SceneTests PRIVATE
        REALRHI_SCENE_SHADER_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/examples/SceneExample/shaders")

    foreach(name IN ITEMS IndirectTests CapabilitiesTests PipelineStateTests ShaderPipelineTests ProfilingTests TextureTests QueueOwnershipTests UploadTests SceneTests DrawTests)
        set_tests_properties(${name} PROPERTIES LABELS "gpu;integration" TIMEOUT 120 RESOURCE_LOCK gpu)
    endforeach()
endif()

if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
