# RealRHI

A C++20 rendering hardware interface with a Vulkan backend, Slang shaders, bindless resources, and a render graph.

## Requirements

- CMake 3.25 or newer, Ninja, and Git.
- For Windows Vulkan builds: Visual Studio 2022 or newer with the Desktop development with C++ workload, using an **x64 Developer PowerShell** or **x64 Native Tools Command Prompt**.
- Vulkan SDK **1.4.363.0**, including the **SDL** and **Vulkan Memory Allocator (VMA)** optional components. The SDK installer calls the SDL component `com.lunarg.vulkan.sdl2`; this bundle supplies the SDL3 files used here.
- Set `VULKAN_SDK` to the SDK directory. Windows presets put its `Bin` directory on `PATH` for configuration, builds, and tests.

The SDK supplies Vulkan/SPIR-V headers, SDL3, VMA, and Slang. CMake checks these headers and libraries and runs a CPU-only Slang probe for `spvDescriptorHeapEXT` before fetching Volk or building the backend. Volk is fetched at commit `0dc3ce00bf98b9f0b6fe708ca0f7eb74e2830173`. No SDK downloads happen during a normal CMake configuration.

The SDK and a compatible GPU are separate requirements. Building the backend and running shader compiler tests do not require a GPU. Rendering and integration tests require a driver/device supporting the backend's required extensions, including `VK_EXT_descriptor_heap`, `VK_KHR_shader_untyped_pointers`, and `VK_KHR_maintenance5`. Device creation reports missing requirements.

## Build and test on Windows

Run these from the repository root in an x64 developer shell:

```powershell
$env:VULKAN_SDK = 'C:\VulkanSDK\1.4.363.0'
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
```

`windows-release` uses `RelWithDebInfo`; use `windows-debug` for a Debug build and its matching build/test presets. Each configuration has its own directory under `out/build/`.

To run just the tests that need no GPU after building the full backend:

```powershell
ctest --preset windows-release-cpu
```

This runs `CoreTests` and `ShaderTests`. `ShaderTests` compiles shaders through Slang without creating a Vulkan device. All 16 device-creating tests carry the `gpu;integration` labels and can be run with:

```powershell
ctest --preset windows-release-gpu
```

Every test preset prints failures, writes JUnit XML under `out/test-results/`, and fails if its filter selects no tests. GPU tests share a CTest resource lock and have a 120-second timeout. Expected-error tests deliberately print diagnostics; CTest's exit status determines success.

## Core tests without an SDK

On Windows use an x64 developer shell; on Linux use a C++20 compiler. These commands need no Vulkan SDK, Slang, SDL, VMA, GPU, or dependency fetch:

```sh
cmake --preset cpu
cmake --build --preset cpu
ctest --preset cpu
```

This configuration exposes the `RealRHICore` header-only target and builds `CoreTests`. It does not build the `RealRHI` Vulkan library or examples.

## Examples

The build copies shader assets beside each example executable. Run examples from their executable directory so relative shader paths resolve:

```powershell
Push-Location out/build/windows-release/examples/SceneExample
./SceneExample.exe --frames 6
Pop-Location

Push-Location out/build/windows-release/examples/CullingExample
./CullingExample.exe
Pop-Location
```

The other example directories are `TriangleExample`, `TextureExample`, and `ComputeExample`.

## Transient memory

The transient pool defaults to a **256 MiB soft budget**, measured using native buffer/image allocation sizes. Set a different budget at construction or with `SetBudget(bytes)`:

```cpp
TransientResourcePool pool(*device, 128ull * 1024 * 1024);
graph.SetOptions({.enableAliasing = true});
// Record or submit the graph with &pool.
```

Pool acquisitions and `Collect()` evict idle allocations in least-recently-acquired order. Active recordings, pending submissions, retained outputs, and retained texture views stay valid and can temporarily exceed the budget. Call `Collect()` after releasing completed recordings, graphs, and output references to enforce the budget. `Trim()` releases all idle entries; a zero budget retains no idle allocations after collection.

Aliasing is opt-in and also works without a pool. Identical graph-created resources with disjoint lifetimes on the same native queue can reuse one physical buffer or image. Imports, exports declared with `SetFinalState`, host-visible buffers, overlapping lifetimes, and resources used across native queues stay separate. A retained `Ref` keeps the physical allocation alive but does not preserve transient contents after its last declared use; export a resource when its contents must survive. Different resource descriptions do not share memory in this implementation.

`pool.GetStatistics()` reports resident, idle, peak, and budget bytes plus eviction counts. `graph.GetDiagnostics()` identifies physical alias groups, actual transient allocation bytes, and bytes saved by aliasing.

## Configuration options

| Option | Default | Purpose |
| --- | --- | --- |
| `REALRHI_BUILD_VULKAN` | `ON` | Build the Vulkan backend and check SDK dependencies. |
| `BUILD_EXAMPLES` | Same as Vulkan option | Build all examples. Requires the backend. |
| `REALRHI_BUILD_TESTS` | `ON` | Build and register tests. |
| `REALRHI_BUILD_GPU_TESTS` | `ON` when backend and tests are enabled | Build device-creating tests. Requires both options. |

Presets set these options explicitly. For example, a library-only build uses:

```powershell
cmake --preset windows-release -DBUILD_EXAMPLES=OFF -DREALRHI_BUILD_TESTS=OFF -DREALRHI_BUILD_GPU_TESTS=OFF
```

To use separately installed dependencies, override `Slang_INCLUDE_DIR`, `Slang_LIBRARY`, `SDL_INCLUDE_DIR`, `SDL_LIBRARY`, `VMA_INCLUDE_DIR`, and `SPIRV_INCLUDE_DIR` as appropriate. Include paths point to the parent of `slang/`, `SDL3/`, `vma/`, and `spirv/`. Use matching headers, libraries, and runtime DLLs. Put DLLs on `PATH` when configuring without the Windows presets. The Vulkan configuration currently supports native builds; the configure-time Slang executable must run on the build machine.

For an offline Vulkan build, prepopulate the pinned Volk checkout and supply `-DFETCHCONTENT_SOURCE_DIR_VOLK=<absolute-checkout-path>`. Core-only builds are offline without overrides. Keep machine-specific preset overrides in `CMakeUserPresets.json`, which is ignored by Git. Existing CMake caches retain explicitly selected options; use a separate build directory when switching configurations.

## Build references

- [LunarG Windows SDK installation and optional components](https://vulkan.lunarg.com/doc/view/latest/windows/getting_started.html)
- [Vulkan SDK downloads and published checksums](https://vulkan.lunarg.com/sdk/home)
- [CMake preset format](https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html)
