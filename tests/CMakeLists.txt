add_executable(CoreTests CoreTests.cpp)
target_link_libraries(CoreTests PRIVATE RealRHICore)
add_test(NAME CoreTests COMMAND CoreTests)
set_tests_properties(CoreTests PROPERTIES LABELS "cpu;unit" TIMEOUT 30)

add_executable(RenderGraphPlanningTests RenderGraphPlanningTests.cpp)
target_link_libraries(RenderGraphPlanningTests PRIVATE RealRHIGraph)
add_test(NAME RenderGraphPlanningTests COMMAND RenderGraphPlanningTests)
set_tests_properties(RenderGraphPlanningTests PROPERTIES LABELS "cpu;unit" TIMEOUT 30)

# Core-only builds register no Vulkan-dependent tests.
if(NOT REALRHI_BUILD_VULKAN)
    return()
endif()

function(realrhi_add_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE RealRHI)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

realrhi_add_test(ShaderTests ShaderTests.cpp)
target_compile_definitions(ShaderTests PRIVATE
    REALRHI_TEST_SHADER_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    REALRHI_OTHER_SHADER_DIRECTORY="${RealRHI_SOURCE_DIR}/examples/TriangleExample/shaders")
set_tests_properties(ShaderTests PROPERTIES LABELS "cpu;shader" TIMEOUT 120)

if(REALRHI_BUILD_GPU_TESTS)
    realrhi_add_test(RenderGraphQueueTests RenderGraphQueueTests.cpp)
    realrhi_add_test(TransientResourcePoolTests TransientResourcePoolTests.cpp)
    realrhi_add_test(RenderGraphTests RenderGraphTests.cpp)
    realrhi_add_test(CacheTests CacheTests.cpp)
    target_compile_definitions(CacheTests PRIVATE
        REALRHI_CACHE_TEST_DIRECTORY="${CMAKE_CURRENT_BINARY_DIR}/cache-test-shaders")
    realrhi_add_test(IndirectTests "${RealRHI_SOURCE_DIR}/examples/CullingExample/CullingExample.cpp")
    target_compile_definitions(IndirectTests PRIVATE REALRHI_INDIRECT_TESTS
        REALRHI_CULLING_SHADER_DIRECTORY="${RealRHI_SOURCE_DIR}/examples/CullingExample/shaders")
    realrhi_add_test(CapabilitiesTests CapabilitiesTests.cpp)
    realrhi_add_test(GraphicsStateTests GraphicsStateTests.cpp)
    realrhi_add_test(ShaderProgramTests ShaderProgramTests.cpp)
    realrhi_add_test(DiagnosticsTests DiagnosticsTests.cpp)
    realrhi_add_test(TextureTests TextureTests.cpp)
    realrhi_add_test(ComputeBackbufferTests ComputeBackbufferTests.cpp)
    target_compile_definitions(ComputeBackbufferTests PRIVATE
        REALRHI_TEST_SHADER_DIRECTORY="${RealRHI_SOURCE_DIR}/examples/ComputeBackbufferExample/shaders")
    realrhi_add_test(ResourceSafetyTests ResourceSafetyTests.cpp)
    realrhi_add_test(QueueSubmissionTests QueueSubmissionTests.cpp)
    realrhi_add_test(QueueOwnershipTests QueueOwnershipTests.cpp)
    realrhi_add_test(UploadTests UploadTests.cpp)
    realrhi_add_test(SceneTests SceneTests.cpp "${RealRHI_SOURCE_DIR}/examples/SceneExample/SceneRenderer.cpp")
    realrhi_add_test(DrawTests DrawTests.cpp)

    foreach(name IN ITEMS RenderGraphQueueTests RenderGraphTests GraphicsStateTests ShaderProgramTests DiagnosticsTests TextureTests QueueOwnershipTests DrawTests)
        target_compile_definitions(${name} PRIVATE
            REALRHI_TEST_SHADER_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/shaders")
    endforeach()
    target_compile_definitions(ResourceSafetyTests PRIVATE
        REALRHI_TEST_SHADER_DIRECTORY="${RealRHI_SOURCE_DIR}/examples/TriangleExample/shaders")
    target_compile_definitions(SceneTests PRIVATE
        REALRHI_SCENE_SHADER_DIRECTORY="${RealRHI_SOURCE_DIR}/examples/SceneExample/shaders")

    foreach(name IN ITEMS RenderGraphQueueTests TransientResourcePoolTests RenderGraphTests CacheTests IndirectTests CapabilitiesTests GraphicsStateTests ShaderProgramTests DiagnosticsTests TextureTests ComputeBackbufferTests ResourceSafetyTests QueueSubmissionTests QueueOwnershipTests UploadTests SceneTests DrawTests)
        set_tests_properties(${name} PROPERTIES LABELS "gpu;integration" TIMEOUT 120 RESOURCE_LOCK gpu)
    endforeach()
endif()
