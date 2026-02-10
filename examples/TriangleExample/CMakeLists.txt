add_executable(TriangleExample
    TriangleExample.cpp
)

target_link_libraries(TriangleExample
    PRIVATE
        RealRHI
        glfw
)
