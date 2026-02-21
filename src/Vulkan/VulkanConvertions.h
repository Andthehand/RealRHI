#pragma once
#include "TextureFormat.h"
#include "TextureDesc.h"
#include "PipelineDesc.h"
#include "BufferAttributes.h"

#include <Vulkan/vulkan.h>

namespace RealRHI::Utils {
    // ---------------------- ShaderStage ------------------
    constexpr VkShaderStageFlagBits ShaderStageToVkShaderStage(ShaderStage stage) {
        switch (stage) {
            case RealRHI::ShaderStage::Vertex: return VK_SHADER_STAGE_VERTEX_BIT;
            case RealRHI::ShaderStage::Fragment: return VK_SHADER_STAGE_FRAGMENT_BIT;
            case RealRHI::ShaderStage::Compute: return VK_SHADER_STAGE_COMPUTE_BIT;
            default: return VK_SHADER_STAGE_FLAG_BITS_MAX_ENUM;
        }
	}
    // ---------------------- ShaderStage ------------------

	// ---------------------- TextureFormat ----------------
    constexpr VkFormat TextureFormatToVkFormat(TextureFormat format) {
        switch (format) {
            case RealRHI::TextureFormat::RGBA8_UNorm: return VK_FORMAT_R8G8B8A8_UNORM;
            case RealRHI::TextureFormat::RGBA8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
            case RealRHI::TextureFormat::RGBA8_SNorm: return VK_FORMAT_R8G8B8A8_SNORM;
            case RealRHI::TextureFormat::RGBA8_UInt: return VK_FORMAT_R8G8B8A8_UINT;
            case RealRHI::TextureFormat::RGBA8_SInt: return VK_FORMAT_R8G8B8A8_SINT;
            case RealRHI::TextureFormat::BGRA8_UNorm: return VK_FORMAT_B8G8R8A8_UNORM;
            case RealRHI::TextureFormat::BGRA8_SRGB: return VK_FORMAT_B8G8R8A8_SRGB;
            case RealRHI::TextureFormat::RG8_UNorm: return VK_FORMAT_R8G8_UNORM;
            case RealRHI::TextureFormat::RG8_SNorm: return VK_FORMAT_R8G8_SNORM;
            case RealRHI::TextureFormat::R8_UNorm: return VK_FORMAT_R8_UNORM;
            case RealRHI::TextureFormat::R8_SNorm: return VK_FORMAT_R8_SNORM;
            case RealRHI::TextureFormat::RGBA16_UNorm: return VK_FORMAT_R16G16B16A16_UNORM;
            case RealRHI::TextureFormat::RGBA16_SNorm: return VK_FORMAT_R16G16B16A16_SNORM;
            case RealRHI::TextureFormat::RGBA16_Float: return VK_FORMAT_R16G16B16A16_SFLOAT;
            case RealRHI::TextureFormat::RGBA16_UInt: return VK_FORMAT_R16G16B16A16_UINT;
            case RealRHI::TextureFormat::RGBA16_SInt: return VK_FORMAT_R16G16B16A16_SINT;
            case RealRHI::TextureFormat::RG16_UNorm: return VK_FORMAT_R16G16_UNORM;
            case RealRHI::TextureFormat::RG16_SNorm: return VK_FORMAT_R16G16_SNORM;
            case RealRHI::TextureFormat::RG16_Float: return VK_FORMAT_R16G16_SFLOAT;
            case RealRHI::TextureFormat::R16_UNorm: return VK_FORMAT_R16_UNORM;
            case RealRHI::TextureFormat::R16_SNorm: return VK_FORMAT_R16_SNORM;
            case RealRHI::TextureFormat::R16_Float: return VK_FORMAT_R16_SFLOAT;
            case RealRHI::TextureFormat::RGBA32_Float: return VK_FORMAT_R32G32B32A32_SFLOAT;
            case RealRHI::TextureFormat::RGBA32_UInt: return VK_FORMAT_R32G32B32A32_UINT;
            case RealRHI::TextureFormat::RGBA32_SInt: return VK_FORMAT_R32G32B32A32_SINT;
            case RealRHI::TextureFormat::RGB32_Float: return VK_FORMAT_R32G32B32_SFLOAT;
            case RealRHI::TextureFormat::RGB32_UInt: return VK_FORMAT_R32G32B32_UINT;
            case RealRHI::TextureFormat::RGB32_SInt: return VK_FORMAT_R32G32B32_SINT;
            case RealRHI::TextureFormat::RG32_Float: return VK_FORMAT_R32G32_SFLOAT;
            case RealRHI::TextureFormat::RG32_UInt: return VK_FORMAT_R32G32_UINT;
            case RealRHI::TextureFormat::RG32_SInt: return VK_FORMAT_R32G32_SINT;
            case RealRHI::TextureFormat::R32_Float: return VK_FORMAT_R32_SFLOAT;
            case RealRHI::TextureFormat::R32_UInt: return VK_FORMAT_R32_UINT;
            case RealRHI::TextureFormat::R32_SInt: return VK_FORMAT_R32_SINT;
            case RealRHI::TextureFormat::RGB10A2_UNorm: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
            case RealRHI::TextureFormat::RG11B10_Float: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
            case RealRHI::TextureFormat::RGB9E5_Float: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
            case RealRHI::TextureFormat::D32_Float: return VK_FORMAT_D32_SFLOAT;
            case RealRHI::TextureFormat::D24_UNorm_S8_UInt: return VK_FORMAT_D24_UNORM_S8_UINT;
            case RealRHI::TextureFormat::D32_Float_S8_UInt: return VK_FORMAT_D32_SFLOAT_S8_UINT;
            case RealRHI::TextureFormat::D16_UNorm: return VK_FORMAT_D16_UNORM;
            case RealRHI::TextureFormat::BC1_UNorm: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
            case RealRHI::TextureFormat::BC1_SRGB: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
            case RealRHI::TextureFormat::BC2_UNorm: return VK_FORMAT_BC2_UNORM_BLOCK;
            case RealRHI::TextureFormat::BC2_SRGB: return VK_FORMAT_BC2_SRGB_BLOCK;
            case RealRHI::TextureFormat::BC3_UNorm: return VK_FORMAT_BC3_UNORM_BLOCK;
            case RealRHI::TextureFormat::BC3_SRGB: return VK_FORMAT_BC3_SRGB_BLOCK;
            case RealRHI::TextureFormat::BC4_UNorm: return VK_FORMAT_BC4_UNORM_BLOCK;
            case RealRHI::TextureFormat::BC4_SNorm: return VK_FORMAT_BC4_SNORM_BLOCK;
            case RealRHI::TextureFormat::BC5_UNorm: return VK_FORMAT_BC5_UNORM_BLOCK;
            case RealRHI::TextureFormat::BC5_SNorm: return VK_FORMAT_BC5_SNORM_BLOCK;
            case RealRHI::TextureFormat::BC6H_UFloat: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
            case RealRHI::TextureFormat::BC6H_SFloat: return VK_FORMAT_BC6H_SFLOAT_BLOCK;
            case RealRHI::TextureFormat::BC7_UNorm: return VK_FORMAT_BC7_UNORM_BLOCK;
            case RealRHI::TextureFormat::BC7_SRGB: return VK_FORMAT_BC7_SRGB_BLOCK;
            case RealRHI::TextureFormat::Unknown:
            default: return VK_FORMAT_UNDEFINED;
        }
	}

    constexpr TextureFormat VkFormatToTextureFormat(VkFormat format) {
        switch (format) {
            case VK_FORMAT_R8G8B8A8_UNORM: return TextureFormat::RGBA8_UNorm;
            case VK_FORMAT_R8G8B8A8_SRGB: return TextureFormat::RGBA8_SRGB;
            case VK_FORMAT_R8G8B8A8_SNORM: return TextureFormat::RGBA8_SNorm;
            case VK_FORMAT_R8G8B8A8_UINT: return TextureFormat::RGBA8_UInt;
            case VK_FORMAT_R8G8B8A8_SINT: return TextureFormat::RGBA8_SInt;
            case VK_FORMAT_B8G8R8A8_UNORM: return TextureFormat::BGRA8_UNorm;
            case VK_FORMAT_B8G8R8A8_SRGB: return TextureFormat::BGRA8_SRGB;
            case VK_FORMAT_R8G8_UNORM: return TextureFormat::RG8_UNorm;
            case VK_FORMAT_R8G8_SNORM: return TextureFormat::RG8_SNorm;
            case VK_FORMAT_R8_UNORM: return TextureFormat::R8_UNorm;
            case VK_FORMAT_R8_SNORM: return TextureFormat::R8_SNorm;
            case VK_FORMAT_R16G16B16A16_UNORM: return TextureFormat::RGBA16_UNorm;
            case VK_FORMAT_R16G16B16A16_SNORM: return TextureFormat::RGBA16_SNorm;
            case VK_FORMAT_R16G16B16A16_SFLOAT: return TextureFormat::RGBA16_Float;
            case VK_FORMAT_R16G16B16A16_UINT: return TextureFormat::RGBA16_UInt;
            case VK_FORMAT_R16G16B16A16_SINT: return TextureFormat::RGBA16_SInt;
            case VK_FORMAT_R16G16_UNORM: return TextureFormat::RG16_UNorm;
            case VK_FORMAT_R16G16_SNORM: return TextureFormat::RG16_SNorm;
            case VK_FORMAT_R16G16_SFLOAT: return TextureFormat::RG16_Float;
            case VK_FORMAT_R16_UNORM: return TextureFormat::R16_UNorm;
            case VK_FORMAT_R16_SNORM: return TextureFormat::R16_SNorm;
            case VK_FORMAT_R16_SFLOAT: return TextureFormat::R16_Float;
            case VK_FORMAT_R32G32B32A32_SFLOAT: return TextureFormat::RGBA32_Float;
            case VK_FORMAT_R32G32B32A32_UINT: return TextureFormat::RGBA32_UInt;
            case VK_FORMAT_R32G32B32A32_SINT: return TextureFormat::RGBA32_SInt;
            case VK_FORMAT_R32G32B32_SFLOAT: return TextureFormat::RGB32_Float;
            case VK_FORMAT_R32G32B32_UINT: return TextureFormat::RGB32_UInt;
            case VK_FORMAT_R32G32B32_SINT: return TextureFormat::RGB32_SInt;
            case VK_FORMAT_R32G32_SFLOAT: return TextureFormat::RG32_Float;
            case VK_FORMAT_R32G32_UINT: return TextureFormat::RG32_UInt;
            case VK_FORMAT_R32G32_SINT: return TextureFormat::RG32_SInt;
            case VK_FORMAT_R32_SFLOAT: return TextureFormat::R32_Float;
            case VK_FORMAT_R32_UINT: return TextureFormat::R32_UInt;
            case VK_FORMAT_R32_SINT: return TextureFormat::R32_SInt;
            case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return TextureFormat::RGB10A2_UNorm;
            case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return TextureFormat::RG11B10_Float;
            case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: return TextureFormat::RGB9E5_Float;
            case VK_FORMAT_D32_SFLOAT: return TextureFormat::D32_Float;
            case VK_FORMAT_D24_UNORM_S8_UINT: return TextureFormat::D24_UNorm_S8_UInt;
            case VK_FORMAT_D32_SFLOAT_S8_UINT: return TextureFormat::D32_Float_S8_UInt;
            case VK_FORMAT_D16_UNORM: return TextureFormat::D16_UNorm;
            case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return TextureFormat::BC1_UNorm;
            case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: return TextureFormat::BC1_SRGB;
            case VK_FORMAT_BC2_UNORM_BLOCK: return TextureFormat::BC2_UNorm;
            case VK_FORMAT_BC2_SRGB_BLOCK: return TextureFormat::BC2_SRGB;
            case VK_FORMAT_BC3_UNORM_BLOCK: return TextureFormat::BC3_UNorm;
            case VK_FORMAT_BC3_SRGB_BLOCK: return TextureFormat::BC3_SRGB;
            case VK_FORMAT_BC4_UNORM_BLOCK: return TextureFormat::BC4_UNorm;
            case VK_FORMAT_BC4_SNORM_BLOCK: return TextureFormat::BC4_SNorm;
            case VK_FORMAT_BC5_UNORM_BLOCK: return TextureFormat::BC5_UNorm;
            case VK_FORMAT_BC5_SNORM_BLOCK: return TextureFormat::BC5_SNorm;
            case VK_FORMAT_BC6H_UFLOAT_BLOCK: return TextureFormat::BC6H_UFloat;
            case VK_FORMAT_BC6H_SFLOAT_BLOCK: return TextureFormat::BC6H_SFloat;
            case VK_FORMAT_BC7_UNORM_BLOCK: return TextureFormat::BC7_UNorm;
            case VK_FORMAT_BC7_SRGB_BLOCK: return TextureFormat::BC7_SRGB;
            default: return TextureFormat::Unknown;
        }
	}
	// ---------------------- TextureFormat ----------------

    // ---------------------- BufferAttributes ----------------
    constexpr VkFormat BufferDataTypeToVkFormat(DataType type) {
        switch (type) {
            case DataType::Float:  return VK_FORMAT_R32_SFLOAT;
            case DataType::Float2: return VK_FORMAT_R32G32_SFLOAT;
            case DataType::Float3: return VK_FORMAT_R32G32B32_SFLOAT;
            case DataType::Float4: return VK_FORMAT_R32G32B32A32_SFLOAT;

            case DataType::Uint:  return VK_FORMAT_R32_UINT;
            case DataType::Uint2: return VK_FORMAT_R32G32_UINT;
            case DataType::Uint3: return VK_FORMAT_R32G32B32_UINT;
            case DataType::Uint4: return VK_FORMAT_R32G32B32A32_UINT;

            case DataType::Int:  return VK_FORMAT_R32_SINT;
            case DataType::Int2: return VK_FORMAT_R32G32_SINT;
            case DataType::Int3: return VK_FORMAT_R32G32B32_SINT;
            case DataType::Int4: return VK_FORMAT_R32G32B32A32_SINT;

            default: return VK_FORMAT_UNDEFINED;
        }
    }
    // ---------------------- BufferAttributes ----------------

    // ---------------------- DepthState ----------------------
    constexpr VkCompareOp CompareOpToVkCompareOp(CompareOp op) {
        switch (op) {
            case CompareOp::Never: return VK_COMPARE_OP_NEVER;
            case CompareOp::Less: return VK_COMPARE_OP_LESS;
            case CompareOp::Equal: return VK_COMPARE_OP_EQUAL;
            case CompareOp::LessEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
            case CompareOp::Greater: return VK_COMPARE_OP_GREATER;
            case CompareOp::NotEqual: return VK_COMPARE_OP_NOT_EQUAL;
            case CompareOp::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
            case CompareOp::Always: return VK_COMPARE_OP_ALWAYS;
            default: return VK_COMPARE_OP_NEVER;
        }
    }
    // ---------------------- DepthState ----------------------

    // ---------------------- RasterState ---------------------
    constexpr VkCullModeFlags CullModeToVkCullMode(CullMode mode) {
        switch (mode) {
            case CullMode::None: return VK_CULL_MODE_NONE;
            case CullMode::Front: return VK_CULL_MODE_FRONT_BIT;
            case CullMode::Back: return VK_CULL_MODE_BACK_BIT;
            default: return VK_CULL_MODE_NONE;
        }
    }

    constexpr VkPolygonMode FillModeToVkPolygonMode(FillMode mode) {
        switch (mode) {
            case FillMode::Solid: return VK_POLYGON_MODE_FILL;
            case FillMode::Wireframe: return VK_POLYGON_MODE_LINE;
            default: return VK_POLYGON_MODE_FILL;
        }
    }

    constexpr VkFrontFace FrontFaceToVkFrontFace(bool frontCounterClockwise) {
        return frontCounterClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    }
    // ---------------------- RasterState ---------------------
}
