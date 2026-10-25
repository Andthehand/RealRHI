#pragma once
#include "BufferDesc.h"
#include "PipelineDesc.h"
#include "RenderingInfo.h"
#include "Shader.h"
#include "TextureDesc.h"
#include "TextureFormat.h"
#include "TextureViewDesc.h"

#include <volk.h>

#include <vma/vk_mem_alloc.h>

namespace RealRHI::Utils {
	constexpr VkShaderStageFlagBits ShaderStageToVkShaderStage(ShaderStage stage) {
		switch (stage) {
			case ShaderStage::Vertex:
				return VK_SHADER_STAGE_VERTEX_BIT;
			case ShaderStage::Fragment:
				return VK_SHADER_STAGE_FRAGMENT_BIT;
			case ShaderStage::Compute:
				return VK_SHADER_STAGE_COMPUTE_BIT;
			default:
				return static_cast<VkShaderStageFlagBits>(0);
		}
	}
	struct VertexFormatInfo {
		VkFormat format = VK_FORMAT_UNDEFINED;
		ShaderScalarType scalarType = ShaderScalarType::Float32;
		uint32_t components = 0;
		uint32_t size = 0;
	};
	constexpr VertexFormatInfo GetVertexFormatInfo(VertexFormat format) {
		switch (format) {
			case VertexFormat::Float:
				return {VK_FORMAT_R32_SFLOAT, ShaderScalarType::Float32, 1, 4};
			case VertexFormat::Float2:
				return {VK_FORMAT_R32G32_SFLOAT, ShaderScalarType::Float32, 2, 8};
			case VertexFormat::Float3:
				return {VK_FORMAT_R32G32B32_SFLOAT, ShaderScalarType::Float32, 3, 12};
			case VertexFormat::Float4:
				return {VK_FORMAT_R32G32B32A32_SFLOAT, ShaderScalarType::Float32, 4, 16};
			case VertexFormat::Int:
				return {VK_FORMAT_R32_SINT, ShaderScalarType::Int32, 1, 4};
			case VertexFormat::Int2:
				return {VK_FORMAT_R32G32_SINT, ShaderScalarType::Int32, 2, 8};
			case VertexFormat::Int3:
				return {VK_FORMAT_R32G32B32_SINT, ShaderScalarType::Int32, 3, 12};
			case VertexFormat::Int4:
				return {VK_FORMAT_R32G32B32A32_SINT, ShaderScalarType::Int32, 4, 16};
			case VertexFormat::UInt:
				return {VK_FORMAT_R32_UINT, ShaderScalarType::UInt32, 1, 4};
			case VertexFormat::UInt2:
				return {VK_FORMAT_R32G32_UINT, ShaderScalarType::UInt32, 2, 8};
			case VertexFormat::UInt3:
				return {VK_FORMAT_R32G32B32_UINT, ShaderScalarType::UInt32, 3, 12};
			case VertexFormat::UInt4:
				return {VK_FORMAT_R32G32B32A32_UINT, ShaderScalarType::UInt32, 4, 16};
			case VertexFormat::UNorm8x4:
				return {VK_FORMAT_R8G8B8A8_UNORM, ShaderScalarType::Float32, 4, 4};
			case VertexFormat::SNorm8x4:
				return {VK_FORMAT_R8G8B8A8_SNORM, ShaderScalarType::Float32, 4, 4};
			default:
				return {};
		}
	}

	constexpr VkAttachmentLoadOp LoadOpToVkLoadOp(LoadOp loadOp) {
		switch (loadOp) {
			case RealRHI::LoadOp::Load:
				return VK_ATTACHMENT_LOAD_OP_LOAD;
			case RealRHI::LoadOp::Clear:
				return VK_ATTACHMENT_LOAD_OP_CLEAR;
			case RealRHI::LoadOp::DontCare:
				return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			default:
				return VK_ATTACHMENT_LOAD_OP_MAX_ENUM;
		}
	}

	constexpr VkAttachmentStoreOp StoreOpToVkStoreOp(StoreOp storeOp) {
		switch (storeOp) {
			case RealRHI::StoreOp::Store:
				return VK_ATTACHMENT_STORE_OP_STORE;
			case RealRHI::StoreOp::DontCare:
				return VK_ATTACHMENT_STORE_OP_DONT_CARE;
			default:
				return VK_ATTACHMENT_STORE_OP_MAX_ENUM;
		}
	}

	constexpr VkBufferUsageFlags BufferUsageToVkBufferUsage(BufferUsage usage) {
		VkBufferUsageFlags flags = 0;
		if (Any(usage & BufferUsage::Vertex)) {
			flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
		}
		if (Any(usage & BufferUsage::Index)) {
			flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
		}
		if (Any(usage & BufferUsage::Uniform)) {
			flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
		}
		if (Any(usage & BufferUsage::Storage)) {
			flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
		}
		if (Any(usage & BufferUsage::TransferSrc)) {
			flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		}
		if (Any(usage & BufferUsage::TransferDst)) {
			flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		}
		if (Any(usage & BufferUsage::Indirect)) {
			flags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
		}
		if (Any(usage & BufferUsage::Heap)) {
			flags |= VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT;
		}
		if (Any(usage & (BufferUsage::Storage | BufferUsage::Uniform | BufferUsage::Addressable))) {
			flags |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
		}
		return flags;
	}

	constexpr VkDescriptorType BufferUsageToVkDescriptorType(BufferUsage usage) {
		if (Any(usage & BufferUsage::Uniform)) {
			return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		}
		if (Any(usage & BufferUsage::Storage)) {
			return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		}
		return VK_DESCRIPTOR_TYPE_MAX_ENUM;
	}

	constexpr VmaAllocationCreateInfo MemoryUsageToVmaAllocationCreateInfo(MemoryUsage usage) {
		VmaAllocationCreateInfo allocInfo{
			.usage = VMA_MEMORY_USAGE_AUTO,
		};

		switch (usage) {
			case MemoryUsage::GPUOnly:
				break;
			case MemoryUsage::CPUToGPU:
				allocInfo.flags =
					VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
				break;
			case MemoryUsage::GPUToCPU:
				allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
				break;
		}
		return allocInfo;
	}

	constexpr VkImageViewType TextureViewTypeToVkImageViewType(TextureViewType type) {
		switch (type) {
			case RealRHI::TextureViewType::View1D:
				return VK_IMAGE_VIEW_TYPE_1D;
			case RealRHI::TextureViewType::View2D:
				return VK_IMAGE_VIEW_TYPE_2D;
			case RealRHI::TextureViewType::View2DArray:
				return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
			case RealRHI::TextureViewType::View3D:
				return VK_IMAGE_VIEW_TYPE_3D;
			case RealRHI::TextureViewType::Cube:
				return VK_IMAGE_VIEW_TYPE_CUBE;
			case RealRHI::TextureViewType::CubeArray:
				return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
			default:
				return VK_IMAGE_VIEW_TYPE_MAX_ENUM;
		}
	}

	constexpr VkImageUsageFlags TextureUsageToVkImageUsage(TextureUsage usage) {
		VkImageUsageFlags flags = 0;
		if (Any(usage & TextureUsage::ShaderResource)) {
			flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
		}
		if (Any(usage & TextureUsage::RenderTarget)) {
			flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		}
		if (Any(usage & TextureUsage::DepthStencil)) {
			flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
		}
		if (Any(usage & TextureUsage::Storage)) {
			flags |= VK_IMAGE_USAGE_STORAGE_BIT;
		}
		if (Any(usage & TextureUsage::TransferSrc)) {
			flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		}
		if (Any(usage & TextureUsage::TransferDst)) {
			flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		}
		return flags;
	}

	constexpr VkImageType TextureDescToVkImageType(const TextureDesc& desc) {
		if (desc.depth > 1) {
			return VK_IMAGE_TYPE_3D;
		}
		return VK_IMAGE_TYPE_2D;
	}

	// Depth/stencil formats require explicit copy aspects and are unsupported here.
	constexpr uint32_t TextureFormatByteSize(TextureFormat format) {
		switch (format) {
			case TextureFormat::R8_UNorm:
			case TextureFormat::R8_SNorm:
				return 1;
			case TextureFormat::RG8_UNorm:
			case TextureFormat::RG8_SNorm:
			case TextureFormat::R16_UNorm:
			case TextureFormat::R16_SNorm:
			case TextureFormat::R16_Float:
				return 2;
			case TextureFormat::RGBA8_UNorm:
			case TextureFormat::RGBA8_SRGB:
			case TextureFormat::RGBA8_SNorm:
			case TextureFormat::RGBA8_UInt:
			case TextureFormat::RGBA8_SInt:
			case TextureFormat::BGRA8_UNorm:
			case TextureFormat::BGRA8_SRGB:
			case TextureFormat::RG16_UNorm:
			case TextureFormat::RG16_SNorm:
			case TextureFormat::RG16_Float:
			case TextureFormat::R32_Float:
			case TextureFormat::R32_UInt:
			case TextureFormat::R32_SInt:
			case TextureFormat::RGB10A2_UNorm:
			case TextureFormat::RG11B10_Float:
			case TextureFormat::RGB9E5_Float:
				return 4;
			case TextureFormat::RGBA16_UNorm:
			case TextureFormat::RGBA16_SNorm:
			case TextureFormat::RGBA16_Float:
			case TextureFormat::RGBA16_UInt:
			case TextureFormat::RGBA16_SInt:
			case TextureFormat::RG32_Float:
			case TextureFormat::RG32_UInt:
			case TextureFormat::RG32_SInt:
				return 8;
			case TextureFormat::RGB32_Float:
			case TextureFormat::RGB32_UInt:
			case TextureFormat::RGB32_SInt:
				return 12;
			case TextureFormat::RGBA32_Float:
			case TextureFormat::RGBA32_UInt:
			case TextureFormat::RGBA32_SInt:
				return 16;
			default:
				return 0;
		}
	}

	constexpr VkFormat TextureFormatToVkFormat(TextureFormat format) {
		switch (format) {
			case RealRHI::TextureFormat::RGBA8_UNorm:
				return VK_FORMAT_R8G8B8A8_UNORM;
			case RealRHI::TextureFormat::RGBA8_SRGB:
				return VK_FORMAT_R8G8B8A8_SRGB;
			case RealRHI::TextureFormat::RGBA8_SNorm:
				return VK_FORMAT_R8G8B8A8_SNORM;
			case RealRHI::TextureFormat::RGBA8_UInt:
				return VK_FORMAT_R8G8B8A8_UINT;
			case RealRHI::TextureFormat::RGBA8_SInt:
				return VK_FORMAT_R8G8B8A8_SINT;
			case RealRHI::TextureFormat::BGRA8_UNorm:
				return VK_FORMAT_B8G8R8A8_UNORM;
			case RealRHI::TextureFormat::BGRA8_SRGB:
				return VK_FORMAT_B8G8R8A8_SRGB;
			case RealRHI::TextureFormat::RG8_UNorm:
				return VK_FORMAT_R8G8_UNORM;
			case RealRHI::TextureFormat::RG8_SNorm:
				return VK_FORMAT_R8G8_SNORM;
			case RealRHI::TextureFormat::R8_UNorm:
				return VK_FORMAT_R8_UNORM;
			case RealRHI::TextureFormat::R8_SNorm:
				return VK_FORMAT_R8_SNORM;
			case RealRHI::TextureFormat::RGBA16_UNorm:
				return VK_FORMAT_R16G16B16A16_UNORM;
			case RealRHI::TextureFormat::RGBA16_SNorm:
				return VK_FORMAT_R16G16B16A16_SNORM;
			case RealRHI::TextureFormat::RGBA16_Float:
				return VK_FORMAT_R16G16B16A16_SFLOAT;
			case RealRHI::TextureFormat::RGBA16_UInt:
				return VK_FORMAT_R16G16B16A16_UINT;
			case RealRHI::TextureFormat::RGBA16_SInt:
				return VK_FORMAT_R16G16B16A16_SINT;
			case RealRHI::TextureFormat::RG16_UNorm:
				return VK_FORMAT_R16G16_UNORM;
			case RealRHI::TextureFormat::RG16_SNorm:
				return VK_FORMAT_R16G16_SNORM;
			case RealRHI::TextureFormat::RG16_Float:
				return VK_FORMAT_R16G16_SFLOAT;
			case RealRHI::TextureFormat::R16_UNorm:
				return VK_FORMAT_R16_UNORM;
			case RealRHI::TextureFormat::R16_SNorm:
				return VK_FORMAT_R16_SNORM;
			case RealRHI::TextureFormat::R16_Float:
				return VK_FORMAT_R16_SFLOAT;
			case RealRHI::TextureFormat::RGBA32_Float:
				return VK_FORMAT_R32G32B32A32_SFLOAT;
			case RealRHI::TextureFormat::RGBA32_UInt:
				return VK_FORMAT_R32G32B32A32_UINT;
			case RealRHI::TextureFormat::RGBA32_SInt:
				return VK_FORMAT_R32G32B32A32_SINT;
			case RealRHI::TextureFormat::RGB32_Float:
				return VK_FORMAT_R32G32B32_SFLOAT;
			case RealRHI::TextureFormat::RGB32_UInt:
				return VK_FORMAT_R32G32B32_UINT;
			case RealRHI::TextureFormat::RGB32_SInt:
				return VK_FORMAT_R32G32B32_SINT;
			case RealRHI::TextureFormat::RG32_Float:
				return VK_FORMAT_R32G32_SFLOAT;
			case RealRHI::TextureFormat::RG32_UInt:
				return VK_FORMAT_R32G32_UINT;
			case RealRHI::TextureFormat::RG32_SInt:
				return VK_FORMAT_R32G32_SINT;
			case RealRHI::TextureFormat::R32_Float:
				return VK_FORMAT_R32_SFLOAT;
			case RealRHI::TextureFormat::R32_UInt:
				return VK_FORMAT_R32_UINT;
			case RealRHI::TextureFormat::R32_SInt:
				return VK_FORMAT_R32_SINT;
			case RealRHI::TextureFormat::RGB10A2_UNorm:
				return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
			case RealRHI::TextureFormat::RG11B10_Float:
				return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
			case RealRHI::TextureFormat::RGB9E5_Float:
				return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
			case RealRHI::TextureFormat::D32_Float:
				return VK_FORMAT_D32_SFLOAT;
			case RealRHI::TextureFormat::D24_UNorm_S8_UInt:
				return VK_FORMAT_D24_UNORM_S8_UINT;
			case RealRHI::TextureFormat::D32_Float_S8_UInt:
				return VK_FORMAT_D32_SFLOAT_S8_UINT;
			case RealRHI::TextureFormat::D16_UNorm:
				return VK_FORMAT_D16_UNORM;
			case RealRHI::TextureFormat::Unknown:
			default:
				return VK_FORMAT_UNDEFINED;
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
			case VK_FORMAT_R8G8B8A8_UNORM:
				return TextureFormat::RGBA8_UNorm;
			case VK_FORMAT_R8G8B8A8_SRGB:
				return TextureFormat::RGBA8_SRGB;
			case VK_FORMAT_R8G8B8A8_SNORM:
				return TextureFormat::RGBA8_SNorm;
			case VK_FORMAT_R8G8B8A8_UINT:
				return TextureFormat::RGBA8_UInt;
			case VK_FORMAT_R8G8B8A8_SINT:
				return TextureFormat::RGBA8_SInt;
			case VK_FORMAT_B8G8R8A8_UNORM:
				return TextureFormat::BGRA8_UNorm;
			case VK_FORMAT_B8G8R8A8_SRGB:
				return TextureFormat::BGRA8_SRGB;
			case VK_FORMAT_R8G8_UNORM:
				return TextureFormat::RG8_UNorm;
			case VK_FORMAT_R8G8_SNORM:
				return TextureFormat::RG8_SNorm;
			case VK_FORMAT_R8_UNORM:
				return TextureFormat::R8_UNorm;
			case VK_FORMAT_R8_SNORM:
				return TextureFormat::R8_SNorm;
			case VK_FORMAT_R16G16B16A16_UNORM:
				return TextureFormat::RGBA16_UNorm;
			case VK_FORMAT_R16G16B16A16_SNORM:
				return TextureFormat::RGBA16_SNorm;
			case VK_FORMAT_R16G16B16A16_SFLOAT:
				return TextureFormat::RGBA16_Float;
			case VK_FORMAT_R16G16B16A16_UINT:
				return TextureFormat::RGBA16_UInt;
			case VK_FORMAT_R16G16B16A16_SINT:
				return TextureFormat::RGBA16_SInt;
			case VK_FORMAT_R16G16_UNORM:
				return TextureFormat::RG16_UNorm;
			case VK_FORMAT_R16G16_SNORM:
				return TextureFormat::RG16_SNorm;
			case VK_FORMAT_R16G16_SFLOAT:
				return TextureFormat::RG16_Float;
			case VK_FORMAT_R16_UNORM:
				return TextureFormat::R16_UNorm;
			case VK_FORMAT_R16_SNORM:
				return TextureFormat::R16_SNorm;
			case VK_FORMAT_R16_SFLOAT:
				return TextureFormat::R16_Float;
			case VK_FORMAT_R32G32B32A32_SFLOAT:
				return TextureFormat::RGBA32_Float;
			case VK_FORMAT_R32G32B32A32_UINT:
				return TextureFormat::RGBA32_UInt;
			case VK_FORMAT_R32G32B32A32_SINT:
				return TextureFormat::RGBA32_SInt;
			case VK_FORMAT_R32G32B32_SFLOAT:
				return TextureFormat::RGB32_Float;
			case VK_FORMAT_R32G32B32_UINT:
				return TextureFormat::RGB32_UInt;
			case VK_FORMAT_R32G32B32_SINT:
				return TextureFormat::RGB32_SInt;
			case VK_FORMAT_R32G32_SFLOAT:
				return TextureFormat::RG32_Float;
			case VK_FORMAT_R32G32_UINT:
				return TextureFormat::RG32_UInt;
			case VK_FORMAT_R32G32_SINT:
				return TextureFormat::RG32_SInt;
			case VK_FORMAT_R32_SFLOAT:
				return TextureFormat::R32_Float;
			case VK_FORMAT_R32_UINT:
				return TextureFormat::R32_UInt;
			case VK_FORMAT_R32_SINT:
				return TextureFormat::R32_SInt;
			case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
				return TextureFormat::RGB10A2_UNorm;
			case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
				return TextureFormat::RG11B10_Float;
			case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
				return TextureFormat::RGB9E5_Float;
			case VK_FORMAT_D32_SFLOAT:
				return TextureFormat::D32_Float;
			case VK_FORMAT_D24_UNORM_S8_UINT:
				return TextureFormat::D24_UNorm_S8_UInt;
			case VK_FORMAT_D32_SFLOAT_S8_UINT:
				return TextureFormat::D32_Float_S8_UInt;
			case VK_FORMAT_D16_UNORM:
				return TextureFormat::D16_UNorm;
			default:
				return TextureFormat::Unknown;
		}
	}

	constexpr VkImageLayout TextureLayoutToVkImageLayout(TextureLayout layout) {
		switch (layout) {
			case TextureLayout::Undefined:
				return VK_IMAGE_LAYOUT_UNDEFINED;
			case TextureLayout::ColorAttachment:
				return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			case TextureLayout::ShaderRead:
				return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			case TextureLayout::TransferSrc:
				return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			case TextureLayout::TransferDst:
				return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			case TextureLayout::Present:
				return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
			case TextureLayout::General:
				return VK_IMAGE_LAYOUT_GENERAL;
			case TextureLayout::DepthStencilAttachment:
				return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			case TextureLayout::DepthStencilReadOnly:
				return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
			default:
				return VK_IMAGE_LAYOUT_UNDEFINED;
		}
	}

	constexpr VkPrimitiveTopology PrimitiveTopologyToVkPrimitiveTopology(PrimitiveTopology topology) {
		switch (topology) {
			case PrimitiveTopology::PointList:
				return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
			case PrimitiveTopology::LineList:
				return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
			case PrimitiveTopology::LineStrip:
				return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
			case PrimitiveTopology::TriangleList:
				return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
			case PrimitiveTopology::TriangleStrip:
				return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
			case PrimitiveTopology::TriangleFan:
				return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
			default:
				return VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
		}
	}

	constexpr VkBlendFactor BlendFactorToVkBlendFactor(BlendFactor factor) {
		switch (factor) {
			case BlendFactor::Zero:
				return VK_BLEND_FACTOR_ZERO;
			case BlendFactor::One:
				return VK_BLEND_FACTOR_ONE;
			case BlendFactor::SourceColor:
				return VK_BLEND_FACTOR_SRC_COLOR;
			case BlendFactor::OneMinusSourceColor:
				return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
			case BlendFactor::DestinationColor:
				return VK_BLEND_FACTOR_DST_COLOR;
			case BlendFactor::OneMinusDestinationColor:
				return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
			case BlendFactor::SourceAlpha:
				return VK_BLEND_FACTOR_SRC_ALPHA;
			case BlendFactor::OneMinusSourceAlpha:
				return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
			case BlendFactor::DestinationAlpha:
				return VK_BLEND_FACTOR_DST_ALPHA;
			case BlendFactor::OneMinusDestinationAlpha:
				return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
			case BlendFactor::ConstantColor:
				return VK_BLEND_FACTOR_CONSTANT_COLOR;
			case BlendFactor::OneMinusConstantColor:
				return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
			case BlendFactor::ConstantAlpha:
				return VK_BLEND_FACTOR_CONSTANT_ALPHA;
			case BlendFactor::OneMinusConstantAlpha:
				return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
			case BlendFactor::SourceAlphaSaturate:
				return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
			default:
				return VK_BLEND_FACTOR_MAX_ENUM;
		}
	}

	constexpr VkBlendOp BlendOperationToVkBlendOp(BlendOperation operation) {
		switch (operation) {
			case BlendOperation::Add:
				return VK_BLEND_OP_ADD;
			case BlendOperation::Subtract:
				return VK_BLEND_OP_SUBTRACT;
			case BlendOperation::ReverseSubtract:
				return VK_BLEND_OP_REVERSE_SUBTRACT;
			case BlendOperation::Min:
				return VK_BLEND_OP_MIN;
			case BlendOperation::Max:
				return VK_BLEND_OP_MAX;
			default:
				return VK_BLEND_OP_MAX_ENUM;
		}
	}

	constexpr VkCompareOp CompareOpToVkCompareOp(CompareOp op) {
		switch (op) {
			case CompareOp::Never:
				return VK_COMPARE_OP_NEVER;
			case CompareOp::Less:
				return VK_COMPARE_OP_LESS;
			case CompareOp::Equal:
				return VK_COMPARE_OP_EQUAL;
			case CompareOp::LessEqual:
				return VK_COMPARE_OP_LESS_OR_EQUAL;
			case CompareOp::Greater:
				return VK_COMPARE_OP_GREATER;
			case CompareOp::NotEqual:
				return VK_COMPARE_OP_NOT_EQUAL;
			case CompareOp::GreaterEqual:
				return VK_COMPARE_OP_GREATER_OR_EQUAL;
			case CompareOp::Always:
				return VK_COMPARE_OP_ALWAYS;
			default:
				return VK_COMPARE_OP_NEVER;
		}
	}

	constexpr VkCullModeFlags CullModeToVkCullMode(CullMode mode) {
		switch (mode) {
			case CullMode::None:
				return VK_CULL_MODE_NONE;
			case CullMode::Front:
				return VK_CULL_MODE_FRONT_BIT;
			case CullMode::Back:
				return VK_CULL_MODE_BACK_BIT;
			default:
				return VK_CULL_MODE_NONE;
		}
	}

	constexpr VkPolygonMode FillModeToVkPolygonMode(FillMode mode) {
		switch (mode) {
			case FillMode::Solid:
				return VK_POLYGON_MODE_FILL;
			case FillMode::Wireframe:
				return VK_POLYGON_MODE_LINE;
			default:
				return VK_POLYGON_MODE_FILL;
		}
	}

	constexpr VkFrontFace FrontFaceToVkFrontFace(bool frontCounterClockwise) {
		if (frontCounterClockwise) {
			return VK_FRONT_FACE_COUNTER_CLOCKWISE;
		}

		return VK_FRONT_FACE_CLOCKWISE;
	}
}
