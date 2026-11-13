add_executable(ImGuiExample ImGuiExample.cpp)
target_link_libraries(ImGuiExample PRIVATE RealRHIImGui)
realrhi_copy_imgui_shader(ImGuiExample)

if(REALRHI_BUILD_TESTS AND REALRHI_BUILD_GPU_TESTS)
    add_test(NAME ImGuiViewportSmoke COMMAND ImGuiExample --frames 32)
    set_tests_properties(ImGuiViewportSmoke PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:ImGuiExample>"
        LABELS "gpu;integration;example" TIMEOUT 120 RESOURCE_LOCK gpu)
endif()
