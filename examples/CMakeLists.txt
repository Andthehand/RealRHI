add_executable(BasicExample
    BasicExample.cpp
)

target_link_libraries(BasicExample
    PRIVATE
        RealRHI
)

find_package(glfw3 REQUIRED)

add_executable(TriangleExample
    TriangleExample.cpp
)

target_link_libraries(TriangleExample
    PRIVATE
        RealRHI
        glfw
)
