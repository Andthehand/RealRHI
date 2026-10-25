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
add_subdirectory(SceneExample)
add_subdirectory(CullingExample)
