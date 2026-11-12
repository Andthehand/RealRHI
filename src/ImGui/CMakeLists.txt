include(FetchContent)
# Applications can provide a matching imgui target before adding RealRHI.
if(NOT TARGET imgui)
    FetchContent_Declare(realrhi_imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.91.9b
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(realrhi_imgui)
    add_library(imgui STATIC
        ${realrhi_imgui_SOURCE_DIR}/imgui.cpp
        ${realrhi_imgui_SOURCE_DIR}/imgui_draw.cpp
        ${realrhi_imgui_SOURCE_DIR}/imgui_tables.cpp
        ${realrhi_imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${realrhi_imgui_SOURCE_DIR}/imgui_demo.cpp)
    target_include_directories(imgui PUBLIC ${realrhi_imgui_SOURCE_DIR})
endif()

add_library(RealRHIImGui ImGuiRenderer.cpp ImGuiRenderer.h shaders/realrhi_imgui.slang)
target_link_libraries(RealRHIImGui PUBLIC RealRHICore imgui)
target_compile_features(RealRHIImGui PUBLIC cxx_std_20)

# Copy into the application's shaderDirectory (default: shaders next to executable).
function(realrhi_copy_imgui_shader target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/shaders"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/shaders/realrhi_imgui.slang"
            "$<TARGET_FILE_DIR:${target}>/shaders/realrhi_imgui.slang")
endfunction()
