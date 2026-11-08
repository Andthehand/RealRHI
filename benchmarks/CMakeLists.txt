add_executable(ResourceTrackingBenchmarks ResourceTrackingBenchmarks.cpp)
target_link_libraries(ResourceTrackingBenchmarks PRIVATE RealRHI)

add_executable(ResourceTrackingPatternBenchmarks ResourceTrackingPatternBenchmarks.cpp)
target_link_libraries(ResourceTrackingPatternBenchmarks PRIVATE RealRHI)

add_executable(SceneResourceBenchmarks
    SceneResourceBenchmarks.cpp
    "${RealRHI_SOURCE_DIR}/examples/SceneExample/SceneRenderer.cpp")
target_link_libraries(SceneResourceBenchmarks PRIVATE RealRHI)
target_compile_definitions(SceneResourceBenchmarks PRIVATE
    REALRHI_SCENE_SHADER_DIRECTORY="${RealRHI_SOURCE_DIR}/examples/SceneExample/shaders"
    REALRHI_BENCHMARK_CONFIGURATION="$<CONFIG>"
    REALRHI_BENCHMARK_COMPILER="${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}")
