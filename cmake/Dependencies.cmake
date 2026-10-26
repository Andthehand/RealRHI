# Vulkan SDK 1.4.363.0 is the tested dependency bundle; explicit cache paths may
# select another installation with the same required APIs and Slang capability.
find_package(Vulkan 1.4.363 REQUIRED)
find_path(Slang_INCLUDE_DIR NAMES slang/slang.h HINTS "$ENV{VULKAN_SDK}/Include")
find_path(SDL_INCLUDE_DIR NAMES SDL3/SDL.h HINTS "$ENV{VULKAN_SDK}/Include")
find_path(VMA_INCLUDE_DIR NAMES vma/vk_mem_alloc.h HINTS "$ENV{VULKAN_SDK}/Include")
find_path(SPIRV_INCLUDE_DIR NAMES spirv/unified1/spirv.h HINTS "$ENV{VULKAN_SDK}/Include")
find_library(Slang_LIBRARY NAMES slang slang-compiler HINTS "$ENV{VULKAN_SDK}/Lib")
find_library(SDL_LIBRARY NAMES SDL3 HINTS "$ENV{VULKAN_SDK}/Lib")

foreach(dependency IN ITEMS Slang_INCLUDE_DIR SDL_INCLUDE_DIR VMA_INCLUDE_DIR SPIRV_INCLUDE_DIR Slang_LIBRARY SDL_LIBRARY)
    if(NOT ${dependency})
        message(FATAL_ERROR "Missing ${dependency}. Install Vulkan SDK 1.4.363.0 with its SDL and VMA optional components, set VULKAN_SDK, or supply -D${dependency}=<path>. See README.md.")
    endif()
endforeach()

set(realrhi_dependency_includes "${Vulkan_INCLUDE_DIR};${Slang_INCLUDE_DIR};${SDL_INCLUDE_DIR};${VMA_INCLUDE_DIR};${SPIRV_INCLUDE_DIR}")
if(CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "The Vulkan configuration currently requires a native build to verify Slang's spvDescriptorHeapEXT capability. Core-only builds support cross compilation.")
endif()
try_run(realrhi_dependency_run realrhi_dependency_compile
    SOURCES "${CMAKE_CURRENT_LIST_DIR}/CheckDependencies.cpp"
    CMAKE_FLAGS "-DINCLUDE_DIRECTORIES:STRING=${realrhi_dependency_includes}"
    LINK_LIBRARIES "${Slang_LIBRARY}"
    CXX_STANDARD 20
    COMPILE_OUTPUT_VARIABLE realrhi_dependency_compile_output
    RUN_OUTPUT_VARIABLE realrhi_dependency_run_output)
if(NOT realrhi_dependency_compile)
    message(FATAL_ERROR "Required Vulkan/SDL3/VMA/Slang APIs did not compile or link. Use matching headers and libraries from Vulkan SDK 1.4.363.0.\n${realrhi_dependency_compile_output}")
endif()
if(NOT realrhi_dependency_run STREQUAL "0")
    message(FATAL_ERROR "Slang capability check failed (${realrhi_dependency_run}). Put the selected SDK's Bin directory on PATH and use a compiler supporting spvDescriptorHeapEXT.\n${realrhi_dependency_run_output}")
endif()
message(STATUS "RealRHI dependency check: ${realrhi_dependency_run_output}")
