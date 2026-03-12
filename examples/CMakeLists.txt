function(rhi_copy_directory_to_target_output target src_dir dest_subdir)
  if(NOT TARGET ${target})
    message(FATAL_ERROR "rhi_copy_directory_to_target_output: target '${target}' does not exist")
  endif()

  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${src_dir}"
            "$<TARGET_FILE_DIR:${target}>/${dest_subdir}"
    COMMENT "Copying '${src_dir}' -> $<TARGET_FILE_DIR:${target}>/${dest_subdir}"
  )
endfunction()

# Add example subdirectories
add_subdirectory(TriangleExample)
