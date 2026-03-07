#pragma once
#include <cstdint>

namespace RealRHI {
    enum class Result : uint32_t {
        Success = 0,
        Failed,
        InvalidParameter,
        OutOfMemory,
    };
}
