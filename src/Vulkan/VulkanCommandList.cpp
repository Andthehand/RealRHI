#include "VulkanCommandList.h"

#include "VulkanBarrierConversions.h"
#include "VulkanBuffer.h"
#include "VulkanCommandQueue.h"
#include "VulkanConversions.h"
#include "VulkanPipeline.h"
#include "VulkanTexture.h"
#include "VulkanTextureView.h"
#include "VulkanTransferValidation.h"

#include <algorithm>
#include <mutex>
#include <unordered_set>
#include <utility>

namespace RealRHI {

	VulkanCommandList::VulkanCommandList(const VulkanDevice* device, Ref<VulkanCommandAllocator> allocator)
		: m_Device(device),
		  m_Allocator(std::move(allocator)),
		  m_QueueId{m_Allocator->GetQueueFamilyIndex(), 0},
		  m_Timestamps(device, m_QueueId) { }

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

		return m_Timestamps.Init();
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
		if (resource == nullptr || GetState() != CommandListState::Recording) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"RetainResource: Requires an active recording and a non-null object.");
			return;
		}

		if (m_ReferencedResources.size() < s_LinearRetentionLimit) {
			for (const auto& retained : m_ReferencedResources) {
				if (retained.Raw() == resource) {
					return;
				}
			}
		}
		else {
			// Rebuild from the owning references when entering the indexed path.
			// Checking sizes also recovers if an earlier allocation threw mid-insertion.
			if (m_RetainedResourceIndex.size() != m_ReferencedResources.size()) {
				m_RetainedResourceIndex.clear();
				if (m_RetainedResourceIndex.bucket_count() < m_ReferencedResources.size() * 2) {
					m_RetainedResourceIndex.reserve(m_ReferencedResources.size() * 2);
				}
				for (auto& retained : m_ReferencedResources) {
					m_RetainedResourceIndex.insert(retained.Raw());
				}
			}
			if (!m_RetainedResourceIndex.insert(resource).second) {
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
		ResetRecordingState();
		m_ReferencedResources.clear();
		return Result::Success;
	}

	void VulkanCommandList::ResetRecordingState() {
		m_RetainedResourceIndex.clear();
		m_ComputePipelineBound = false;
		m_GraphicsPipeline = nullptr;
		m_Rendering = false;
		m_RenderTargetFormats = {};
		m_RenderingSampleCount = 0;
		m_ReadOnlyDepth = false;
		m_ReadOnlyStencil = false;
		m_ActiveVertexBindings.clear();
		m_ResourceTracker.Reset();
		m_ShaderViews.clear();
		m_ShaderBuffers.clear();
		m_VertexBuffers.clear();
		m_IndexBuffer = nullptr;
		m_Timestamps.Reset();
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
		const uint32_t mip = view->GetSubresources().baseMipLevel;
		const auto extent = texture->GetMipExtent(mip);
		const uint32_t width = extent.width;
		const uint32_t height = extent.height;
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

	bool VulkanCommandList::ValidateGraphicsState(const char* operation) const {
		auto reject = [&](const char* reason) {
			m_Device->SendDebugMessage(
				DebugSeverity::Error, DebugMessageType::General, (std::string(operation) + ": " + reason).c_str());
			return false;
		};
		if (GetState() != CommandListState::Recording || !m_Rendering || m_GraphicsPipeline == nullptr) {
			return reject("Requires an active rendering scope and a bound graphics pipeline.");
		}

		const auto& formats = m_GraphicsPipeline->GetRenderTargetFormats();
		if (formats.colorFormats != m_RenderTargetFormats.colorFormats ||
			(formats.depthFormat != m_RenderTargetFormats.depthFormat &&
				m_RenderTargetFormats.depthFormat != TextureFormat::Unknown)) {
			return reject("Pipeline attachment formats must match the active rendering scope.");
		}
		if (m_RenderingSampleCount != 0 && m_GraphicsPipeline->GetSampleCount() != m_RenderingSampleCount) {
			return reject("Pipeline sample count must match the source attachments.");
		}
		if (m_ReadOnlyDepth && m_GraphicsPipeline->WritesDepth()) {
			return reject("A depth-writing pipeline cannot draw into a read-only depth attachment.");
		}
		if (m_ReadOnlyStencil && m_GraphicsPipeline->WritesStencil()) {
			return reject("A stencil-writing pipeline cannot draw into a read-only stencil attachment.");
		}

		return true;
	}

	void VulkanCommandList::BeginRendering(const RenderingInfo& renderingInfo) {
		auto reject = [this](const char* reason) {
			m_Device->SendDebugMessage(
				DebugSeverity::Error, DebugMessageType::General, (std::string("BeginRendering: ") + reason).c_str());
		};
		if (GetState() != CommandListState::Recording || m_Rendering || renderingInfo.renderArea.width == 0 ||
			renderingInfo.renderArea.height == 0 ||
			renderingInfo.colorAttachments.size() > m_Device->GetCapabilities().limits.maxColorAttachments) {
			reject("Requires recording outside rendering, a nonzero area, and a supported color attachment count.");
			return;
		}

		uint32_t sampleCount = 0;
		RenderTargetFormats formats;
		struct AttachmentUse {
			VulkanTextureView* view;
			TextureLayout layout;
		};
		std::vector<AttachmentUse> uses;
		auto addAttachment = [&](TextureView* target, TextureUsage usage, TextureLayout layout, bool resolve) {
			auto* view = static_cast<VulkanTextureView*>(target);
			if (view == nullptr) {
				reject("Attachment requires a texture view.");
				return false;
			}
			if (!ValidateAttachmentView(view, usage, renderingInfo.renderArea)) {
				return false;
			}

			const auto* texture = view->GetTexture();
			const auto range = view->GetSubresources();
			for (const auto& use : uses) {
				const auto other = use.view->GetSubresources();
				if (texture == use.view->GetTexture() && range.baseMipLevel == other.baseMipLevel &&
					range.baseArrayLayer < other.baseArrayLayer + other.arrayLayerCount &&
					other.baseArrayLayer < range.baseArrayLayer + range.arrayLayerCount) {
					reject("Attachments and resolve targets must not overlap.");
					return false;
				}
			}
			if (!resolve) {
				if (sampleCount != 0 && sampleCount != texture->GetSampleCount()) {
					reject("Source attachments must have the same sample count.");
					return false;
				}
				sampleCount = texture->GetSampleCount();
			}
			uses.push_back({view, layout});
			return true;
		};
		auto addResolve = [&](TextureView* source, TextureView* target, TextureUsage usage, TextureLayout layout) {
			auto* sourceView = static_cast<VulkanTextureView*>(source);
			auto* targetView = static_cast<VulkanTextureView*>(target);
			if (targetView == nullptr || sourceView->GetTexture()->GetSampleCount() == 1 ||
				targetView->GetTexture()->GetSampleCount() != 1 ||
				sourceView->GetTexture()->GetFormat() != targetView->GetTexture()->GetFormat()) {
				reject("Resolve requires a multisampled source and a single-sampled target with matching format.");
				return false;
			}
			const auto sourceExtent =
				sourceView->GetTexture()->GetMipExtent(sourceView->GetSubresources().baseMipLevel);
			const auto targetExtent =
				targetView->GetTexture()->GetMipExtent(targetView->GetSubresources().baseMipLevel);
			if (sourceExtent.width != targetExtent.width || sourceExtent.height != targetExtent.height ||
				sourceView->GetSubresources().arrayLayerCount != targetView->GetSubresources().arrayLayerCount) {
				reject("Resolve source and target views require matching extents and layer counts.");
				return false;
			}
			return addAttachment(target, usage, layout, true);
		};

		std::vector<VkRenderingAttachmentInfo> colors;
		for (const auto& attachment : renderingInfo.colorAttachments) {
			if (!addAttachment(attachment.target, TextureUsage::RenderTarget, TextureLayout::ColorAttachment, false)) {
				return;
			}
			auto* view = static_cast<VulkanTextureView*>(attachment.target);
			const auto format = Utils::VkFormatToTextureFormat(view->GetTexture()->GetFormat());
			formats.colorFormats.push_back(format);
			VkRenderingAttachmentInfo info{
				.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
				.imageView = view->GetImageView(),
				.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				.loadOp = Utils::LoadOpToVkLoadOp(attachment.loadOp),
				.storeOp = Utils::StoreOpToVkStoreOp(attachment.storeOp),
				.clearValue = {
					{{attachment.clearColor.r,
						attachment.clearColor.g,
						attachment.clearColor.b,
						attachment.clearColor.a}}
				},
			};
			if (attachment.resolveTarget != nullptr) {
				if (!addResolve(attachment.target,
						attachment.resolveTarget,
						TextureUsage::RenderTarget,
						TextureLayout::ColorAttachment)) {
					return;
				}
				info.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
				if (Utils::IsIntegerColorFormat(format)) {
					info.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
				}
				info.resolveImageView = static_cast<VulkanTextureView*>(attachment.resolveTarget)->GetImageView();
				info.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			}
			colors.push_back(info);
		}

		VkRenderingAttachmentInfo depth{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
		VkRenderingAttachmentInfo stencil{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
		bool hasStencil = false;
		if (renderingInfo.hasDepth) {
			const auto& attachment = renderingInfo.depthAttachment;
			auto* view = static_cast<VulkanTextureView*>(attachment.target);
			if (view == nullptr) {
				reject("Depth/stencil attachment requires a texture view.");
				return;
			}
			formats.depthFormat = Utils::VkFormatToTextureFormat(view->GetTexture()->GetFormat());
			hasStencil = (Utils::TextureFormatToVkImageAspect(formats.depthFormat) & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
			const bool readOnly = attachment.readOnlyDepth && (!hasStencil || attachment.readOnlyStencil);
			const auto layout = readOnly ? TextureLayout::DepthStencilReadOnly : TextureLayout::DepthStencilAttachment;
			if (!addAttachment(attachment.target, TextureUsage::DepthStencil, layout, false)) {
				return;
			}
			if ((attachment.readOnlyDepth && attachment.depthLoadOp == LoadOp::Clear) ||
				(hasStencil && attachment.readOnlyStencil && attachment.stencilLoadOp == LoadOp::Clear)) {
				reject("Read-only depth/stencil aspects cannot use a Clear load operation.");
				return;
			}
			depth.imageView = view->GetImageView();
			depth.imageLayout = readOnly ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
										 : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			depth.loadOp = Utils::LoadOpToVkLoadOp(attachment.depthLoadOp);
			depth.storeOp = Utils::StoreOpToVkStoreOp(attachment.depthStoreOp);
			depth.clearValue.depthStencil = {attachment.clear.depth, attachment.clear.stencil};
			stencil = depth;
			stencil.loadOp = Utils::LoadOpToVkLoadOp(attachment.stencilLoadOp);
			stencil.storeOp = Utils::StoreOpToVkStoreOp(attachment.stencilStoreOp);

			const auto depthMode = static_cast<uint32_t>(attachment.depthResolveMode);
			const auto stencilMode = static_cast<uint32_t>(attachment.stencilResolveMode);
			const auto& support = m_Device->GetCapabilities().depthStencilResolve;
			auto validMode = [](uint32_t mode, uint32_t supported) {
				return mode == 0 || ((mode & (mode - 1)) == 0 && (mode & supported) != 0);
			};
			if (!validMode(depthMode, support.depthModes) || !validMode(stencilMode, support.stencilModes) ||
				(!hasStencil && stencilMode != 0) ||
				((depthMode != 0 || stencilMode != 0) != (attachment.resolveTarget != nullptr)) ||
				(hasStencil && depthMode != stencilMode &&
					((depthMode == 0 || stencilMode == 0) ? !support.independentResolveNone
														  : !support.independentResolve))) {
				reject(
					"Depth/stencil resolve modes must be supported, compatible, and accompanied by a resolve target.");
				return;
			}
			if (attachment.resolveTarget != nullptr) {
				if (!addResolve(attachment.target,
						attachment.resolveTarget,
						TextureUsage::DepthStencil,
						TextureLayout::DepthStencilAttachment)) {
					return;
				}
				const auto resolveView = static_cast<VulkanTextureView*>(attachment.resolveTarget)->GetImageView();
				depth.resolveMode = static_cast<VkResolveModeFlagBits>(depthMode);
				stencil.resolveMode = static_cast<VkResolveModeFlagBits>(stencilMode);
				depth.resolveImageView = resolveView;
				stencil.resolveImageView = resolveView;
				depth.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
				stencil.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			}
		}

		const VkRenderingInfo info{
			.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
			.renderArea =
				{{renderingInfo.renderArea.x, renderingInfo.renderArea.y},
					{renderingInfo.renderArea.width, renderingInfo.renderArea.height}},
			.layerCount = 1,
			.colorAttachmentCount = static_cast<uint32_t>(colors.size()),
			.pColorAttachments = colors.data(),
			.pDepthAttachment = renderingInfo.hasDepth ? &depth : nullptr,
			.pStencilAttachment = hasStencil ? &stencil : nullptr,
		};
		for (const auto& use : uses) {
			RetainResource(use.view);
			RetainResource(const_cast<VulkanTexture*>(use.view->GetTexture()));
			m_ResourceTracker.RecordTextureUse(const_cast<VulkanTexture*>(use.view->GetTexture()),
				use.view->GetSubresources(),
				use.layout,
				"BeginRendering");
		}
		m_RenderTargetFormats = std::move(formats);
		m_RenderingSampleCount = sampleCount;
		m_ReadOnlyDepth = renderingInfo.hasDepth && renderingInfo.depthAttachment.readOnlyDepth;
		m_ReadOnlyStencil = hasStencil && renderingInfo.depthAttachment.readOnlyStencil;
		vkCmdBeginRendering(m_CommandBuffer, &info);
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

	void VulkanCommandList::ClearResourceDeclarations() {
		if (GetState() != CommandListState::Recording) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"ClearResourceDeclarations: Requires an active recording.");
			return;
		}
		m_ShaderBuffers.clear();
		m_ShaderViews.clear();
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
			m_GraphicsPipeline = vulkanPipeline;
			m_ActiveVertexBindings = vulkanPipeline->GetVertexBindings();
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

	void VulkanCommandList::BindVertexBuffer(Buffer* vertexBuffer, uint32_t binding, uint64_t offset) {
		auto* buffer = static_cast<VulkanBuffer*>(vertexBuffer);
		if (GetState() != CommandListState::Recording || buffer == nullptr ||
			!Any(buffer->GetUsage() & BufferUsage::Vertex) || offset >= buffer->GetSize()) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"BindVertexBuffer: Requires recording, Vertex usage, and an offset within the buffer.");
			return;
		}
		if (binding >= m_Device->GetCapabilities().limits.maxVertexInputBindings) {
			m_Device->SendDebugMessage(
				DebugSeverity::Error, DebugMessageType::General, "BindVertexBuffer: Binding exceeds device limit.");
			return;
		}
		RetainResource(vertexBuffer);
		m_VertexBuffers[binding] = buffer;
		VkBuffer vertexBuffers[] = {buffer->GetBuffer()};
		VkDeviceSize offsets[] = {offset};
		vkCmdBindVertexBuffers(m_CommandBuffer, binding, 1, vertexBuffers, offsets);
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
		vkCmdBindIndexBuffer(m_CommandBuffer, buffer->GetBuffer(), 0, VK_INDEX_TYPE_UINT32);
	}

	bool VulkanCommandList::RecordVertexBuffers(const char* operation) {
		for (uint32_t binding : m_ActiveVertexBindings) {
			if (!m_VertexBuffers.contains(binding)) {
				m_Device->SendDebugMessage(DebugSeverity::Error,
					DebugMessageType::General,
					(std::string(operation) + ": Bind a vertex buffer at binding " + std::to_string(binding)).c_str());
				return false;
			}
		}
		for (uint32_t binding : m_ActiveVertexBindings) {
			auto* buffer = m_VertexBuffers.at(binding);
			m_ResourceTracker.RecordBufferUse(buffer, 0, buffer->GetSize(), operation);
		}
		return true;
	}

	void VulkanCommandList::RecordShaderResources(const char* operation) {
		for (auto* buffer : m_ShaderBuffers) {
			m_ResourceTracker.RecordBufferUse(buffer, 0, buffer->GetSize(), operation);
		}
		for (auto* view : m_ShaderViews) {
			TextureLayout layout = TextureLayout::ShaderRead;
			if (Any(view->GetUsage() & TextureUsage::Storage)) {
				layout = TextureLayout::General;
			}

			m_ResourceTracker.RecordTextureUse(
				const_cast<VulkanTexture*>(view->GetTexture()), view->GetSubresources(), layout, operation);
		}
	}

	void VulkanCommandList::Draw(uint32_t vertexCount,
		uint32_t instanceCount,
		uint32_t firstVertex,
		uint32_t firstInstance) {
		if (!ValidateGraphicsState("Draw")) {
			return;
		}

		RecordShaderResources("Draw");
		if (!RecordVertexBuffers("Draw vertex buffer")) {
			return;
		}
		vkCmdDraw(m_CommandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
	}

	void VulkanCommandList::DrawIndexed(uint32_t indexCount,
		uint32_t instanceCount,
		uint32_t firstIndex,
		int32_t vertexOffset,
		uint32_t firstInstance) {
		if (!ValidateGraphicsState("DrawIndexed")) {
			return;
		}

		if (m_IndexBuffer == nullptr || firstIndex > m_IndexBuffer->GetSize() / sizeof(uint32_t) ||
			indexCount > m_IndexBuffer->GetSize() / sizeof(uint32_t) - firstIndex) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"DrawIndexed: Bind an index buffer and select an in-bounds range of 32-bit indices.");

			return;
		}

		RecordShaderResources("DrawIndexed");
		if (!RecordVertexBuffers("DrawIndexed vertex buffer")) {
			return;
		}
		m_ResourceTracker.RecordBufferUse(m_IndexBuffer, 0, m_IndexBuffer->GetSize(), "DrawIndexed index buffer");
		vkCmdDrawIndexed(m_CommandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
	}

	bool VulkanCommandList::ValidateIndirectArguments(const VulkanBuffer* buffer,
		uint64_t offset,
		uint32_t count,
		uint32_t stride,
		uint32_t argumentSize,
		const char* operation) const {
		auto reject = [&](const char* reason) {
			m_Device->SendDebugMessage(
				DebugSeverity::Error, DebugMessageType::General, (std::string(operation) + ": " + reason).c_str());
			return false;
		};

		if (buffer == nullptr || !Any(buffer->GetUsage() & BufferUsage::Indirect)) {
			return reject("Provide a buffer with Indirect usage.");
		}
		if (offset % 4 != 0 || (count > 1 && (stride % 4 != 0 || stride < argumentSize))) {
			return reject(
				"Offset must be 4-byte aligned; multiple draws require an aligned stride at least the argument size.");
		}

		uint64_t size = 0;
		if (count != 0) {
			size = uint64_t(count - 1) * stride + argumentSize;
		}
		if (offset > buffer->GetSize() || size > buffer->GetSize() - offset) {
			return reject("Argument range exceeds the buffer size.");
		}
		if (count > m_Device->GetCapabilities().limits.maxDrawIndirectCount ||
			(count > 1 && !m_Device->GetCapabilities().features.multiDrawIndirect)) {
			return reject("Draw count exceeds maxDrawIndirectCount or requires multiDrawIndirect support.");
		}
		return true;
	}

	void VulkanCommandList::DispatchComputeIndirect(Buffer* arguments, uint64_t offset) {
		static_assert(sizeof(DispatchIndirectArguments) == sizeof(VkDispatchIndirectCommand));
		if (GetState() != CommandListState::Recording || m_Rendering || !m_ComputePipelineBound) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"DispatchComputeIndirect: Requires recording outside rendering and a bound compute pipeline.");
			return;
		}

		auto* buffer = static_cast<VulkanBuffer*>(arguments);
		if (!ValidateIndirectArguments(
				buffer, offset, 1, 0, sizeof(DispatchIndirectArguments), "DispatchComputeIndirect")) {
			return;
		}
		RetainResource(buffer);
		m_ResourceTracker.RecordBufferUse(buffer, offset, sizeof(DispatchIndirectArguments), "DispatchComputeIndirect");
		RecordShaderResources("DispatchComputeIndirect");
		vkCmdDispatchIndirect(m_CommandBuffer, buffer->GetBuffer(), offset);
	}

	void VulkanCommandList::DrawIndirect(Buffer* arguments, uint64_t offset, uint32_t drawCount, uint32_t stride) {
		static_assert(sizeof(DrawIndirectArguments) == sizeof(VkDrawIndirectCommand));
		if (!ValidateGraphicsState("DrawIndirect")) {
			return;
		}

		auto* buffer = static_cast<VulkanBuffer*>(arguments);
		if (!ValidateIndirectArguments(
				buffer, offset, drawCount, stride, sizeof(DrawIndirectArguments), "DrawIndirect") ||
			drawCount == 0 || !RecordVertexBuffers("DrawIndirect vertex buffer")) {
			return;
		}
		RetainResource(buffer);
		m_ResourceTracker.RecordBufferUse(
			buffer, offset, uint64_t(drawCount - 1) * stride + sizeof(DrawIndirectArguments), "DrawIndirect");
		RecordShaderResources("DrawIndirect");
		vkCmdDrawIndirect(m_CommandBuffer, buffer->GetBuffer(), offset, drawCount, stride);
	}

	void VulkanCommandList::DrawIndexedIndirect(Buffer* arguments,
		uint64_t offset,
		uint32_t drawCount,
		uint32_t stride) {
		static_assert(sizeof(DrawIndexedIndirectArguments) == sizeof(VkDrawIndexedIndirectCommand));
		if (!ValidateGraphicsState("DrawIndexedIndirect")) {
			return;
		}

		if (m_IndexBuffer == nullptr) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"DrawIndexedIndirect: Bind an index buffer before drawing.");
			return;
		}

		auto* buffer = static_cast<VulkanBuffer*>(arguments);
		if (!ValidateIndirectArguments(
				buffer, offset, drawCount, stride, sizeof(DrawIndexedIndirectArguments), "DrawIndexedIndirect") ||
			drawCount == 0 || !RecordVertexBuffers("DrawIndexedIndirect vertex buffer")) {
			return;
		}
		RetainResource(buffer);
		m_ResourceTracker.RecordBufferUse(buffer,
			offset,
			uint64_t(drawCount - 1) * stride + sizeof(DrawIndexedIndirectArguments),
			"DrawIndexedIndirect");
		m_ResourceTracker.RecordBufferUse(
			m_IndexBuffer, 0, m_IndexBuffer->GetSize(), "DrawIndexedIndirect index buffer");
		RecordShaderResources("DrawIndexedIndirect");
		vkCmdDrawIndexedIndirect(m_CommandBuffer, buffer->GetBuffer(), offset, drawCount, stride);
	}

	void VulkanCommandList::CopyBuffer(Buffer* source, Buffer* destination, const BufferCopyRegion& region) {
		if (!ValidateTransferRecording("CopyBuffer")) {
			return;
		}
		auto* sourceBuffer = static_cast<VulkanBuffer*>(source);
		auto* destinationBuffer = static_cast<VulkanBuffer*>(destination);
		const VulkanTransferValidation validation(m_Device, m_ResourceTracker);
		VkBufferCopy copy;
		if (!validation.PrepareBufferCopy(sourceBuffer, destinationBuffer, region, copy)) {
			return;
		}
		RetainResource(sourceBuffer);
		RetainResource(destinationBuffer);
		m_ResourceTracker.RecordBufferUse(sourceBuffer, copy.srcOffset, copy.size, "CopyBuffer source");
		m_ResourceTracker.RecordBufferUse(destinationBuffer, copy.dstOffset, copy.size, "CopyBuffer destination");
		vkCmdCopyBuffer(m_CommandBuffer, sourceBuffer->GetBuffer(), destinationBuffer->GetBuffer(), 1, &copy);
	}

	void VulkanCommandList::CopyBufferToTexture(Buffer* source,
		Texture* destination,
		const BufferTextureCopyRegion& region) {
		if (!ValidateTransferRecording("CopyBufferToTexture")) {
			return;
		}
		auto* buffer = static_cast<VulkanBuffer*>(source);
		auto* texture = static_cast<VulkanTexture*>(destination);
		const VulkanTransferValidation validation(m_Device, m_ResourceTracker);
		VulkanBufferTextureCopy copy;
		if (!validation.PrepareBufferTextureCopy(VulkanTransferDirection::Upload, buffer, texture, region, copy)) {
			return;
		}
		RetainResource(buffer);
		RetainResource(texture);
		const auto& subresource = copy.region.imageSubresource;
		m_ResourceTracker.RecordTextureUse(texture,
			{subresource.mipLevel, 1, subresource.baseArrayLayer, subresource.layerCount},
			TextureLayout::TransferDst,
			"CopyBufferToTexture");
		m_ResourceTracker.RecordBufferUse(buffer, copy.region.bufferOffset, copy.byteSize, "CopyBufferToTexture");
		vkCmdCopyBufferToImage(m_CommandBuffer,
			buffer->GetBuffer(),
			texture->GetImage(),
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1,
			&copy.region);
	}

	void VulkanCommandList::CopyTextureToBuffer(Texture* source,
		Buffer* destination,
		const BufferTextureCopyRegion& region) {
		if (!ValidateTransferRecording("CopyTextureToBuffer")) {
			return;
		}
		auto* texture = static_cast<VulkanTexture*>(source);
		auto* buffer = static_cast<VulkanBuffer*>(destination);
		const VulkanTransferValidation validation(m_Device, m_ResourceTracker);
		VulkanBufferTextureCopy copy;
		if (!validation.PrepareBufferTextureCopy(VulkanTransferDirection::Readback, buffer, texture, region, copy)) {
			return;
		}
		RetainResource(texture);
		RetainResource(buffer);
		const auto& subresource = copy.region.imageSubresource;
		m_ResourceTracker.RecordTextureUse(texture,
			{subresource.mipLevel, 1, subresource.baseArrayLayer, subresource.layerCount},
			TextureLayout::TransferSrc,
			"CopyTextureToBuffer");
		m_ResourceTracker.RecordBufferUse(buffer, copy.region.bufferOffset, copy.byteSize, "CopyTextureToBuffer");
		vkCmdCopyImageToBuffer(m_CommandBuffer,
			texture->GetImage(),
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			buffer->GetBuffer(),
			1,
			&copy.region);
	}

	void VulkanCommandList::CopyTexture(Texture* source, Texture* destination, const TextureCopyRegion& region) {
		if (!ValidateTransferRecording("CopyTexture")) {
			return;
		}
		auto* sourceTexture = static_cast<VulkanTexture*>(source);
		auto* destinationTexture = static_cast<VulkanTexture*>(destination);
		const VulkanTransferValidation validation(m_Device, m_ResourceTracker);
		VkImageCopy copy;
		if (!validation.PrepareTextureCopy(sourceTexture, destinationTexture, region, copy)) {
			return;
		}
		RetainResource(sourceTexture);
		RetainResource(destinationTexture);
		m_ResourceTracker.RecordTextureUse(sourceTexture,
			{copy.srcSubresource.mipLevel, 1, copy.srcSubresource.baseArrayLayer, copy.srcSubresource.layerCount},
			TextureLayout::TransferSrc,
			"CopyTexture source");
		m_ResourceTracker.RecordTextureUse(destinationTexture,
			{copy.dstSubresource.mipLevel, 1, copy.dstSubresource.baseArrayLayer, copy.dstSubresource.layerCount},
			TextureLayout::TransferDst,
			"CopyTexture destination");
		vkCmdCopyImage(m_CommandBuffer,
			sourceTexture->GetImage(),
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			destinationTexture->GetImage(),
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1,
			&copy);
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

		if (barriers.memory.empty() && barriers.buffers.empty() && barriers.textures.empty()) {
			return;
		}

		std::vector<VulkanResourceOperation> operations;

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
			if (barrier.acquireFromPresent || barrier.releaseToPresent) {
				if (!texture->IsExternal() || (barrier.acquireFromPresent && barrier.releaseToPresent) ||
					barrier.sourceQueue || barrier.destinationQueue) {
					m_Device->SendDebugMessage(DebugSeverity::Error,
						DebugMessageType::General,
						"Barrier: Presentation ownership flags require a swapchain image, one endpoint, and no "
						"explicit queue roles.");
					return;
				}
				if (barrier.acquireFromPresent) {
					sourceFamily = m_Device->GetPresentQueueFamily();
					destinationFamily = m_QueueId.familyIndex;
				}
				else {
					sourceFamily = m_QueueId.familyIndex;
					destinationFamily = m_Device->GetPresentQueueFamily();
				}
			}
			const bool transfer = sourceFamily != UINT32_MAX && sourceFamily != destinationFamily;
			const bool acquire = transfer && m_QueueId.familyIndex == destinationFamily;
			for (uint32_t mip = barrier.subresources.baseMipLevel;
				mip < barrier.subresources.baseMipLevel + barrier.subresources.mipLevelCount;
				++mip) {
				if (!acquire &&
					!m_ResourceTracker.HasRecordedLayout(texture,
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
		std::unordered_set<VulkanSubresourceKey, VulkanSubresourceKeyHash> touched;
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

		for (const BufferBarrier& barrier : barriers.buffers) {
			RetainResource(barrier.buffer);
		}
		for (const TextureBarrier& barrier : barriers.textures) {
			RetainResource(barrier.texture);
		}
		m_ResourceTracker.RecordBarriers(operations);

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

	void VulkanCommandList::WriteTimestamp(const char* name) {
		if (!SupportsTimestamps()) {
			return;
		}
		if (GetState() != CommandListState::Recording) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"WriteTimestamp: Requires recording, a nonempty name, and fewer than 128 recorded markers.");
			return;
		}
		m_Timestamps.WriteTimestamp(m_CommandBuffer, name);
	}

	uint32_t VulkanCommandList::BeginGpuTiming(const char* name) {
		if (!SupportsTimestamps()) {
			return UINT32_MAX;
		}
		if (GetState() != CommandListState::Recording) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"BeginGpuTiming: Requires an active recording and a nonempty name.");
			return UINT32_MAX;
		}
		return m_Timestamps.BeginGpuTiming(m_CommandBuffer, name);
	}

	void VulkanCommandList::EndGpuTiming(uint32_t timing) {
		if (timing == UINT32_MAX) {
			return;
		}
		if (GetState() != CommandListState::Recording) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"EndGpuTiming: Requires an active recording and an open interval from this recording.");
			return;
		}
		m_Timestamps.EndGpuTiming(m_CommandBuffer, timing);
	}

	bool VulkanCommandList::ReplayResources(VulkanResourceStateTransaction& transaction) const {
		return m_ResourceTracker.Replay(transaction);
	}

	TextureLayout VulkanCommandList::GetExpectedLayout(const VulkanTexture* texture,
		uint32_t mip,
		uint32_t layer) const {
		return m_ResourceTracker.GetExpectedLayout(texture, mip, layer);
	}

	void VulkanCommandList::PrepareSubmission() {
		std::scoped_lock lock(m_StateMutex);
		m_Timestamps.PrepareSubmission();
	}

	void VulkanCommandList::OnSubmitted() {
		std::scoped_lock lock(m_StateMutex);
		m_Timestamps.OnSubmitted();
		m_State = CommandListState::Pending;
	}

	void VulkanCommandList::OnCompleted() {
		std::scoped_lock lock(m_StateMutex);
		if (m_State != CommandListState::Pending) {
			return;
		}
		if (m_Device->IsProfilingEnabled()) {
			// Collect before publishing Executable: another thread may immediately recycle this recording.
			m_Timestamps.OnCompleted();
		}
		m_State = CommandListState::Executable;
	}

	Result VulkanCommandList::GetTimestampResults(std::vector<GpuTimestamp>& outTimestamps) const {
		std::scoped_lock lock(m_StateMutex);
		outTimestamps.clear();
		if (!SupportsTimestamps()) {
			return Result::Success;
		}
		if (m_State != CommandListState::Executable) {
			return Result::NotReady;
		}
		return m_Timestamps.GetResults(outTimestamps);
	}

	bool VulkanCommandList::ValidateDebugLabel(const char* operation, const char* name) const {
		if (GetState() == CommandListState::Recording && name != nullptr && name[0] != '\0') {
			return true;
		}
		const std::string message = std::string(operation) + ": Requires recording and a nonempty label name.";
		m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, message.c_str());
		return false;
	}

	void VulkanCommandList::BeginDebugLabel(const char* name) {
		if (!m_Device->SupportsDebugLabels() || !ValidateDebugLabel("BeginDebugLabel", name)) {
			return;
		}
		const VkDebugUtilsLabelEXT label{
			.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
			.pLabelName = name,
			.color = {0.2f, 0.6f, 0.9f, 1.0f},
		};
		vkCmdBeginDebugUtilsLabelEXT(m_CommandBuffer, &label);
	}

	void VulkanCommandList::EndDebugLabel() {
		if (!m_Device->SupportsDebugLabels()) {
			return;
		}
		if (GetState() != CommandListState::Recording) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"EndDebugLabel: Requires a recording command list and an outstanding label in queue submission order.");
			return;
		}
		vkCmdEndDebugUtilsLabelEXT(m_CommandBuffer);
	}

	void VulkanCommandList::InsertDebugLabel(const char* name) {
		if (!m_Device->SupportsDebugLabels() || !ValidateDebugLabel("InsertDebugLabel", name)) {
			return;
		}
		const VkDebugUtilsLabelEXT label{
			.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
			.pLabelName = name,
			.color = {0.2f, 0.6f, 0.9f, 1.0f},
		};
		vkCmdInsertDebugUtilsLabelEXT(m_CommandBuffer, &label);
	}

	bool VulkanCommandList::ValidateTransferRecording(const char* operation) const {
		if (GetState() != CommandListState::Recording || m_Rendering) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				(std::string(operation) + ": Requires recording outside rendering.").c_str());
			return false;
		}
		return true;
	}

	void VulkanCommandList::GenerateMipmaps(Texture* texture, const MipGenerationDesc& desc) {
		if (!ValidateTransferRecording("GenerateMipmaps")) {
			return;
		}
		auto* vulkanTexture = static_cast<VulkanTexture*>(texture);
		const VulkanTransferValidation validation(m_Device, m_ResourceTracker);
		TextureSubresourceRange range;
		if (!validation.PrepareMipGeneration(vulkanTexture, desc, m_QueueId, range)) {
			return;
		}

		RetainResource(texture);

		const auto transitionMip = [&](uint32_t mip, TextureLayout layout, ResourceState after) {
			BarrierDesc barriers;
			for (uint32_t layer = range.baseArrayLayer; layer < range.baseArrayLayer + range.arrayLayerCount; ++layer) {
				barriers.textures.push_back({
					.texture = texture,
					.before = {PipelineStage::AllCommands, ResourceAccess::MemoryRead | ResourceAccess::MemoryWrite},
					.after = after,
					.oldLayout = m_ResourceTracker.GetExpectedLayout(vulkanTexture, mip, layer),
					.newLayout = layout,
					.subresources = {mip, 1, layer, 1},
				});
			}

			Barrier(barriers);
		};

		VkFilter filter = VK_FILTER_NEAREST;
		if (desc.filter == Filter::Linear) {
			filter = VK_FILTER_LINEAR;
		}

		const uint32_t endMip = range.baseMipLevel + range.mipLevelCount;
		for (uint32_t mip = range.baseMipLevel + 1; mip < endMip; ++mip) {
			transitionMip(mip - 1, TextureLayout::TransferSrc, {PipelineStage::Transfer, ResourceAccess::TransferRead});
			transitionMip(mip, TextureLayout::TransferDst, {PipelineStage::Transfer, ResourceAccess::TransferWrite});

			const auto sourceExtent = vulkanTexture->GetMipExtent(mip - 1);
			const auto destinationExtent = vulkanTexture->GetMipExtent(mip);
			const VkOffset3D sourceEnd{
				.x = static_cast<int32_t>(sourceExtent.width),
				.y = static_cast<int32_t>(sourceExtent.height),
				.z = static_cast<int32_t>(sourceExtent.depth),
			};
			const VkOffset3D destinationEnd{
				.x = static_cast<int32_t>(destinationExtent.width),
				.y = static_cast<int32_t>(destinationExtent.height),
				.z = static_cast<int32_t>(destinationExtent.depth),
			};

			VkImageBlit blit{
				.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, range.baseArrayLayer, range.arrayLayerCount},
				.srcOffsets = {{0, 0, 0}, sourceEnd},
				.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, range.baseArrayLayer, range.arrayLayerCount},
				.dstOffsets = {{0, 0, 0}, destinationEnd},
			};

			m_ResourceTracker.RecordTextureUse(vulkanTexture,
				{mip - 1, 1, range.baseArrayLayer, range.arrayLayerCount},
				TextureLayout::TransferSrc,
				"GenerateMipmaps source");
			m_ResourceTracker.RecordTextureUse(vulkanTexture,
				{mip, 1, range.baseArrayLayer, range.arrayLayerCount},
				TextureLayout::TransferDst,
				"GenerateMipmaps destination");

			vkCmdBlitImage(m_CommandBuffer,
				vulkanTexture->GetImage(),
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				vulkanTexture->GetImage(),
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				1,
				&blit,
				filter);
		}

		for (uint32_t mip = range.baseMipLevel; mip < endMip; ++mip) {
			transitionMip(mip, desc.finalLayout, desc.finalState);
		}
	}
}
