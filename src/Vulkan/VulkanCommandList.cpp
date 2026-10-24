#include "VulkanCommandList.h"

#include "VulkanBarrierConversions.h"
#include "VulkanBuffer.h"
#include "VulkanCommandQueue.h"
#include "VulkanConvertions.h"
#include "VulkanPipeline.h"
#include "VulkanTexture.h"
#include "VulkanTextureView.h"

#include <algorithm>
#include <mutex>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace RealRHI {

	VulkanCommandList::VulkanCommandList(const VulkanDevice* device, Ref<VulkanCommandAllocator> allocator)
		: m_Device(device), m_Allocator(std::move(allocator)), m_QueueId{m_Allocator->GetQueueFamilyIndex(), 0} { }

	VulkanCommandList::~VulkanCommandList() {
		std::scoped_lock lock(m_StateMutex);

		if (m_CommandBuffer != VK_NULL_HANDLE && m_State != CommandListState::Pending) {
			m_Allocator->Free(m_CommandBuffer);
			m_CommandBuffer = VK_NULL_HANDLE;
		}
	}

	Result VulkanCommandList::Create(const VulkanDevice* device,
		Ref<VulkanCommandAllocator> allocator,
		Ref<VulkanCommandList>& out) {
		if (!allocator) {
			return Result::Failed;
		}

		Ref<VulkanCommandList> list = Ref<VulkanCommandList>::Create(device, std::move(allocator));
		if (list->Init() != Result::Success) {
			return Result::Failed;
		}

		out = list;
		return Result::Success;
	}

	Result VulkanCommandList::Init() {
		if (m_Allocator->Allocate(m_CommandBuffer) != VK_SUCCESS) {
			return Result::Failed;
		}

		return Result::Success;
	}

	Result VulkanCommandList::Begin() {
		{
			std::scoped_lock lock(m_StateMutex);
			if (m_State == CommandListState::Recording || m_State == CommandListState::Pending) {
				m_Device->SendDebugMessage(DebugSeverity::Error,
					DebugMessageType::General,
					"Command buffer cannot begin while already recording or pending.");

				return Result::Failed;
			}
		}

		if (GetState() == CommandListState::Executable) {
			const Result resetResult = Reset();
			if (resetResult != Result::Success) {
				return resetResult;
			}
		}

		VkCommandBufferBeginInfo beginInfo{};
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

		{
			std::scoped_lock lock(m_StateMutex);
			if (vkBeginCommandBuffer(m_CommandBuffer, &beginInfo) != VK_SUCCESS) {
				m_Device->SendDebugMessage(
					DebugSeverity::Error, DebugMessageType::General, "Failed to begin Vulkan command buffer.");

				return Result::Failed;
			}
			m_State = CommandListState::Recording;
			m_ComputePipelineBound = false;
			m_GraphicsPipelineBound = false;
			m_Rendering = false;
		}

		return Result::Success;
	}

	Result VulkanCommandList::End() {
		if (m_Rendering) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"End: Call EndRendering before ending command recording.");
			return Result::InvalidParameter;
		}
		{
			std::scoped_lock lock(m_StateMutex);
			if (m_State != CommandListState::Recording) {
				m_Device->SendDebugMessage(
					DebugSeverity::Error, DebugMessageType::General, "Command buffer can only end while recording.");

				return Result::Failed;
			}
		}

		{
			std::scoped_lock lock(m_StateMutex);
			if (vkEndCommandBuffer(m_CommandBuffer) != VK_SUCCESS) {
				m_Device->SendDebugMessage(
					DebugSeverity::Error, DebugMessageType::General, "Failed to end Vulkan command buffer.");

				return Result::Failed;
			}
			m_State = CommandListState::Executable;
		}

		return Result::Success;
	}

	void VulkanCommandList::RetainResource(RefCounted* resource) {
		if (resource == nullptr) {
			m_Device->SendDebugMessage(
				DebugSeverity::Error, DebugMessageType::General, "Attempted to retain a null resource.");
			return;
		}

		for (const auto& retained : m_ReferencedResources) {
			if (retained.Raw() == resource) {
				return;
			}
		}

		m_ReferencedResources.emplace_back(resource);
	}

	Result VulkanCommandList::Reset() {
		CommandListState state = GetState();
		if (state == CommandListState::Pending || state == CommandListState::Recording) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"Cannot reset a command buffer while it is pending or recording.");
			return Result::Failed;
		}

		if (state == CommandListState::Initial) {
			return Result::Success;
		}

		{
			std::scoped_lock lock(m_StateMutex);
			VkResult resetResult = m_Allocator->ResetBuffer(m_CommandBuffer);
			if (resetResult != VK_SUCCESS) {
				m_Device->SendDebugMessage(
					DebugSeverity::Error, DebugMessageType::General, "Failed to reset Vulkan command buffer.");
				return Result::Failed;
			}

			m_State = CommandListState::Initial;
		}

		// The native command buffer no longer references the old recording.
		// Release outside the state mutex (destructors may invoke other locks).
		std::vector<Ref<RefCounted>> released;
		released.swap(m_ReferencedResources);
		m_ComputePipelineBound = false;
		m_RecordedLayouts.clear();
		m_ResourceOperations.clear();
		m_ShaderViews.clear();
		m_ShaderBuffers.clear();
		m_VertexBuffer = nullptr;
		m_IndexBuffer = nullptr;
		return Result::Success;
	}

	bool VulkanCommandList::ValidateAttachmentView(const VulkanTextureView* view,
		TextureUsage usage,
		const Rect& renderArea) const {
		if (!view->HasNativeImageView() || !Any(view->GetUsage() & usage)) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"BeginRendering: Attachment requires a native view with matching color or depth/stencil usage.");

			return false;
		}

		const auto* texture = view->GetTexture();
		if (texture->IsExternal()) {
			// Swapchain textures expose their current extent through FrameContext.
			return true;
		}

		const uint32_t mip = view->GetSubresources().baseMipLevel;
		const auto extent = texture->GetExtent();
		const uint32_t width = std::max(1u, extent.width >> mip);
		const uint32_t height = std::max(1u, extent.height >> mip);
		if (renderArea.x < 0 || renderArea.y < 0 || static_cast<uint32_t>(renderArea.x) > width ||
			static_cast<uint32_t>(renderArea.y) > height ||
			renderArea.width > width - static_cast<uint32_t>(renderArea.x) ||
			renderArea.height > height - static_cast<uint32_t>(renderArea.y)) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"BeginRendering: The render area must fit within the attachment view's selected mip.");

			return false;
		}

		return true;
	}

	void VulkanCommandList::BeginRendering(const RenderingInfo& renderingInfo) {
		if (GetState() != CommandListState::Recording || m_Rendering || renderingInfo.renderArea.width == 0 ||
			renderingInfo.renderArea.height == 0) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"BeginRendering: Requires recording outside a rendering scope and a nonzero render area.");
			return;
		}

		std::vector<VkRenderingAttachmentInfo> colorAttachmentInfos(renderingInfo.colorAttachments.size());
		for (uint32_t i = 0; i < renderingInfo.colorAttachments.size(); i++) {
			const auto& attachment = renderingInfo.colorAttachments[i];
			if (attachment.target == nullptr) {
				m_Device->SendDebugMessage(DebugSeverity::Error,
					DebugMessageType::General,
					"Rendering color attachment is missing a texture view.");
				return;
			}

			auto* textureView = static_cast<VulkanTextureView*>(attachment.target);
			if (!ValidateAttachmentView(textureView, TextureUsage::RenderTarget, renderingInfo.renderArea)) {
				return;
			}

			RetainResource(textureView);
			RetainResource(const_cast<VulkanTexture*>(textureView->GetTexture()));

			VkClearValue clearColor = {
				{{attachment.clearColor.r, attachment.clearColor.g, attachment.clearColor.b, attachment.clearColor.a}}
			};
			colorAttachmentInfos[i] = {
				.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
				.imageView = textureView->GetImageView(),
				.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				.resolveMode = VK_RESOLVE_MODE_NONE,
				.loadOp = Utils::LoadOpToVkLoadOp(attachment.loadOp),
				.storeOp = Utils::StoreOpToVkStoreOp(attachment.storeOp),
				.clearValue = clearColor,
			};
		}

		VkRenderingAttachmentInfo depthAttachmentInfo{
			.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
		};
		const VkRenderingAttachmentInfo* pDepthAttachment = nullptr;
		const VkRenderingAttachmentInfo* pStencilAttachment = nullptr;

		if (renderingInfo.hasDepth) {
			const auto& attachment = renderingInfo.depthAttachment;
			if (attachment.target == nullptr) {
				m_Device->SendDebugMessage(DebugSeverity::Error,
					DebugMessageType::General,
					"Rendering depth attachment is missing a texture view.");
				return;
			}

			auto* textureView = static_cast<VulkanTextureView*>(attachment.target);
			if (!ValidateAttachmentView(textureView, TextureUsage::DepthStencil, renderingInfo.renderArea)) {
				return;
			}

			RetainResource(textureView);
			RetainResource(const_cast<VulkanTexture*>(textureView->GetTexture()));

			const auto format = Utils::VkFormatToTextureFormat(textureView->GetTexture()->GetFormat());
			const VkImageAspectFlags aspectMask = Utils::TextureFormatToVkImageAspect(format);
			const bool hasStencilAspect = (aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
			const bool readOnly = attachment.readOnlyDepth && (!hasStencilAspect || attachment.readOnlyStencil);
			VkImageLayout depthLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			if (readOnly) {
				depthLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
			}
			VkClearValue clearValue{};
			clearValue.depthStencil = {
				.depth = attachment.clear.depth,
				.stencil = attachment.clear.stencil,
			};

			depthAttachmentInfo = {
				.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
				.imageView = textureView->GetImageView(),
				.imageLayout = depthLayout,
				.resolveMode = VK_RESOLVE_MODE_NONE,
				.loadOp = Utils::LoadOpToVkLoadOp(attachment.depthLoadOp),
				.storeOp = Utils::StoreOpToVkStoreOp(attachment.depthStoreOp),
				.clearValue = clearValue,
			};

			if ((aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) != 0) {
				pDepthAttachment = &depthAttachmentInfo;
			}
			if (hasStencilAspect) {
				pStencilAttachment = &depthAttachmentInfo;
			}
		}

		auto& renderAreaRect = renderingInfo.renderArea;
		VkRect2D renderArea{
			.offset =
				{
					.x = renderAreaRect.x,
					.y = renderAreaRect.y,
				},
			.extent = {
				.width = renderAreaRect.width,
				.height = renderAreaRect.height,
			},
		};

		VkRenderingInfo renderPassInfo{
			.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
			.renderArea = renderArea,
			.layerCount = 1,
			.colorAttachmentCount = static_cast<uint32_t>(colorAttachmentInfos.size()),
			.pColorAttachments = colorAttachmentInfos.data(),
			.pDepthAttachment = pDepthAttachment,
			.pStencilAttachment = pStencilAttachment,
		};

		auto recordAttachment = [this](TextureView* view, TextureLayout layout) {
			auto* textureView = static_cast<VulkanTextureView*>(view);
			RecordTextureUse(const_cast<VulkanTexture*>(textureView->GetTexture()),
				textureView->GetSubresources(),
				layout,
				"BeginRendering");
		};
		for (const auto& attachment : renderingInfo.colorAttachments) {
			recordAttachment(attachment.target, TextureLayout::ColorAttachment);
		}
		if (renderingInfo.hasDepth) {
			TextureLayout layout = TextureLayout::DepthStencilAttachment;
			if (depthAttachmentInfo.imageLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL) {
				layout = TextureLayout::DepthStencilReadOnly;
			}
			recordAttachment(renderingInfo.depthAttachment.target, layout);
		}
		vkCmdBeginRendering(m_CommandBuffer, &renderPassInfo);
		m_Rendering = true;
	}

	void VulkanCommandList::EndRendering() {
		if (GetState() != CommandListState::Recording || !m_Rendering) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"EndRendering: Call BeginRendering before ending a rendering scope.");
			return;
		}
		vkCmdEndRendering(m_CommandBuffer);
		m_Rendering = false;
	}

	void VulkanCommandList::SetViewport(const Viewport& vp) {
		if (GetState() != CommandListState::Recording) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"SetViewport: Call Begin before setting the viewport.");
			return;
		}
		VkViewport viewport{
			.x = vp.x,
			.y = vp.y,
			.width = vp.width,
			.height = vp.height,
			.minDepth = vp.minDepth,
			.maxDepth = vp.maxDepth,
		};
		vkCmdSetViewport(m_CommandBuffer, 0, 1, &viewport);
	}

	void VulkanCommandList::SetScissor(const Rect& rect) {
		if (GetState() != CommandListState::Recording) {
			m_Device->SendDebugMessage(
				DebugSeverity::Error, DebugMessageType::General, "SetScissor: Call Begin before setting the scissor.");
			return;
		}
		VkRect2D scissor{
			.offset = {.x = rect.x, .y = rect.y},
			.extent = {.width = rect.width, .height = rect.height},
		};
		vkCmdSetScissor(m_CommandBuffer, 0, 1, &scissor);
	}

	void VulkanCommandList::UseResource(Texture* texture) {
		if (texture == nullptr || GetState() != CommandListState::Recording) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"UseResource(Texture): Requires an active recording and a non-null texture.");
			return;
		}

		UseResource(texture->GetTextureView());
	}

	void VulkanCommandList::UseResource(TextureView* view) {
		if (view == nullptr || GetState() != CommandListState::Recording) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"UseResource(TextureView): Requires an active recording and a non-null view.");

			return;
		}

		auto* vulkanView = static_cast<VulkanTextureView*>(view);
		if (!Any(vulkanView->GetUsage() & (TextureUsage::ShaderResource | TextureUsage::Storage))) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"UseResource(TextureView): Declare a sampled or storage view for shader access.");

			return;
		}

		RetainResource(view);
		RetainResource(const_cast<VulkanTexture*>(vulkanView->GetTexture()));

		if (std::find(m_ShaderViews.begin(), m_ShaderViews.end(), vulkanView) == m_ShaderViews.end()) {
			m_ShaderViews.push_back(vulkanView);
		}
	}

	void VulkanCommandList::UseResource(Sampler* sampler) {
		if (sampler == nullptr || GetState() != CommandListState::Recording) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"UseResource(Sampler): Requires an active recording and a non-null sampler.");

			return;
		}

		RetainResource(sampler);
	}

	void VulkanCommandList::UseResource(Buffer* buffer) {
		if (buffer == nullptr || GetState() != CommandListState::Recording) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"UseResource(Buffer): Requires an active recording and a non-null buffer.");
			return;
		}

		RetainResource(buffer);
		auto* vulkanBuffer = static_cast<VulkanBuffer*>(buffer);
		if (std::find(m_ShaderBuffers.begin(), m_ShaderBuffers.end(), vulkanBuffer) == m_ShaderBuffers.end()) {
			m_ShaderBuffers.push_back(vulkanBuffer);
		}
	}

	void VulkanCommandList::BindPipeline(Pipeline* pipeline) {
		auto* vulkanPipeline = static_cast<VulkanPipeline*>(pipeline);
		if (GetState() != CommandListState::Recording || vulkanPipeline == nullptr) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"BindPipeline: Begin command recording and provide a non-null pipeline before binding.");

			return;
		}

		RetainResource(vulkanPipeline);
		vkCmdBindPipeline(m_CommandBuffer, vulkanPipeline->GetBindPoint(), vulkanPipeline->GetPipeline());
		m_Device->BindDescriptorHeaps(m_CommandBuffer);

		if (vulkanPipeline->GetBindPoint() == VK_PIPELINE_BIND_POINT_COMPUTE) {
			m_ComputePipelineBound = true;
		}
		else {
			m_GraphicsPipelineBound = true;
		}
	}

	void VulkanCommandList::DispatchCompute(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) {
		if (GetState() != CommandListState::Recording || m_Rendering || !m_ComputePipelineBound || groupCountX == 0 ||
			groupCountY == 0 || groupCountZ == 0) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"DispatchCompute: A compute pipeline must be bound, the command list must be "
				"recording, and all three dispatch group counts must be nonzero.");
			return;
		}

		RecordShaderResources("DispatchCompute");
		vkCmdDispatch(m_CommandBuffer, groupCountX, groupCountY, groupCountZ);
	}

	void VulkanCommandList::SetPushConstants(const void* data, uint32_t size, uint32_t offset) {
		const uint32_t maxSize = m_Device->GetMaxPushDataSize();
		if (GetState() != CommandListState::Recording || data == nullptr || size == 0 || (size % 4) != 0 ||
			(offset % 4) != 0 || offset > maxSize || size > maxSize - offset) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"SetPushConstants: Requires an active recording, non-null data, nonzero "
				"4-byte-aligned size and offset, and a range within maxPushDataSize.");
			return;
		}

		const VkPushDataInfoEXT pushDataInfo{
			.sType = VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT,
			.offset = offset,
			.data = {
				.address = data,
				.size = size,
			},
		};
		vkCmdPushDataEXT(m_CommandBuffer, &pushDataInfo);
	}

	void VulkanCommandList::BindVertexBuffer(Buffer* vertexBuffer) {
		auto* buffer = static_cast<VulkanBuffer*>(vertexBuffer);
		if (GetState() != CommandListState::Recording || buffer == nullptr ||
			!Any(buffer->GetUsage() & BufferUsage::Vertex)) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"BindVertexBuffer: Requires recording and a Vulkan buffer with Vertex usage.");
			return;
		}
		RetainResource(vertexBuffer);
		m_VertexBuffer = buffer;
		VkBuffer vertexBuffers[] = {static_cast<VulkanBuffer*>(vertexBuffer)->GetBuffer()};
		VkDeviceSize offsets[] = {0};
		vkCmdBindVertexBuffers(m_CommandBuffer, 0, 1, vertexBuffers, offsets);
	}

	void VulkanCommandList::BindIndexBuffer(Buffer* indexBuffer) {
		auto* buffer = static_cast<VulkanBuffer*>(indexBuffer);
		if (GetState() != CommandListState::Recording || buffer == nullptr ||
			!Any(buffer->GetUsage() & BufferUsage::Index)) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"BindIndexBuffer: Requires recording and a Vulkan buffer with Index usage.");
			return;
		}
		RetainResource(indexBuffer);
		m_IndexBuffer = buffer;
		vkCmdBindIndexBuffer(
			m_CommandBuffer, static_cast<VulkanBuffer*>(indexBuffer)->GetBuffer(), 0, VK_INDEX_TYPE_UINT32);
	}

	void VulkanCommandList::CopyBuffer(Buffer* source, Buffer* destination, const BufferCopyRegion& region) {
		if (GetState() != CommandListState::Recording || region.size == 0) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"CopyBuffer failed: Command list is not recording or region size is zero.");

			return;
		}

		auto* sourceBuffer = static_cast<VulkanBuffer*>(source);
		auto* destinationBuffer = static_cast<VulkanBuffer*>(destination);
		if (sourceBuffer == nullptr || destinationBuffer == nullptr ||
			!Any(sourceBuffer->GetUsage() & BufferUsage::TransferSrc) ||
			!Any(destinationBuffer->GetUsage() & BufferUsage::TransferDst)) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"CopyBuffer: Provide non-null source and destination buffers with TransferSrc "
				"and TransferDst usage respectively.");

			return;
		}

		if (region.sourceOffset > sourceBuffer->GetSize() ||
			region.size > sourceBuffer->GetSize() - region.sourceOffset ||
			region.destinationOffset > destinationBuffer->GetSize() ||
			region.size > destinationBuffer->GetSize() - region.destinationOffset) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"CopyBuffer failed: Copy region is out of bounds of source or destination buffer.");

			return;
		}

		RetainResource(sourceBuffer);
		RetainResource(destinationBuffer);
		RecordBufferUse(sourceBuffer, region.sourceOffset, region.size, "CopyBuffer source");
		RecordBufferUse(destinationBuffer, region.destinationOffset, region.size, "CopyBuffer destination");

		VkBufferCopy copyRegion{
			.srcOffset = region.sourceOffset,
			.dstOffset = region.destinationOffset,
			.size = region.size,
		};
		vkCmdCopyBuffer(m_CommandBuffer, sourceBuffer->GetBuffer(), destinationBuffer->GetBuffer(), 1, &copyRegion);
	}

	TextureLayout VulkanCommandList::GetExpectedLayout(const VulkanTexture* texture,
		uint32_t mip,
		uint32_t layer) const {
		const auto it = m_RecordedLayouts.find({texture, mip, layer});
		if (it != m_RecordedLayouts.end()) {
			return it->second;
		}

		return texture->GetSubmittedState(mip, layer).layout;
	}

	bool VulkanCommandList::ValidateOwnership(VulkanQueueOwnership& ownership,
		const ResourceOperation& operation,
		SubmissionToken submission,
		std::span<const VkSemaphoreSubmitInfo> waits) const {
		auto reject = [&](const char* cause) {
			std::ostringstream message;
			message << "Submit: " << operation.name << ": " << cause << "; family=" << m_QueueId.familyIndex
					<< ", owner=" << ownership.family << ", pending destination=" << ownership.pendingFamily;
			if (operation.buffer) {
				message << ", buffer=" << operation.buffer << ", offset=" << operation.offset
						<< ", size=" << operation.size;
			}
			else {
				message << ", texture=" << operation.texture << ", base mip=" << operation.subresources.baseMipLevel
						<< ", base layer=" << operation.subresources.baseArrayLayer;
			}
			if (ownership.release.IsValid()) {
				message << ", release token=" << ownership.release.queue.familyIndex << ":"
						<< ownership.release.queue.queueIndex << "/" << ownership.release.value;
			}
			m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, message.str().c_str());
			return false;
		};

		const bool transfer =
			operation.sourceFamily != UINT32_MAX && operation.sourceFamily != operation.destinationFamily;
		const bool acquire = transfer && m_QueueId.familyIndex == operation.destinationFamily;
		if (acquire) {
			if (ownership.family != operation.sourceFamily || ownership.pendingFamily != operation.destinationFamily ||
				!ownership.release.IsValid()) {
				return reject("acquire requires a matching release from the source family");
			}
			auto* sourceQueue = m_Device->GetCommandQueue(ownership.release.queue);
			bool waited = false;
			for (const auto& wait : waits) {
				if (sourceQueue && wait.semaphore == sourceQueue->GetTimelineSemaphore() &&
					wait.value >= ownership.release.value &&
					(wait.stageMask & VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) != 0) {
					waited = true;
					break;
				}
			}
			if (!waited) {
				return reject("acquire must waitFor the release submission token (or a later token on that queue)");
			}
			ownership.family = operation.destinationFamily;
			ownership.pendingFamily = UINT32_MAX;
			ownership.release = {};
			return true;
		}

		if (ownership.pendingFamily != UINT32_MAX) {
			return reject("resource was released; record a matching acquire before using or releasing it again");
		}
		if (ownership.family != UINT32_MAX && ownership.family != m_QueueId.familyIndex) {
			return reject("resource belongs to another family; release there and acquire on this family");
		}
		ownership.family = m_QueueId.familyIndex;
		if (transfer) {
			ownership.pendingFamily = operation.destinationFamily;
			ownership.release = submission;
		}
		return true;
	}

	bool VulkanCommandList::ValidateSubmissionState(SubmissionState& predicted,
		SubmissionToken submission,
		std::span<const VkSemaphoreSubmitInfo> waits) const {
		for (const auto& operation : m_ResourceOperations) {
			const bool transfer =
				operation.sourceFamily != UINT32_MAX && operation.sourceFamily != operation.destinationFamily;
			const bool acquire = transfer && m_QueueId.familyIndex == operation.destinationFamily;
			if (operation.buffer) {
				auto [entry, inserted] = predicted.buffers.try_emplace(operation.buffer);
				if (inserted) {
					entry->second = operation.buffer->GetSubmittedRanges();
				}
				auto& ranges = entry->second;
				const uint64_t end = operation.offset + operation.size;
				// Split at operation boundaries so disjoint buffer regions can change owners independently.
				for (uint64_t boundary : {operation.offset, end}) {
					for (size_t i = 0; i < ranges.size(); ++i) {
						auto& range = ranges[i];
						if (boundary > range.offset && boundary < range.offset + range.size) {
							auto tail = range;
							tail.size = range.offset + range.size - boundary;
							tail.offset = boundary;
							range.size = boundary - range.offset;
							ranges.insert(ranges.begin() + i + 1, tail);
							break;
						}
					}
				}
				for (auto& range : ranges) {
					if (range.offset < operation.offset || range.offset >= end) {
						continue;
					}
					if (!ValidateOwnership(range.ownership, operation, submission, waits)) {
						return false;
					}
					if (acquire && (range.releaseOffset != operation.offset || range.releaseSize != operation.size)) {
						m_Device->SendDebugMessage(DebugSeverity::Error,
							DebugMessageType::General,
							"Submit: Buffer acquire offset and size must exactly match the release barrier.");
						return false;
					}
					if (transfer && !acquire) {
						range.releaseOffset = operation.offset;
						range.releaseSize = operation.size;
					}
				}
				continue;
			}

			const auto& subresources = operation.subresources;
			for (uint32_t mip = subresources.baseMipLevel; mip < subresources.baseMipLevel + subresources.mipLevelCount;
				++mip) {
				for (uint32_t layer = subresources.baseArrayLayer;
					layer < subresources.baseArrayLayer + subresources.arrayLayerCount;
					++layer) {
					const SubresourceKey key{operation.texture, mip, layer};
					auto [entry, inserted] = predicted.textures.try_emplace(key);
					if (inserted) {
						entry->second = operation.texture->GetSubmittedState(mip, layer);
					}
					auto& state = entry->second;
					if (!ValidateOwnership(state.ownership, operation, submission, waits)) {
						return false;
					}
					if (acquire) {
						const auto& released = state.releaseRange;
						if (state.releaseOldLayout != operation.oldLayout ||
							state.releaseNewLayout != operation.newLayout ||
							released.baseMipLevel != subresources.baseMipLevel ||
							released.mipLevelCount != subresources.mipLevelCount ||
							released.baseArrayLayer != subresources.baseArrayLayer ||
							released.arrayLayerCount != subresources.arrayLayerCount) {
							m_Device->SendDebugMessage(DebugSeverity::Error,
								DebugMessageType::General,
								"Submit: Texture acquire old/new layouts and mip/layer range must exactly match the "
								"release barrier.");
							return false;
						}
					}
					else if (operation.oldLayout != TextureLayout::Undefined && state.layout != operation.oldLayout) {
						std::ostringstream message;
						message << "Submit: " << operation.name << ": texture=" << operation.texture << ", mip=" << mip
								<< ", layer=" << layer << ": layout differs from the recorded requirement; expected="
								<< static_cast<uint32_t>(operation.oldLayout)
								<< ", submitted=" << static_cast<uint32_t>(state.layout);
						m_Device->SendDebugMessage(
							DebugSeverity::Error, DebugMessageType::General, message.str().c_str());
						return false;
					}
					if (transfer && !acquire) {
						state.releaseOldLayout = operation.oldLayout;
						state.releaseNewLayout = operation.newLayout;
						state.releaseRange = subresources;
					}
					if (operation.barrier) {
						state.layout = operation.newLayout;
					}
				}
			}
		}
		return true;
	}

	void VulkanCommandList::CommitSubmissionState(SubmissionState& predicted) {
		for (auto& [buffer, ranges] : predicted.buffers) {
			buffer->CommitSubmittedRanges(std::move(ranges));
		}
		for (const auto& [key, state] : predicted.textures) {
			const_cast<VulkanTexture*>(key.texture)->CommitSubmittedState(key.mipLevel, key.arrayLayer, state);
		}
	}

	void VulkanCommandList::RecordBufferUse(VulkanBuffer* buffer,
		uint64_t offset,
		uint64_t size,
		const char* operation) {
		m_ResourceOperations.push_back({.name = operation, .buffer = buffer, .offset = offset, .size = size});
	}

	void VulkanCommandList::RecordTextureUse(VulkanTexture* texture,
		TextureSubresourceRange subresources,
		TextureLayout layout,
		const char* operation) {
		// Swapchain pre/post transitions and presentation ownership are managed by Device::Submit.
		if (!texture->IsExternal()) {
			m_ResourceOperations.push_back(
				{.name = operation, .texture = texture, .subresources = subresources, .oldLayout = layout});
		}
	}

	void VulkanCommandList::RecordShaderResources(const char* operation) {
		for (auto* buffer : m_ShaderBuffers) {
			RecordBufferUse(buffer, 0, buffer->GetSize(), operation);
		}
		for (auto* view : m_ShaderViews) {
			TextureLayout layout = TextureLayout::ShaderRead;
			if (Any(view->GetUsage() & TextureUsage::Storage)) {
				layout = TextureLayout::General;
			}

			RecordTextureUse(
				const_cast<VulkanTexture*>(view->GetTexture()), view->GetSubresources(), layout, operation);
		}
	}

	void VulkanCommandList::RegisterRequiredLayout(const VulkanTexture* texture,
		uint32_t mipLevel,
		uint32_t baseLayer,
		uint32_t layerCount,
		TextureLayout expected) {
		RecordTextureUse(
			const_cast<VulkanTexture*>(texture), {mipLevel, 1, baseLayer, layerCount}, expected, "Texture copy");
	}

	bool VulkanCommandList::HasRecordedLayout(const VulkanTexture* texture,
		uint32_t mipLevel,
		uint32_t baseLayer,
		uint32_t layerCount,
		TextureLayout expected) const {
		for (uint32_t layer = baseLayer; layer < baseLayer + layerCount; ++layer) {
			const auto recorded = m_RecordedLayouts.find({texture, mipLevel, layer});
			if (expected != TextureLayout::Undefined && recorded != m_RecordedLayouts.end() &&
				recorded->second != expected) {
				return false;
			}
		}
		return true;
	}

	void VulkanCommandList::CopyBufferToTexture(Buffer* source,
		Texture* destination,
		const BufferTextureCopyRegion& region) {
		if (GetState() != CommandListState::Recording || region.width == 0 || region.height == 0 || region.depth == 0 ||
			region.layerCount == 0) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"CopyBufferToTexture failed: Command list is not recording or region dimensions are zero.");

			return;
		}

		auto* sourceBuffer = static_cast<VulkanBuffer*>(source);
		auto* destinationTexture = static_cast<VulkanTexture*>(destination);
		if (sourceBuffer == nullptr || destinationTexture == nullptr ||
			region.mipLevel >= destinationTexture->GetMipLevelCount() ||
			region.baseArrayLayer >= destinationTexture->GetArrayLayerCount() ||
			region.layerCount > destinationTexture->GetArrayLayerCount() - region.baseArrayLayer ||
			!Any(sourceBuffer->GetUsage() & BufferUsage::TransferSrc) ||
			!Any(destinationTexture->GetUsage() & TextureUsage::TransferDst)) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"CopyBufferToTexture failed: Invalid source buffer or destination texture, or "
				"invalid copy region parameters.");

			return;
		}

		const VkExtent3D mipExtent{
			.width = std::max(1u, destinationTexture->GetExtent().width >> region.mipLevel),
			.height = std::max(1u, destinationTexture->GetExtent().height >> region.mipLevel),
			.depth = std::max(1u, destinationTexture->GetExtent().depth >> region.mipLevel),
		};
		if (region.width > mipExtent.width || region.height > mipExtent.height || region.depth > mipExtent.depth ||
			region.bufferOffset >= sourceBuffer->GetSize()) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"CopyBufferToTexture failed: Copy region dimensions exceed mip level extent or "
				"buffer offset is out of bounds.");

			return;
		}

		const uint32_t texelSize =
			Utils::TextureFormatByteSize(Utils::VkFormatToTextureFormat(destinationTexture->GetFormat()));
		if (texelSize == 0 || region.bufferOffset % texelSize != 0 ||
			region.width > UINT64_MAX / region.height / region.depth / region.layerCount / texelSize ||
			static_cast<uint64_t>(region.width) * region.height * region.depth * region.layerCount * texelSize >
				sourceBuffer->GetSize() - region.bufferOffset) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"CopyBufferToTexture: Unsupported format, misaligned buffer offset, or "
				"insufficient source buffer capacity.");
			return;
		}

		RegisterRequiredLayout(
			destinationTexture, region.mipLevel, region.baseArrayLayer, region.layerCount, TextureLayout::TransferDst);
		RetainResource(sourceBuffer);
		RetainResource(destinationTexture);
		RecordBufferUse(sourceBuffer,
			region.bufferOffset,
			static_cast<uint64_t>(region.width) * region.height * region.depth * region.layerCount * texelSize,
			"CopyBufferToTexture");
		VkBufferImageCopy copyRegion{
			.bufferOffset = region.bufferOffset,
			.imageSubresource =
				{
					.aspectMask = Utils::TextureFormatToVkImageAspect(
						Utils::VkFormatToTextureFormat(destinationTexture->GetFormat())),
					.mipLevel = region.mipLevel,
					.baseArrayLayer = region.baseArrayLayer,
					.layerCount = region.layerCount,
				},
			.imageExtent = {region.width, region.height, region.depth},
		};
		vkCmdCopyBufferToImage(m_CommandBuffer,
			sourceBuffer->GetBuffer(),
			destinationTexture->GetImage(),
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1,
			&copyRegion);
	}

	void VulkanCommandList::CopyTextureToBuffer(Texture* source,
		Buffer* destination,
		const BufferTextureCopyRegion& region) {
		if (GetState() != CommandListState::Recording || region.width == 0 || region.height == 0 || region.depth == 0 ||
			region.layerCount == 0) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"Texture copy failed: Command list is not recording or region dimensions are zero.");
			return;
		}

		auto* texture = static_cast<VulkanTexture*>(source);
		auto* buffer = static_cast<VulkanBuffer*>(destination);
		if (texture == nullptr || buffer == nullptr || !Any(texture->GetUsage() & TextureUsage::TransferSrc) ||
			!Any(buffer->GetUsage() & BufferUsage::TransferDst) ||

			region.mipLevel >= texture->GetMipLevelCount() || region.baseArrayLayer >= texture->GetArrayLayerCount() ||
			region.layerCount > texture->GetArrayLayerCount() - region.baseArrayLayer ||
			region.bufferOffset >= buffer->GetSize() ||
			!HasRecordedLayout(
				texture, region.mipLevel, region.baseArrayLayer, region.layerCount, TextureLayout::TransferSrc)) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"CopyTextureToBuffer failed: Invalid source texture, destination buffer, or subresource range.");
			return;
		}

		const VkExtent3D mipExtent{
			.width = std::max(1u, texture->GetExtent().width >> region.mipLevel),
			.height = std::max(1u, texture->GetExtent().height >> region.mipLevel),
			.depth = std::max(1u, texture->GetExtent().depth >> region.mipLevel),
		};
		if (region.width > mipExtent.width || region.height > mipExtent.height || region.depth > mipExtent.depth) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"CopyTextureToBuffer failed: Copy dimensions exceed source mip extent.");
			return;
		}

		// Initial readback supports tightly packed RGBA8 data. Other formats
		// need format-aware block size and row pitch handling.
		if (texture->GetFormat() != VK_FORMAT_R8G8B8A8_UNORM ||
			region.width > UINT64_MAX / region.height / region.depth / region.layerCount / 4 ||
			static_cast<uint64_t>(region.width) * region.height * region.depth * region.layerCount * 4 >
				buffer->GetSize() - region.bufferOffset) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"CopyTextureToBuffer failed: Unsupported texture format or insufficient readback buffer capacity.");
			return;
		}

		RegisterRequiredLayout(
			texture, region.mipLevel, region.baseArrayLayer, region.layerCount, TextureLayout::TransferSrc);
		RetainResource(texture);
		RetainResource(buffer);
		RecordBufferUse(buffer,
			region.bufferOffset,
			static_cast<uint64_t>(region.width) * region.height * region.depth * region.layerCount * 4,
			"CopyTextureToBuffer");
		VkBufferImageCopy copyRegion{
			.bufferOffset = region.bufferOffset,
			.imageSubresource =
				{
					.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
					.mipLevel = region.mipLevel,
					.baseArrayLayer = region.baseArrayLayer,
					.layerCount = region.layerCount,
				},
			.imageExtent = {region.width, region.height, region.depth},
		};
		vkCmdCopyImageToBuffer(m_CommandBuffer,
			texture->GetImage(),
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			buffer->GetBuffer(),
			1,
			&copyRegion);
	}

	void VulkanCommandList::CopyTexture(Texture* source, Texture* destination, const TextureCopyRegion& region) {
		if (GetState() != CommandListState::Recording || region.width == 0 || region.height == 0 || region.depth == 0 ||
			region.layerCount == 0) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"Texture copy failed: Command list is not recording or region dimensions are zero.");
			return;
		}

		auto* sourceTexture = static_cast<VulkanTexture*>(source);
		auto* destinationTexture = static_cast<VulkanTexture*>(destination);
		if (sourceTexture == nullptr || destinationTexture == nullptr ||
			!Any(sourceTexture->GetUsage() & TextureUsage::TransferSrc) ||
			!Any(destinationTexture->GetUsage() & TextureUsage::TransferDst) ||

			sourceTexture == destinationTexture || sourceTexture->GetFormat() != destinationTexture->GetFormat() ||
			sourceTexture->GetFormat() != VK_FORMAT_R8G8B8A8_UNORM ||
			region.sourceMipLevel >= sourceTexture->GetMipLevelCount() ||
			region.destinationMipLevel >= destinationTexture->GetMipLevelCount() ||
			region.sourceBaseArrayLayer >= sourceTexture->GetArrayLayerCount() ||
			region.destinationBaseArrayLayer >= destinationTexture->GetArrayLayerCount() ||
			region.layerCount > sourceTexture->GetArrayLayerCount() - region.sourceBaseArrayLayer ||
			region.layerCount > destinationTexture->GetArrayLayerCount() - region.destinationBaseArrayLayer ||
			!HasRecordedLayout(sourceTexture,
				region.sourceMipLevel,
				region.sourceBaseArrayLayer,
				region.layerCount,
				TextureLayout::TransferSrc) ||
			!HasRecordedLayout(destinationTexture,
				region.destinationMipLevel,
				region.destinationBaseArrayLayer,
				region.layerCount,
				TextureLayout::TransferDst)) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"CopyTexture failed: Invalid textures, formats, or subresource range.");
			return;
		}

		const auto fits = [&](const VulkanTexture* texture, uint32_t mipLevel) {
			return region.width <= std::max(1u, texture->GetExtent().width >> mipLevel) &&
				region.height <= std::max(1u, texture->GetExtent().height >> mipLevel) &&
				region.depth <= std::max(1u, texture->GetExtent().depth >> mipLevel);
		};
		if (!fits(sourceTexture, region.sourceMipLevel) || !fits(destinationTexture, region.destinationMipLevel)) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"CopyTexture failed: Copy dimensions exceed source or destination mip extent.");
			return;
		}

		RegisterRequiredLayout(sourceTexture,
			region.sourceMipLevel,
			region.sourceBaseArrayLayer,
			region.layerCount,
			TextureLayout::TransferSrc);
		RegisterRequiredLayout(destinationTexture,
			region.destinationMipLevel,
			region.destinationBaseArrayLayer,
			region.layerCount,
			TextureLayout::TransferDst);
		RetainResource(sourceTexture);
		RetainResource(destinationTexture);
		VkImageCopy copyRegion{
			.srcSubresource =
				{
					.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
					.mipLevel = region.sourceMipLevel,
					.baseArrayLayer = region.sourceBaseArrayLayer,
					.layerCount = region.layerCount,
				},
			.dstSubresource =
				{
					.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
					.mipLevel = region.destinationMipLevel,
					.baseArrayLayer = region.destinationBaseArrayLayer,
					.layerCount = region.layerCount,
				},
			.extent = {region.width, region.height, region.depth},
		};
		vkCmdCopyImage(m_CommandBuffer,
			sourceTexture->GetImage(),
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			destinationTexture->GetImage(),
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1,
			&copyRegion);
	}

	bool VulkanCommandList::ResolveQueueFamilies(std::optional<QueueType> source,
		std::optional<QueueType> destination,
		uint32_t& sourceFamily,
		uint32_t& destinationFamily) const {
		sourceFamily = UINT32_MAX;
		destinationFamily = UINT32_MAX;
		if (source.has_value() != destination.has_value()) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"Barrier: Specify both sourceQueue and destinationQueue, or neither.");
			return false;
		}
		if (!source) {
			return true;
		}
		auto* sourceQueue = m_Device->GetCommandQueue(*source);
		auto* destinationQueue = m_Device->GetCommandQueue(*destination);
		if (!sourceQueue || !destinationQueue) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"Barrier: Queue endpoints must identify available graphics, compute or transfer queues.");
			return false;
		}
		sourceFamily = sourceQueue->GetQueueId().familyIndex;
		destinationFamily = destinationQueue->GetQueueId().familyIndex;
		if (m_QueueId.familyIndex != sourceFamily && m_QueueId.familyIndex != destinationFamily) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"Barrier: Record ownership transfers on the source family (release) or destination family (acquire).");
			return false;
		}
		return true;
	}

	void VulkanCommandList::Barrier(const BarrierDesc& barriers) {
		if (GetState() != CommandListState::Recording || m_Rendering) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"Barrier: Requires recording outside a rendering scope.");
			return;
		}

		std::vector<ResourceOperation> operations;

		std::vector<VkMemoryBarrier2> memoryBarriers;
		std::vector<VkBufferMemoryBarrier2> bufferBarriers;
		std::vector<VkImageMemoryBarrier2> imageBarriers;
		memoryBarriers.reserve(barriers.memory.size());
		bufferBarriers.reserve(barriers.buffers.size());
		imageBarriers.reserve(barriers.textures.size());

		for (const MemoryBarrier& barrier : barriers.memory) {
			memoryBarriers.push_back({
				.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
				.srcStageMask = Utils::ToVkPipelineStages(barrier.before.stages),
				.srcAccessMask = Utils::ToVkResourceAccess(barrier.before.access),
				.dstStageMask = Utils::ToVkPipelineStages(barrier.after.stages),
				.dstAccessMask = Utils::ToVkResourceAccess(barrier.after.access),
			});
		}

		for (const BufferBarrier& barrier : barriers.buffers) {
			auto* buffer = static_cast<VulkanBuffer*>(barrier.buffer);
			if (buffer == nullptr || barrier.size == 0 || barrier.offset >= buffer->GetSize() ||
				barrier.size > buffer->GetSize() - barrier.offset) {
				m_Device->SendDebugMessage(DebugSeverity::Error,
					DebugMessageType::General,
					"Barrier: Buffer range is invalid. Provide a non-null buffer, nonzero size, "
					"and in-bounds offset and size.");
				return;
			}

			uint32_t sourceFamily, destinationFamily;
			if (!ResolveQueueFamilies(barrier.sourceQueue, barrier.destinationQueue, sourceFamily, destinationFamily)) {
				return;
			}
			const bool transfer = sourceFamily != UINT32_MAX && sourceFamily != destinationFamily;
			ResourceState before = barrier.before;
			ResourceState after = barrier.after;
			if (transfer && m_QueueId.familyIndex == sourceFamily) {
				after = {};
			}
			if (transfer && m_QueueId.familyIndex == destinationFamily) {
				before = {};
			}
			bufferBarriers.push_back({
				.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
				.srcStageMask = Utils::ToVkPipelineStages(before.stages),
				.srcAccessMask = Utils::ToVkResourceAccess(before.access),
				.dstStageMask = Utils::ToVkPipelineStages(after.stages),
				.dstAccessMask = Utils::ToVkResourceAccess(after.access),
				.srcQueueFamilyIndex = transfer ? sourceFamily : VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = transfer ? destinationFamily : VK_QUEUE_FAMILY_IGNORED,
				.buffer = buffer->GetBuffer(),
				.offset = barrier.offset,
				.size = barrier.size,
			});
			operations.push_back(
				{.name = "Buffer barrier",
					.buffer = buffer,
					.offset = barrier.offset,
					.size = barrier.size,
					.sourceFamily = sourceFamily,
					.destinationFamily = destinationFamily,
					.barrier = true});
		}

		for (const TextureBarrier& barrier : barriers.textures) {
			auto* texture = static_cast<VulkanTexture*>(barrier.texture);
			if (texture == nullptr || barrier.newLayout == TextureLayout::Undefined ||
				barrier.subresources.mipLevelCount == 0 || barrier.subresources.arrayLayerCount == 0 ||
				barrier.subresources.baseMipLevel >= texture->GetMipLevelCount() ||
				barrier.subresources.mipLevelCount > texture->GetMipLevelCount() - barrier.subresources.baseMipLevel ||
				barrier.subresources.baseArrayLayer >= texture->GetArrayLayerCount() ||
				barrier.subresources.arrayLayerCount >
					texture->GetArrayLayerCount() - barrier.subresources.baseArrayLayer) {
				m_Device->SendDebugMessage(DebugSeverity::Error,
					DebugMessageType::General,
					"Barrier: Provide a defined newLayout and a valid texture mip/layer range.");
				return;
			}

			uint32_t sourceFamily, destinationFamily;
			if (!ResolveQueueFamilies(barrier.sourceQueue, barrier.destinationQueue, sourceFamily, destinationFamily)) {
				return;
			}
			const bool transfer = sourceFamily != UINT32_MAX && sourceFamily != destinationFamily;
			const bool acquire = transfer && m_QueueId.familyIndex == destinationFamily;
			for (uint32_t mip = barrier.subresources.baseMipLevel;
				mip < barrier.subresources.baseMipLevel + barrier.subresources.mipLevelCount;
				++mip) {
				if (!acquire &&
					!HasRecordedLayout(texture,
						mip,
						barrier.subresources.baseArrayLayer,
						barrier.subresources.arrayLayerCount,
						barrier.oldLayout)) {
					m_Device->SendDebugMessage(DebugSeverity::Error,
						DebugMessageType::General,
						"Texture barrier old layout disagrees with an earlier transition in this command list.");
					return;
				}
			}

			ResourceState before = barrier.before;
			ResourceState after = barrier.after;
			if (transfer && !acquire) {
				after = {};
			}
			if (acquire) {
				before = {};
			}
			imageBarriers.push_back({
				.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
				.srcStageMask = Utils::ToVkPipelineStages(before.stages),
				.srcAccessMask = Utils::ToVkResourceAccess(before.access),
				.dstStageMask = Utils::ToVkPipelineStages(after.stages),
				.dstAccessMask = Utils::ToVkResourceAccess(after.access),
				.oldLayout = Utils::TextureLayoutToVkImageLayout(barrier.oldLayout),
				.newLayout = Utils::TextureLayoutToVkImageLayout(barrier.newLayout),
				.srcQueueFamilyIndex = transfer ? sourceFamily : VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = transfer ? destinationFamily : VK_QUEUE_FAMILY_IGNORED,
				.image = texture->GetImage(),
				.subresourceRange = {
					.aspectMask =
						Utils::TextureFormatToVkImageAspect(Utils::VkFormatToTextureFormat(texture->GetFormat())),
					.baseMipLevel = barrier.subresources.baseMipLevel,
					.levelCount = barrier.subresources.mipLevelCount,
					.baseArrayLayer = barrier.subresources.baseArrayLayer,
					.layerCount = barrier.subresources.arrayLayerCount,
				},
			});
			operations.push_back(
				{.name = "Texture barrier",
					.texture = texture,
					.subresources = barrier.subresources,
					.oldLayout = barrier.oldLayout,
					.newLayout = barrier.newLayout,
					.sourceFamily = sourceFamily,
					.destinationFamily = destinationFamily,
					.barrier = true});
		}

		// Reject overlapping ranges: barriers within one dependency are not sequential.
		for (size_t i = 0; i < barriers.buffers.size(); ++i) {
			for (size_t j = 0; j < i; ++j) {
				const auto& first = barriers.buffers[i];
				const auto& second = barriers.buffers[j];
				if (first.buffer == second.buffer && first.offset < second.offset + second.size &&
					second.offset < first.offset + first.size) {
					m_Device->SendDebugMessage(DebugSeverity::Error,
						DebugMessageType::General,
						"Barrier: Overlapping buffer barriers must be recorded in separate calls.");
					return;
				}
			}
		}
		std::unordered_set<SubresourceKey, SubresourceKeyHash> touched;
		for (const TextureBarrier& barrier : barriers.textures) {
			auto* texture = static_cast<VulkanTexture*>(barrier.texture);
			for (uint32_t mip = barrier.subresources.baseMipLevel;
				mip < barrier.subresources.baseMipLevel + barrier.subresources.mipLevelCount;
				++mip) {
				for (uint32_t layer = barrier.subresources.baseArrayLayer;
					layer < barrier.subresources.baseArrayLayer + barrier.subresources.arrayLayerCount;
					++layer) {
					if (!touched.insert({texture, mip, layer}).second) {
						m_Device->SendDebugMessage(DebugSeverity::Error,
							DebugMessageType::General,
							"Overlapping texture barriers must be recorded in separate calls.");
						return;
					}
				}
			}
		}

		// Update the command list's layout knowledge only after all barriers validate.
		for (const TextureBarrier& barrier : barriers.textures) {
			auto* texture = static_cast<VulkanTexture*>(barrier.texture);
			for (uint32_t mip = barrier.subresources.baseMipLevel;
				mip < barrier.subresources.baseMipLevel + barrier.subresources.mipLevelCount;
				++mip) {
				for (uint32_t layer = barrier.subresources.baseArrayLayer;
					layer < barrier.subresources.baseArrayLayer + barrier.subresources.arrayLayerCount;
					++layer) {
					const SubresourceKey key{texture, mip, layer};
					m_RecordedLayouts[key] = barrier.newLayout;
				}
			}
		}

		for (const BufferBarrier& barrier : barriers.buffers) {
			RetainResource(barrier.buffer);
		}
		for (const TextureBarrier& barrier : barriers.textures) {
			RetainResource(barrier.texture);
		}
		m_ResourceOperations.insert(m_ResourceOperations.end(), operations.begin(), operations.end());

		VkDependencyInfo dependencyInfo{
			.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			.memoryBarrierCount = static_cast<uint32_t>(memoryBarriers.size()),
			.pMemoryBarriers = memoryBarriers.data(),
			.bufferMemoryBarrierCount = static_cast<uint32_t>(bufferBarriers.size()),
			.pBufferMemoryBarriers = bufferBarriers.data(),
			.imageMemoryBarrierCount = static_cast<uint32_t>(imageBarriers.size()),
			.pImageMemoryBarriers = imageBarriers.data(),
		};
		vkCmdPipelineBarrier2(m_CommandBuffer, &dependencyInfo);
	}

	void VulkanCommandList::Draw(uint32_t vertexCount) {
		if (GetState() != CommandListState::Recording || !m_Rendering || !m_GraphicsPipelineBound) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"Draw: Requires an active rendering scope and a bound graphics pipeline.");
			return;
		}
		RecordShaderResources("Draw");
		if (m_VertexBuffer) {
			RecordBufferUse(m_VertexBuffer, 0, m_VertexBuffer->GetSize(), "Draw vertex buffer");
		}
		vkCmdDraw(m_CommandBuffer, vertexCount, 1, 0, 0);
	}

	void VulkanCommandList::DrawIndexed(uint32_t indexCount) {
		if (GetState() != CommandListState::Recording || !m_Rendering || !m_GraphicsPipelineBound) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"DrawIndexed: Requires an active rendering scope and a bound graphics pipeline.");
			return;
		}
		RecordShaderResources("DrawIndexed");
		if (m_VertexBuffer) {
			RecordBufferUse(m_VertexBuffer, 0, m_VertexBuffer->GetSize(), "DrawIndexed vertex buffer");
		}
		if (m_IndexBuffer) {
			RecordBufferUse(m_IndexBuffer, 0, m_IndexBuffer->GetSize(), "DrawIndexed index buffer");
		}
		vkCmdDrawIndexed(m_CommandBuffer, indexCount, 1, 0, 0, 0);
	}
}
