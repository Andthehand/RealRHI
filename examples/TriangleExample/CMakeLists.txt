add_executable(TriangleExample
    TriangleExample.cpp
)

target_link_libraries(TriangleExample
    PRIVATE
        RealRHI
        $<IF:$<TARGET_EXISTS:SDL3::SDL3>,SDL3::SDL3,${SDL_LIBRARY}>
)
