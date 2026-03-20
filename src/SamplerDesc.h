#pragma once

namespace RealRHI {
    enum class SamplerFilter {
        Linear,
        Nearest,
    };

    enum class SamplerAddressMode {
        Repeat,
        MirroredRepeat,
        ClampToEdge,
        ClampToBorder,
    };

    struct SamplerDesc {
        SamplerFilter minFilter = SamplerFilter::Linear;
        SamplerFilter magFilter = SamplerFilter::Linear;
        SamplerAddressMode addressModeU = SamplerAddressMode::Repeat;
        SamplerAddressMode addressModeV = SamplerAddressMode::Repeat;
        SamplerAddressMode addressModeW = SamplerAddressMode::Repeat;
        bool anisotropyEnable = false;
        float maxAnisotropy = 1.0f;
    };
}
