function(realrhi_add_example name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE RealRHI)
    add_custom_command(TARGET ${name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
            "$<TARGET_FILE_DIR:${name}>/shaders"
        COMMENT "Copying ${name} shaders"
    )
endfunction()

add_subdirectory(TriangleExample)
add_subdirectory(TextureExample)
add_subdirectory(ComputeExample)
add_subdirectory(ComputeBackbufferExample)
add_subdirectory(SceneExample)
add_subdirectory(CullingExample)
add_subdirectory(RenderGraphExample)
add_subdirectory(MultithreadedExample)
add_subdirectory(MultithreadedRenderGraphExample)
if(REALRHI_BUILD_IMGUI)
    add_subdirectory(ImGuiExample)
endif()

if(REALRHI_BUILD_TESTS AND REALRHI_BUILD_GPU_TESTS)
    foreach(name IN ITEMS MultithreadedExample MultithreadedRenderGraphExample)
        add_test(NAME ${name}Smoke COMMAND $<TARGET_FILE:${name}> --frames 12)
        set_tests_properties(${name}Smoke PROPERTIES
            WORKING_DIRECTORY "$<TARGET_FILE_DIR:${name}>"
            LABELS "gpu;integration;example"
            TIMEOUT 60
            RESOURCE_LOCK gpu)
    endforeach()
endif()
