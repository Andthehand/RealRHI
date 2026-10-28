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

`VulkanTransferValidation` prepares native copy descriptions before recording or retaining copy resources. Buffer/image copies share checks for mip and layer bounds, extents, alignment, and capacity. Copy recording and upload batches use the same overflow-safe packed byte calculation; mip generation and uploads share final-layout usage checks. Copies require an active recording outside rendering. Buffer self-copies require disjoint byte ranges. Readback and texture-to-texture copies retain their RGBA8_UNorm restriction.

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

## Resource-tracking benchmarks

Enable the optional benchmarks in a release build:

```powershell
cmake --preset windows-release -DREALRHI_BUILD_BENCHMARKS=ON
cmake --build --preset windows-release --target ResourceTrackingBenchmarks ResourceTrackingPatternBenchmarks
./out/build/windows-release/ResourceTrackingBenchmarks.exe > resource-tracking.csv
./out/build/windows-release/ResourceTrackingPatternBenchmarks.exe > resource-tracking-patterns.csv
```

Both executables require a Vulkan device and report CPU timings in milliseconds: one warmup followed by seven measured samples, with median, minimum, and maximum. Validation and profiling are disabled. Native allocations and GPU execution are outside the timed sections; these measurements describe tracking overhead, not rendering performance.

The pattern benchmark includes already-fragmented submitted buffer state. Setup submits barriers for alternating 16-byte regions, leaving the gaps unclaimed so adjacent states cannot merge, even on devices with one queue family. Each measured transaction validates either one region near the end or the whole buffer, then discards its prediction. Timings include locking, copying the submitted range vector, validation and merging, and transaction destruction. Setup and GPU waits are excluded. The `tracked_ranges` column identifies the initial range count for these cases; zero indicates other workloads where this count is not reported.

## Resource-heavy scene benchmark

`SceneResourceBenchmarks` reuses the scene renderer offscreen at 960x640 with shadows.
It draws a fixed grid using either one shared asset set (`--shared`) or a separate
vertex buffer, index buffer, and 32x32 texture for each object. Each draw has its own
material tint; pipelines and explicitly bound samplers are shared. Sampled textures
also allocate an implicit default sampler. `--stream N` overwrites N distinct
vertex buffers and textures per frame, rotating through the asset sets. Each streamed
asset uploads 5,120 bytes. Shared mode permits at most one streamed asset per frame.

In Visual Studio, select the `windows-release` configuration (or `x64-Release` with
`CMakeSettings.json`), select **SceneResourceBenchmarks.exe** as the startup target,
and use **Start Without Debugging**. Benchmark targets are enabled in the Windows
presets and both Visual Studio configurations. Running with no arguments executes
the full comparison suite directly in C++; no external runner is needed.

The executable validates all three workloads, then rotates their order across three
repeats at 64, 256, and 768 objects. Each run has one initialization frame, 60
warmup frames, and 300 measured frames. Streaming updates 16 asset sets per frame.
Results go into `scene-resources/` beneath the working directory; the console prints
its absolute location. This includes per-frame CSVs, `summary.csv`, and
`methodology.txt` with device/build information, settings, measurement scopes, and
completion status. Run it without other GPU tests or benchmarks active.

For a shorter suite, set these command arguments in Visual Studio's debug settings:

```text
--suite --objects 64 --frames 30 --warmup 5 --repeats 1
```

`--output <directory>` changes the suite report folder. `--objects` selects one size;
otherwise the suite uses all three defaults. Suite mode always streams up to 16 asset
sets per frame. For one custom workload, use arguments such as:

```text
--objects 256 --frames 300 --warmup 60 --stream 16 --csv scene-resources.csv
```

Add `--validation` for correctness checks; validation is off by default for timing.
Every run reads back the final image outside measurement and checks for opaque output
and visible geometry. The executable returns failure on RHI/Vulkan errors, including
cleanup errors. Numeric options reject invalid ranges; object count is capped at 8,192.

The current backend reserves 1,024 sampler slots. On the RTX 4060 Laptop GPU used for
the initial baseline, 1,024 unique objects fail initialization with
`AllocateSamplerDescriptor: Sampler descriptor heap is full.` Each sampled object
texture consumes a default sampler even though the shader uses a shared explicit
sampler. The default sweep therefore stops at 768. CSVs include sampler usage and
capacity so that this limit remains visible; larger workloads may require sampler
reuse or optional default-sampler creation in the RHI.

CSV timings are CPU milliseconds per frame. `record_total_ms` includes graph building,
compilation, allocation, recording, and destruction. The three graph columns separate
compilation, allocation/pool lookup/alias planning/barrier resolution, and pass recording.
`queue_submit_ms` covers render and streaming submissions; `state_validate_ms` measures
resource-log replay after the queue/device locks, excluding commit, and `native_submit_ms`
measures the native queue call. Those two columns are nested within submission time.
`stream_cpu_ms` includes upload staging and submission, so categories must not be summed.
`frame_cpu_ms` excludes the separately reported GPU completion wait. Profiling and GPU
timestamps are enabled equally for all workloads.

Live and peak memory columns report native VMA allocation sizes, including internal
descriptor heaps and staging. Peaks include initialization; pool memory is already part
of the buffer/texture totals. These do not measure process RAM or all driver memory.
Frames wait for GPU completion before the next frame can overwrite assets. This makes
the benchmark suitable for CPU/resource scaling comparisons; it does not measure
presentation, frames-in-flight overlap, or establish GPU/FPS speedup.

`RenderGraph::Execute` accepts an optional `RenderGraphExecutionTimings*` after its pool
argument. It measures the execution phases without enabling device profiling. Passing
no pointer avoids its clock reads. Device profiling exposes `resourceStateValidation`
and `nativeSubmissions` alongside the existing inclusive `submissions` statistics.

The initial baseline uses MSVC RelWithDebInfo and an NVIDIA GeForce RTX 4060 Laptop
GPU. Values below average the three run means (900 measured frames per row). Raw
per-run summaries from that earlier measurement remain under
`out/test-results/scene-resources/` when available locally. The table preserves those
initial results; the built-in suite writes fresh reports to its selected output folder.

| Objects | Assets | CPU/frame ms | Graph compile ms | Pass recording ms | State validation ms |
| --- | --- | ---: | ---: | ---: | ---: |
| 64 | Shared | 0.175 | 0.012 | 0.103 | 0.010 |
| 64 | Unique | 0.522 | 0.203 | 0.169 | 0.028 |
| 64 | Unique + streaming | 0.629 | 0.202 | 0.164 | 0.036 |
| 256 | Shared | 0.271 | 0.011 | 0.191 | 0.027 |
| 256 | Unique | 3.436 | 2.254 | 0.517 | 0.125 |
| 256 | Unique + streaming | 3.381 | 2.137 | 0.480 | 0.128 |
| 768 | Shared | 0.609 | 0.012 | 0.472 | 0.073 |
| 768 | Unique | 35.877 | 29.525 | 3.700 | 0.742 |
| 768 | Unique + streaming | 38.329 | 31.404 | 3.840 | 0.791 |

At 768 unique objects, graph compilation accounts for about 82% of measured CPU
work. The three run means range from 34.884 to 36.893 ms per frame; streaming ranges
from 35.261 to 39.916 ms. Small differences between variants can reflect run variance.
Live buffer plus texture allocations are about 43.08 MiB shared, 46.96 MiB unique,
and 50.96 MiB unique with streaming; the transient pool stays at 8.69 MiB. Streaming
uploads 81,920 bytes per measured frame. The unique variants use 773/1,024 sampler
slots, compared with six for shared assets.

Graph validation now groups imports by resource identity. Buffer imports use ordered
intervals and check only their insertion neighbors; texture imports compare mip/layer
ranges only within the same texture. Pass declarations use dense resource indices with
a per-pass marker to reject duplicates without clearing or searching a growing list.
Validation still visits resources and declarations in their original order and reports
the same first error. Many disjoint imports of one texture can still require pairwise
range checks within that texture's group.

Fresh comparisons of saved baseline and optimized executables used three paired runs,
alternating binary order, with 60 warmup and 300 measured frames per workload per run.
Both variants use the same built-in C++ suite and settings. Each row below averages
900 measured frames per variant; these numbers supersede the earlier baseline for
assessing this change. Absolute timings differ from the initial run, so comparisons
use the freshly measured baseline rather than combining separate measurement sessions.

| Objects | Assets | CPU ms before → after | Compile ms before → after |
| --- | --- | ---: | ---: |
| 64 | Shared | 0.106 → 0.113 | 0.008 → 0.009 |
| 64 | Unique | 0.389 → 0.298 | 0.172 → 0.080 |
| 64 | Unique + streaming | 0.479 → 0.440 | 0.174 → 0.094 |
| 256 | Shared | 0.206 → 0.243 | 0.008 → 0.010 |
| 256 | Unique | 2.545 → 1.032 | 1.793 → 0.299 |
| 256 | Unique + streaming | 2.641 → 1.082 | 1.802 → 0.293 |
| 768 | Shared | 0.508 → 0.496 | 0.009 → 0.009 |
| 768 | Unique | 16.577 → 3.502 | 13.870 → 0.930 |
| 768 | Unique + streaming | 16.646 → 3.678 | 13.918 → 0.972 |

At 768 unique objects, compilation is 93.3% lower and total measured CPU work is
78.9% lower. Shared workloads show little compilation benefit and mixed CPU results,
including a 0.037 ms increase at 256 objects. All 39 paired final-image checksums
(including validation probes) match exactly, with no validation errors or warnings.
The full Debug and Release suites pass, including SDK-independent checks for buffer
interval boundaries, texture mip/layer overlap, and large pass declaration sets.
Detailed comparison CSVs, per-frame results, executable hashes and methodology are
saved locally under `out/test-results/graph-validation/`.

Sampler reuse or optional default samplers remain a separate capacity improvement
needed before the 1,024-object unique workload can run.

## Configuration options

| Option | Default | Purpose |
| --- | --- | --- |
| `REALRHI_BUILD_VULKAN` | `ON` | Build the Vulkan backend and check SDK dependencies. |
| `BUILD_EXAMPLES` | Same as Vulkan option | Build all examples. Requires the backend. |
| `REALRHI_BUILD_TESTS` | `ON` | Build and register tests. |
| `REALRHI_BUILD_GPU_TESTS` | `ON` when backend and tests are enabled | Build device-creating tests. Requires both options. |
| `REALRHI_BUILD_BENCHMARKS` | `OFF` | Build resource-tracking and scene CPU benchmarks. Requires the Vulkan backend and a device to run. |

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
