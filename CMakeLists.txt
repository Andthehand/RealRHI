cmake_minimum_required(VERSION 3.25)

project(RealRHI VERSION 1.0.0 LANGUAGES CXX)

option(REALRHI_ENABLE_TRACY "Enable Tracy CPU profiling (requires an enabled Tracy::TracyClient)" OFF)

option(REALRHI_BUILD_VULKAN "Build the Vulkan backend" ON)
option(BUILD_EXAMPLES "Build example applications" ${REALRHI_BUILD_VULKAN})
option(REALRHI_BUILD_TESTS "Build RealRHI tests" ON)
option(REALRHI_BUILD_BENCHMARKS "Build resource-tracking and scene CPU benchmarks" OFF)

set(realrhi_gpu_tests_default OFF)
if(REALRHI_BUILD_VULKAN AND REALRHI_BUILD_TESTS)
    set(realrhi_gpu_tests_default ON)
endif()
option(REALRHI_BUILD_GPU_TESTS "Build Vulkan integration tests" ${realrhi_gpu_tests_default})

if(REALRHI_BUILD_GPU_TESTS AND (NOT REALRHI_BUILD_VULKAN OR NOT REALRHI_BUILD_TESTS))
    message(FATAL_ERROR "REALRHI_BUILD_GPU_TESTS requires REALRHI_BUILD_VULKAN and REALRHI_BUILD_TESTS. Set it OFF for core-only or tests-disabled builds.")
endif()
if(BUILD_EXAMPLES AND NOT REALRHI_BUILD_VULKAN)
    message(FATAL_ERROR "BUILD_EXAMPLES requires REALRHI_BUILD_VULKAN. Set BUILD_EXAMPLES=OFF for a core-only build.")
endif()
if(REALRHI_BUILD_BENCHMARKS AND NOT REALRHI_BUILD_VULKAN)
    message(FATAL_ERROR "REALRHI_BUILD_BENCHMARKS requires REALRHI_BUILD_VULKAN.")
endif()

add_library(RealRHICore INTERFACE)
target_include_directories(RealRHICore INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(RealRHICore INTERFACE cxx_std_20)

# Reuse the application's client, or require an installed Tracy package.
# Never download a second client or override its configuration.
if(REALRHI_ENABLE_TRACY)
    if(NOT TARGET Tracy::TracyClient)
        find_package(Tracy CONFIG REQUIRED)
    endif()
endif()

function(realrhi_configure_tracy target)
    if(REALRHI_ENABLE_TRACY)
        target_link_libraries(${target} PRIVATE Tracy::TracyClient)
        target_compile_definitions(${target} PRIVATE REALRHI_ENABLE_TRACY=1)
    endif()
endfunction()

# Graph compilation and resource pooling depend only on the RHI interfaces.
add_library(RealRHIGraph
    src/RenderGraph/RenderGraph.h
    src/RenderGraph/RenderGraph.cpp
    src/RenderGraph/RenderGraphCompiler.cpp
    src/RenderGraph/RenderGraphExecution.cpp
    src/RenderGraph/RenderGraphQueuePlanner.cpp
    src/RenderGraph/TransientResourcePool.h
    src/RenderGraph/TransientResourcePool.cpp)
target_link_libraries(RealRHIGraph PUBLIC RealRHICore)
realrhi_configure_tracy(RealRHIGraph)

# Vulkan dependencies are unnecessary for core and graph tests.
if(REALRHI_BUILD_VULKAN)
    include(cmake/Dependencies.cmake)
    include(FetchContent)
    if(WIN32)
        set(VOLK_STATIC_DEFINES VK_USE_PLATFORM_WIN32_KHR)
    endif()
    FetchContent_Declare(volk
        GIT_REPOSITORY https://github.com/zeux/volk.git
        GIT_TAG 0dc3ce00bf98b9f0b6fe708ca0f7eb74e2830173
    )
    FetchContent_MakeAvailable(volk)

    set(REALRHI_HEADERS
        "src/DeviceCapabilities.h"
        "src/VertexLayout.h"
        "src/Slang/SlangShaderCompiler.h"
        "src/Slang/SlangShaderReflection.h"
        "src/Statistics.h"
        "src/Detail/CpuProfiling.h"
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
        "src/Vulkan/VulkanResourceOperation.h"
        "src/Vulkan/VulkanResourceStateTransaction.h"
        "src/Vulkan/VulkanCommandResourceTracker.h"
        "src/Queue.h"
        "src/Swapchain.h"
        "src/Texture.h"
        "src/Buffer.h"
        "src/CommandList.h"
        "src/Device.h"
        "src/BufferDesc.h"
        "src/GraphicsState.h"
        "src/TextureDesc.h"
        "src/DeviceDesc.h"
        "src/Vulkan/VulkanSwapchain.h"
        "src/SwapchainDesc.h"
        "src/Shader.h"
        "src/Vulkan/VulkanShader.h"
        "src/Window.h"
        "src/Vulkan/VulkanWindow.h"
        "src/WindowDesc.h"
        "src/Vulkan/VulkanGraphicsState.h"
        "src/Vulkan/VulkanConversions.h"
        "src/Vulkan/VulkanBarrierConversions.h"
        "src/TextureFormat.h"
        "src/ShaderDesc.h"
        "src/Vulkan/VulkanBuffer.h"
        "src/RenderingInfo.h"
        "src/Vulkan/VulkanCommandList.h"
        "src/Vulkan/VulkanTransferValidation.h"
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
        "src/Vulkan/VulkanGraphicsState.cpp"
        "src/Vulkan/VulkanBuffer.cpp"
        "src/Vulkan/VulkanCommandList.cpp"
        "src/Vulkan/VulkanTransferValidation.cpp"
        "src/Vulkan/VulkanCommandResourceTracker.cpp"
        "src/Vulkan/VulkanResourceStateTransaction.cpp"
        "src/Vulkan/VulkanTexture.cpp"
        "src/Vulkan/VulkanTextureView.cpp"
        "src/Vulkan/VulkanDescriptorHeaps.cpp"
        "src/Vulkan/VulkanDescriptorManager.cpp"
    )

    add_library(RealRHI ${REALRHI_SOURCES} ${REALRHI_HEADERS})
    realrhi_configure_tracy(RealRHI)
    if(WIN32)
        target_compile_definitions(RealRHI PUBLIC NOMINMAX)
    endif()

    target_include_directories(RealRHI
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    target_include_directories(RealRHI SYSTEM PUBLIC
        "${Vulkan_INCLUDE_DIR}" "${Slang_INCLUDE_DIR}" "${SDL_INCLUDE_DIR}"
        "${VMA_INCLUDE_DIR}" "${SPIRV_INCLUDE_DIR}")

    target_link_libraries(RealRHI
        PUBLIC
        RealRHIGraph
        volk
        ${SDL_LIBRARY}
        ${Slang_LIBRARY}
    )

    target_compile_features(RealRHI PUBLIC cxx_std_20)

endif()

if(REALRHI_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(REALRHI_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
