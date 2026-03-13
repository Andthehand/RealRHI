#pragma once
#include "TextureFormat.h"
#include "TextureDesc.h"
#include "PipelineDesc.h"
#include "TextureViewDesc.h"
#include "Shader.h"

#include <Vulkan/vulkan.h>

namespace RealRHI::Utils {
    // ---------------------- ShaderStage ------------------
    constexpr VkShaderStageFlagBits SlangStageToVkShaderStage(SlangStage stage) {
        switch (stage) {
            case SLANG_STAGE_VERTEX: return VK_SHADER_STAGE_VERTEX_BIT;
            case SLANG_STAGE_HULL: return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
            case SLANG_STAGE_DOMAIN: return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
            case SLANG_STAGE_GEOMETRY: return VK_SHADER_STAGE_GEOMETRY_BIT;
            case SLANG_STAGE_FRAGMENT: return VK_SHADER_STAGE_FRAGMENT_BIT;
            case SLANG_STAGE_COMPUTE: return VK_SHADER_STAGE_COMPUTE_BIT;
            case SLANG_STAGE_RAY_GENERATION: return VK_SHADER_STAGE_RAYGEN_BIT_KHR;
            case SLANG_STAGE_INTERSECTION: return VK_SHADER_STAGE_INTERSECTION_BIT_KHR;
            case SLANG_STAGE_ANY_HIT: return VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
            case SLANG_STAGE_CLOSEST_HIT: return VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
            case SLANG_STAGE_MISS: return VK_SHADER_STAGE_MISS_BIT_KHR;
            case SLANG_STAGE_CALLABLE: return VK_SHADER_STAGE_CALLABLE_BIT_KHR;
            case SLANG_STAGE_MESH: return VK_SHADER_STAGE_MESH_BIT_EXT;
            case SLANG_STAGE_AMPLIFICATION: return VK_SHADER_STAGE_TASK_BIT_EXT;
			default: return static_cast<VkShaderStageFlagBits>(0);
		}
    }

    constexpr VkFormat ScalarTypeToVkFormat(ScalarType type) {
        switch (type) {
            case ScalarType::Int8: return VK_FORMAT_R8_SINT;
            case ScalarType::UInt8: return VK_FORMAT_R8_UINT;
            case ScalarType::Int16: return VK_FORMAT_R16_SINT;
            case ScalarType::UInt16: return VK_FORMAT_R16_UINT;
            case ScalarType::Int32: return VK_FORMAT_R32_SINT;
            case ScalarType::UInt32: return VK_FORMAT_R32_UINT;
            case ScalarType::Int64: return VK_FORMAT_R64_SINT;
            case ScalarType::UInt64: return VK_FORMAT_R64_UINT;
            case ScalarType::Float16: return VK_FORMAT_R16_SFLOAT;
            case ScalarType::Float32: return VK_FORMAT_R32G32B32_SFLOAT;
			case ScalarType::Float64: return VK_FORMAT_R64_SFLOAT;
			case ScalarType::Vec2: return VK_FORMAT_R32G32_SFLOAT;
			case ScalarType::Vec3: return VK_FORMAT_R32G32B32_SFLOAT;
			case ScalarType::Vec4: return VK_FORMAT_R32G32B32A32_SFLOAT;
			default: return VK_FORMAT_UNDEFINED;
        }
	}

    constexpr uint8_t ScalarTypeToSizeOf(ScalarType type) {
        switch (type) {
            case ScalarType::Int8:
            case ScalarType::UInt8:
                return 1;
            case ScalarType::Int16:
            case ScalarType::UInt16:
            case ScalarType::Float16:
                return 2;
            case ScalarType::Int32:
            case ScalarType::UInt32:
            case ScalarType::Float32:
            case ScalarType::Vec2:
            case ScalarType::Vec3:
            case ScalarType::Vec4:
				return 4;
            case ScalarType::Int64:
            case ScalarType::UInt64:
            case ScalarType::Float64:
				return 8;
			default:
                return 0;
        }
	}

    constexpr ScalarType SlangVectorToRealRHIScalarType(slang::TypeReflection::ScalarType type, uint8_t elementCount) {
        // This is only used to return vec types
        switch (type) {
            case slang::TypeReflection::ScalarType::Float32:
                switch (elementCount) {
                    case 2: return ScalarType::Vec2;
                    case 3: return ScalarType::Vec3;
                    case 4: return ScalarType::Vec4;
                }
			default:
				return ScalarType::None;
		}
    }
    // ---------------------- ShaderStage ------------------

    // ---------------------- TextureView ------------------
    constexpr VkImageViewType TextureViewTypeToVkImageViewType(TextureViewType type) {
        switch (type) {
            case RealRHI::TextureViewType::View1D: return VK_IMAGE_VIEW_TYPE_1D;
            case RealRHI::TextureViewType::View2D: return VK_IMAGE_VIEW_TYPE_2D;
            case RealRHI::TextureViewType::View2DArray: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            case RealRHI::TextureViewType::View3D: return VK_IMAGE_VIEW_TYPE_3D;
            case RealRHI::TextureViewType::Cube: return VK_IMAGE_VIEW_TYPE_CUBE;
            case RealRHI::TextureViewType::CubeArray: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
            default: return VK_IMAGE_VIEW_TYPE_MAX_ENUM;
        }
    }
    // ---------------------- TextureView ------------------

    // ---------------------- RenderingInfo ----------------
    constexpr VkAttachmentLoadOp LoadOpToVkLoadOp(LoadOp loadOp) {
        switch (loadOp) {
            case RealRHI::LoadOp::Load: return VK_ATTACHMENT_LOAD_OP_LOAD;
            case RealRHI::LoadOp::Clear: return VK_ATTACHMENT_LOAD_OP_CLEAR;
            case RealRHI::LoadOp::DontCare: return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            default: return VK_ATTACHMENT_LOAD_OP_MAX_ENUM;
        }
	}

    constexpr VkAttachmentStoreOp StoreOpToVkStoreOp(StoreOp storeOp) {
        switch (storeOp) {
            case RealRHI::StoreOp::Store: return VK_ATTACHMENT_STORE_OP_STORE;
            case RealRHI::StoreOp::DontCare: return VK_ATTACHMENT_STORE_OP_DONT_CARE;
            default: return VK_ATTACHMENT_STORE_OP_MAX_ENUM;
        }
    }
    // ---------------------- RenderingInfo ---------------

    // ---------------------- BufferDesc -------------------
    constexpr VkBufferUsageFlags BufferUsageToVkBufferUsage(BufferUsage usage) {
        VkBufferUsageFlags flags = 0;
        if (Any(usage & BufferUsage::Vertex)) flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        if (Any(usage & BufferUsage::Index)) flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        if (Any(usage & BufferUsage::Uniform)) flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        if (Any(usage & BufferUsage::Storage)) flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        if (Any(usage & BufferUsage::TransferSrc)) flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (Any(usage & BufferUsage::TransferDst)) flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (Any(usage & BufferUsage::Indirect)) flags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        return flags;
	}

    constexpr VmaAllocationCreateInfo MemoryUsageToVmaAllocationCreateInfo(MemoryUsage usage) {
        VmaAllocationCreateInfo allocInfo{
            .usage = VMA_MEMORY_USAGE_AUTO,
        };

        switch (usage) {
            case MemoryUsage::GPUOnly:
                allocInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
                break;
            case MemoryUsage::CPUToGPU:
                allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
                break;
            case MemoryUsage::GPUToCPU:
                allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
                break;
        }
        return allocInfo;
    }
    // ---------------------- BufferDesc -------------------

	// ---------------------- TextureFormat ----------------
    constexpr VkImageUsageFlags TextureUsageToVkImageUsage(TextureUsage usage) {
        VkImageUsageFlags flags = 0;
        if (Any(usage & TextureUsage::ShaderResource)) flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
        if (Any(usage & TextureUsage::RenderTarget)) flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (Any(usage & TextureUsage::DepthStencil)) flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        if (Any(usage & TextureUsage::Storage)) flags |= VK_IMAGE_USAGE_STORAGE_BIT;
        if (Any(usage & TextureUsage::TransferSrc)) flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (Any(usage & TextureUsage::TransferDst)) flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        return flags;
    }

    constexpr VkImageType TextureDescToVkImageType(const TextureDesc& desc) {
        if (desc.depth > 1) return VK_IMAGE_TYPE_3D;
        if (desc.height > 1) return VK_IMAGE_TYPE_2D;
        return VK_IMAGE_TYPE_1D;
    }

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
            case RealRHI::TextureFormat::Unknown:
            default: return VK_FORMAT_UNDEFINED;
        }
	}

    constexpr VkImageAspectFlags TextureFormatToVkImageAspect(TextureFormat format) {
        switch (format) {
            case TextureFormat::D32_Float:
            case TextureFormat::D16_UNorm:
                return VK_IMAGE_ASPECT_DEPTH_BIT;
            case TextureFormat::D24_UNorm_S8_UInt:
            case TextureFormat::D32_Float_S8_UInt:
                return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
            default:
                return VK_IMAGE_ASPECT_COLOR_BIT;
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
            default: return TextureFormat::Unknown;
        }
	}
	// ---------------------- TextureFormat ----------------

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
