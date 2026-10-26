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

This runs `CoreTests`, `RenderGraphPlanningTests`, and `ShaderTests`. The planning tests cover resource hazards, deterministic scheduling, culling, stale handles, and texture initialization without creating a device. `ShaderTests` compiles shaders through Slang without creating a Vulkan device. All 16 device-creating tests carry the `gpu;integration` labels and can be run with:

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

This configuration exposes the `RealRHICore` header-only target, builds the backend-independent `RealRHIGraph` library, and runs `CoreTests` and `RenderGraphPlanningTests`. It does not build the `RealRHI` Vulkan library or examples.

## Architecture and API contracts

`RealRHICore` provides the resource/device interfaces and reference-counting, queue, and cleanup helpers. `RealRHIGraph` implements graph planning and transient resource pooling against those interfaces. `RealRHI` adds the Vulkan backend and Slang compiler and links the graph library transitively. Applications using `RealRHI` keep their existing link target; graph-only users can link `RealRHIGraph` without an SDK.

Graph scheduling indexes users by resource and groups readers by layout, avoiding comparisons between unrelated passes or compatible readers. A priority queue selects the earliest inserted ready pass, preserving deterministic execution order. Diagnostics retain every direct hazard and explicit prerequisite in their original order; graphs with many conflicting writes can still require a quadratic number of dependency edges.

The device must outlive its resources, queues, graphs holding resources, and pools. Command recordings retain referenced objects until reset; GPU completion makes a recording reusable but does not release those references. Deferred deletion processes a snapshot with completion queries and destruction callbacks outside its lock. Cleanup enqueued during processing remains available for a later call.

Within the Vulkan backend, `VulkanCommandList` owns command recording, binding state, and retained resources. `VulkanCommandTimestamps` owns timestamp queries and results; `VulkanCommandResourceTracker` owns recording-local layouts and the ordered resource log. A queue batch replays every list into one `VulkanResourceStateTransaction`, then commits submitted resource state only after native submission succeeds. The queue retains the existing submission/device lock ordering, while the command list collects timing results before publishing completion and releases retained resources outside its state lock during reset.

Queue backends implement `Submit(const SubmitInfo&)`; the single-list and list-span overloads have shared default implementations. `SubmitInfo::commandLists` and the span overload use `std::span<CommandList* const>`, allowing const arrays while keeping command-list objects mutable. Derived backends should expose the convenience overloads with `using CommandQueue::Submit`. Vulkan validates, retains, and submits the same list collection through one internal path, including swapchain transition buffers when needed.

`Buffer::WriteData` now returns `Result`, matching `ReadData`, so callers can handle mapped-copy and flush failures. Existing calls may ignore the result; custom buffer implementations must update their override's return type. The redundant Vulkan-only `CopyToMappedMemory` helper and unused compiler accessor were removed. `DeviceDesc::maxFramesInFlight` was also removed because the backend never used it: the swapchain currently uses two frame slots, while `FrameResources::Initialize` accepts an explicit slot count.

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

## Presenting a render graph

Acquire a frame, import its backbuffer, and declare that resource in the passes that render to it:

```cpp
const auto frame = swapchain->BeginFrame();
if (frame.backBufferView) {
    RenderGraph graph;
    const auto output = graph.ImportBackBuffer(*swapchain, frame);
    // Add passes declaring output as a ColorAttachment write.
    RenderGraphSubmission completion;
    const auto result = graph.Submit(*device, completion);
}
```

`Submit` waits for image availability before the first backbuffer use, transitions the image to `Present`, returns ownership to the presentation family, and presents automatically. Its final batch joins every graph queue, so `BeginFrame` safely reuses the frame slot. Diagnostics expose the acquire batch and presentation join. The scene example uses this path; offscreen graphs still use `Submit` or `Execute` as before.

One acquired frame is supported per graph. Backbuffer contents are discarded, so first use must write. Keep its automatically assigned final state. Frame acquisition IDs reject stale or repeated submissions, including after resize. Compilation/allocation failures consume no acquire semaphore and can be corrected before retrying. A failure after partial frame submission preserves completion tokens but requires rebuilding the swapchain before reusing that frame. Direct `Swapchain::Present` now returns `Result` for callers using the existing `Device::Submit` path.

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

## Stencil and multisampling

Stencil state lives in `PipelineDesc::stencilState`. Enable it with a stencil-capable depth format (`D24_UNorm_S8_UInt` or `D32_Float_S8_UInt`), then configure the front and back faces independently:

```cpp
pipelineDesc.stencilState.enable = true;
pipelineDesc.stencilState.front = {
    .passOp = StencilOp::Replace,
    .compareOp = CompareOp::Always,
    .reference = 1,
};
pipelineDesc.stencilState.back = pipelineDesc.stencilState.front;
```

Each face has fail/pass/depth-fail operations, a comparison, comparison/write masks, and a reference value. Depth and stencil attachments have separate load/store operations. For read-only aspects, use `Load` or `DontCare` and a pipeline that cannot write that aspect. `readOnlyDepth` and `readOnlyStencil` both default to false; stencil load/store defaults to Clear/Store.

Textures and pipelines default to one sample. Query `GetTextureFormatSupport(format)` for attachment sample-count masks, and use `SupportsTexture(desc)` to check the complete combination of usages and dimensions. Match the pipeline sample count to every source attachment:

```cpp
textureDesc.sampleCount = 4;
pipelineDesc.multisampleState.sampleCount = 4;
// Optional: sampleMask and alphaToCoverageEnable.

ColorAttachment color{
    .target = multisampledColor->GetTextureView(),
    .resolveTarget = singleSampleColor->GetTextureView(),
};
```

Resolve targets require matching formats, selected-mip extents and view layer counts, attachment usage, and one sample. Color resolves use averaging for floating-point/normalized formats and sample zero for integer formats. A swapchain backbuffer can be the color resolve target. Multisampled textures require one mip and depth 1; storage usage and buffer uploads/readback are unsupported. Resolve to a single-sampled texture before reading pixels back.

For depth/stencil resolves, set `DepthStencilAttachment::resolveTarget` and the desired `depthResolveMode`/`stencilResolveMode`. Check `GetCapabilities().depthStencilResolve` for supported mode masks and whether the two aspects can resolve independently. Modes default to `None`.

Declare both the source and resolve target in render graph pass resources. Color resolve targets use `ColorAttachment` layout and `ColorAttachmentOutput`/`ColorAttachmentWrite` state. Depth/stencil resolve targets use **the same stage/access state**, with `DepthStencilAttachment` layout: Vulkan performs depth/stencil resolve writes at color output. Include those writes in the source state of subsequent barriers as well. Source depth/stencil attachments continue to use early/late fragment tests and depth/stencil access.

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
