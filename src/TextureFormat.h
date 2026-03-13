#pragma once

namespace RealRHI {
    enum class TextureFormat {
        // 8-bit formats
        RGBA8_UNorm,
        RGBA8_SRGB,
        RGBA8_SNorm,
        RGBA8_UInt,
        RGBA8_SInt,
        BGRA8_UNorm,
        BGRA8_SRGB,
        RG8_UNorm,
        RG8_SNorm,
        R8_UNorm,
        R8_SNorm,
        
        // 16-bit formats
        RGBA16_UNorm,
        RGBA16_SNorm,
        RGBA16_Float,
        RGBA16_UInt,
        RGBA16_SInt,
        RG16_UNorm,
        RG16_SNorm,
        RG16_Float,
        R16_UNorm,
        R16_SNorm,
        R16_Float,
        
        // 32-bit formats
        RGBA32_Float,
        RGBA32_UInt,
        RGBA32_SInt,
        RGB32_Float,
        RGB32_UInt,
        RGB32_SInt,
        RG32_Float,
        RG32_UInt,
        RG32_SInt,
        R32_Float,
        R32_UInt,
        R32_SInt,
        
        // Packed formats
        RGB10A2_UNorm,
        RG11B10_Float,
        RGB9E5_Float,
        
        // Depth/Stencil formats
        D32_Float,
        D24_UNorm_S8_UInt,
        D32_Float_S8_UInt,
        D16_UNorm,

        Unknown
    };
}