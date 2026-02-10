add_library(RealRHI
    Device.cpp
    CommandBuffer.cpp
)

target_include_directories(RealRHI
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE
        ${Vulkan_INCLUDE_DIRS}
)

target_link_libraries(RealRHI
    PUBLIC
        ${Vulkan_LIBRARIES}
)
