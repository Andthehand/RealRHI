#pragma once
#include <cstdint>

namespace RealRHI {
    enum class BufferUsage : uint32_t {
        None = 0,
        Vertex = 1 << 0,
        Index = 1 << 1,
        Uniform = 1 << 2,
        Storage = 1 << 3,
        TransferSrc = 1 << 4,
        TransferDst = 1 << 5,
        Indirect = 1 << 6,
    };

    enum class MemoryUsage {
        GPUOnly,        // Device local
        CPUToGPU,       // Upload buffer
        GPUToCPU,       // Readback buffer
    };

    struct BufferDesc {
        uint64_t size = 0;

        BufferUsage usage = BufferUsage::None;
        MemoryUsage memoryUsage = MemoryUsage::GPUOnly;

        const void* initialData = nullptr;
    };
}