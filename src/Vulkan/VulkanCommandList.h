#pragma once
#include "CommandList.h"
#include "VulkanDevice.h"
#include "Result.h"

#include <Vulkan/vulkan.h>
#include <unordered_map>
#include <functional>

namespace RealRHI {
	class VulkanPipeline;

	class VulkanCommandList : public CommandList {
	public:
		VulkanCommandList(const VulkanDevice* device);
		~VulkanCommandList();

		static Result Create(const VulkanDevice* device, Ref<VulkanCommandList>& outCommandList);
		Result Init();

		Result Begin() override;
		Result End() override;
		
		void BeginRendering(const RenderingInfo& renderingInfo) override;
		void EndRendering() override;

		void SetViewport(const Viewport& vp);
		void SetScissor(const Rect& rect);

		void BindPipeline(Pipeline* pipeline) override;
		void BindVertexBuffer(Buffer* vertexBuffer) override;
		void BindIndexBuffer(Buffer* indexBuffer) override;
		void BindTexture(uint32_t binding, Texture* texture, Sampler* sampler) override;

		void Draw(uint32_t vertexCount) override;
		void DrawIndexed(uint32_t indexCount) override;
	protected:
		friend class VulkanDevice;
		friend class VulkanTexture;
		VkCommandBuffer GetCommandBuffer() const { return m_CommandBuffer; }
	private:
		struct DescriptorCacheKey {
			VkImageView imageView;
			VkSampler sampler;

			bool operator==(const DescriptorCacheKey& other) const {
				return imageView == other.imageView && sampler == other.sampler;
			}
		};

		struct DescriptorCacheKeyHash {
			size_t operator()(const DescriptorCacheKey& key) const {
				size_t h1 = std::hash<uint64_t>{}(reinterpret_cast<uint64_t>(key.imageView));
				size_t h2 = std::hash<uint64_t>{}(reinterpret_cast<uint64_t>(key.sampler));
				// Hash combining using a prime multiplier for better distribution
				return h1 ^ (h2 * 2654435761u);
			}
		};

		const VulkanDevice* m_Device;
		const VulkanPipeline* m_BoundPipeline = nullptr;

		VkCommandPool m_CommandPool;
		VkCommandBuffer m_CommandBuffer;

		std::unordered_map<DescriptorCacheKey, VkDescriptorSet, DescriptorCacheKeyHash> m_DescriptorSetCache;
	};
}
