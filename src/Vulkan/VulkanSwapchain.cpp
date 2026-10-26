#include "VulkanSwapchain.h"

#include "VulkanCommandQueue.h"
#include "VulkanConversions.h"

#include <algorithm>
#include <atomic>
#include <iostream>
#include <limits>

namespace RealRHI {
	namespace {
		std::atomic<uint64_t> s_NextAcquisitionId{1};
	}
	VulkanSwapchain::VulkanSwapchain(const VulkanDevice* device)
		: m_Device(device) { }

	VulkanSwapchain::~VulkanSwapchain() {
		vkDeviceWaitIdle(m_Device->GetDevice());

		for (auto& sync : m_FrameSync) {
			vkDestroySemaphore(m_Device->GetDevice(), sync.imageAvailableSemaphore, nullptr);
		}
		for (auto semaphore : m_RenderFinishedSemaphores) {
			vkDestroySemaphore(m_Device->GetDevice(), semaphore, nullptr);
		}
		if (m_TransitionPool != VK_NULL_HANDLE) {
			vkDestroyCommandPool(m_Device->GetDevice(), m_TransitionPool, nullptr);
		}

		m_SwapchainImages.clear();
		for (auto& retired : m_RetiredSwapchains) {
			retired.images.clear();
			vkDestroySwapchainKHR(m_Device->GetDevice(), retired.handle, nullptr);
		}
		vkDestroySwapchainKHR(m_Device->GetDevice(), m_Swapchain, nullptr);
		if (m_Window) {
			m_Window->DestroyVulkanSurface(m_Surface);
		}
	}

	Result VulkanSwapchain::Create(const VulkanDevice* device,
		const SwapchainDesc& desc,
		Ref<VulkanSwapchain>& outSwapchain) {
		Ref<VulkanSwapchain> swapchain = Ref<VulkanSwapchain>::Create(device);
		Result res = swapchain->Init(desc);
		if (res != Result::Success) {
			return res;
		}

		outSwapchain = swapchain;
		return Result::Success;
	}

	Result VulkanSwapchain::Init(const SwapchainDesc& desc) {
		m_Window = static_cast<VulkanWindow*>(desc.window);
		if (!m_Window->CreateVulkanSurface(&m_Surface)) {
			m_Device->SendDebugMessage(
				DebugSeverity::Error, DebugMessageType::General, "Failed to create Vulkan surface for swapchain.");
			return Result::Failed;
		}

		if (RecreateSwapchain() != Result::Success) {
			return Result::Failed;
		}

		VkCommandPoolCreateInfo poolInfo{
			.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
			.queueFamilyIndex = m_Device->GetGraphicsQueueFamily(),
		};
		if (vkCreateCommandPool(m_Device->GetDevice(), &poolInfo, nullptr, &m_TransitionPool) != VK_SUCCESS) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"Failed to create Vulkan transition command pool for swapchain.");
			return Result::Failed;
		}

		std::vector<VkCommandBuffer> cmdBufs(s_MaxFramesInFlight * 2);
		VkCommandBufferAllocateInfo allocInfo{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool = m_TransitionPool,
			.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount = static_cast<uint32_t>(cmdBufs.size()),
		};
		if (vkAllocateCommandBuffers(m_Device->GetDevice(), &allocInfo, cmdBufs.data()) != VK_SUCCESS) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"Failed to allocate Vulkan transition command buffers for swapchain.");
			return Result::Failed;
		}

		m_FrameSync.resize(s_MaxFramesInFlight);
		VkSemaphoreCreateInfo semInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};

		for (uint32_t i = 0; i < s_MaxFramesInFlight; i++) {
			m_FrameSync[i].preCmdBuf = cmdBufs[i * 2];
			m_FrameSync[i].postCmdBuf = cmdBufs[i * 2 + 1];
			if (vkCreateSemaphore(m_Device->GetDevice(), &semInfo, nullptr, &m_FrameSync[i].imageAvailableSemaphore) !=
				VK_SUCCESS) {
				m_Device->SendDebugMessage(DebugSeverity::Error,
					DebugMessageType::General,
					"Failed to create Vulkan frame sync objects for swapchain.");
				return Result::Failed;
			}
		}

		return Result::Success;
	}

	SwapChainSupportDetails VulkanSwapchain::QuerySwapChainSupport() {
		SwapChainSupportDetails details;

		vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_Device->GetPhysicalDevice(), m_Surface, &details.capabilities);

		uint32_t formatCount;
		vkGetPhysicalDeviceSurfaceFormatsKHR(m_Device->GetPhysicalDevice(), m_Surface, &formatCount, nullptr);
		if (formatCount != 0) {
			details.formats.resize(formatCount);
			vkGetPhysicalDeviceSurfaceFormatsKHR(
				m_Device->GetPhysicalDevice(), m_Surface, &formatCount, details.formats.data());
		}

		uint32_t presentModeCount;
		vkGetPhysicalDeviceSurfacePresentModesKHR(m_Device->GetPhysicalDevice(), m_Surface, &presentModeCount, nullptr);
		if (presentModeCount != 0) {
			details.presentModes.resize(presentModeCount);
			vkGetPhysicalDeviceSurfacePresentModesKHR(
				m_Device->GetPhysicalDevice(), m_Surface, &presentModeCount, details.presentModes.data());
		}

		return details;
	}

	VkSurfaceFormatKHR VulkanSwapchain::ChooseSwapSurfaceFormat(
		const std::vector<VkSurfaceFormatKHR>& availableFormats) {
		for (const auto& availableFormat : availableFormats) {
			if (availableFormat.format == VK_FORMAT_B8G8R8A8_SRGB &&
				availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
				return availableFormat;
			}
		}

		return availableFormats[0];
	}

	VkPresentModeKHR VulkanSwapchain::ChooseSwapPresentMode(
		const std::vector<VkPresentModeKHR>& availablePresentModes) {
		for (const auto& availablePresentMode : availablePresentModes) {
			if (availablePresentMode == VK_PRESENT_MODE_MAILBOX_KHR) {
				return availablePresentMode;
			}
		}

		return VK_PRESENT_MODE_FIFO_KHR;
	}

	VkExtent2D VulkanSwapchain::ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities,
		VkExtent2D requestedExtent) {
		if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
			return capabilities.currentExtent;
		}
		else {
			requestedExtent.width =
				std::clamp(requestedExtent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
			requestedExtent.height = std::clamp(
				requestedExtent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);

			return requestedExtent;
		}
	}

	Result VulkanSwapchain::RecreateSwapchain() {
		const VkExtent2D requestedExtent{m_Window->GetWidth(), m_Window->GetHeight()};
		if (requestedExtent.width == 0 || requestedExtent.height == 0) {
			return Result::Failed;
		}

		if (vkDeviceWaitIdle(m_Device->GetDevice()) != VK_SUCCESS) {
			return Result::Failed;
		}

		SwapChainSupportDetails support = QuerySwapChainSupport();
		if (support.formats.empty() || support.presentModes.empty()) {
			return Result::Failed;
		}

		const VkSurfaceFormatKHR surfaceFormat = ChooseSwapSurfaceFormat(support.formats);
		const VkPresentModeKHR presentMode = ChooseSwapPresentMode(support.presentModes);
		const VkExtent2D extent = ChooseSwapExtent(support.capabilities, requestedExtent);
		if (extent.width == 0 || extent.height == 0) {
			return Result::Failed;
		}

		uint32_t imageCount = support.capabilities.minImageCount + 1;
		if (support.capabilities.maxImageCount > 0) {
			imageCount = std::min(imageCount, support.capabilities.maxImageCount);
		}

		VkSwapchainCreateInfoKHR createInfo{
			.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
			.surface = m_Surface,
			.minImageCount = imageCount,
			.imageFormat = surfaceFormat.format,
			.imageColorSpace = surfaceFormat.colorSpace,
			.imageExtent = extent,
			.imageArrayLayers = 1,
			.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
			.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
			.preTransform = support.capabilities.currentTransform,
			.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
			.presentMode = presentMode,
			.clipped = VK_TRUE,
			.oldSwapchain = m_Swapchain,
		};

		VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
		if (vkCreateSwapchainKHR(m_Device->GetDevice(), &createInfo, nullptr, &newSwapchain) != VK_SUCCESS) {
			return Result::Failed;
		}

		if (vkGetSwapchainImagesKHR(m_Device->GetDevice(), newSwapchain, &imageCount, nullptr) != VK_SUCCESS ||
			imageCount == 0) {
			vkDestroySwapchainKHR(m_Device->GetDevice(), newSwapchain, nullptr);
			return Result::Failed;
		}

		std::vector<VkImage> images(imageCount);
		if (vkGetSwapchainImagesKHR(m_Device->GetDevice(), newSwapchain, &imageCount, images.data()) != VK_SUCCESS) {
			vkDestroySwapchainKHR(m_Device->GetDevice(), newSwapchain, nullptr);
			return Result::Failed;
		}

		std::vector<Ref<VulkanTexture>> newImages(imageCount);
		for (uint32_t i = 0; i < imageCount; ++i) {
			if (VulkanTexture::CreateFromSwapChain(m_Device, surfaceFormat.format, images[i], extent, newImages[i]) !=
				Result::Success) {
				newImages.clear();
				vkDestroySwapchainKHR(m_Device->GetDevice(), newSwapchain, nullptr);
				return Result::Failed;
			}
		}

		std::vector<VkSemaphore> newSemaphores;
		if (imageCount != m_RenderFinishedSemaphores.size()) {
			newSemaphores.resize(imageCount, VK_NULL_HANDLE);
			const VkSemaphoreCreateInfo semaphoreInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
			for (VkSemaphore& semaphore : newSemaphores) {
				if (vkCreateSemaphore(m_Device->GetDevice(), &semaphoreInfo, nullptr, &semaphore) != VK_SUCCESS) {
					for (VkSemaphore created : newSemaphores) {
						if (created != VK_NULL_HANDLE) {
							vkDestroySemaphore(m_Device->GetDevice(), created, nullptr);
						}
					}
					newImages.clear();
					vkDestroySwapchainKHR(m_Device->GetDevice(), newSwapchain, nullptr);
					return Result::Failed;
				}
			}
		}

		if (m_Swapchain != VK_NULL_HANDLE) {
			m_RetiredSwapchains.push_back({
				.handle = m_Swapchain,
				.images = std::move(m_SwapchainImages),
			});
		}

		m_Swapchain = newSwapchain;
		m_SwapchainImages = std::move(newImages);
		m_SwapchainImageFormat = Utils::VkFormatToTextureFormat(surfaceFormat.format);
		m_SwapchainExtent = extent;
		m_ImageLayouts.assign(imageCount, VK_IMAGE_LAYOUT_UNDEFINED);

		if (!newSemaphores.empty()) {
			for (VkSemaphore semaphore : m_RenderFinishedSemaphores) {
				vkDestroySemaphore(m_Device->GetDevice(), semaphore, nullptr);
			}
			m_RenderFinishedSemaphores = std::move(newSemaphores);
		}

		CollectRetiredSwapchains();
		return Result::Success;
	}

	void VulkanSwapchain::CollectRetiredSwapchains() {
		auto it = m_RetiredSwapchains.begin();
		while (it != m_RetiredSwapchains.end()) {
			bool referenced = false;
			for (const auto& image : it->images) {
				if (image->GetRefCount() > 1) {
					referenced = true;
					break;
				}
			}
			if (referenced) {
				++it;
				continue;
			}

			it->images.clear();
			vkDestroySwapchainKHR(m_Device->GetDevice(), it->handle, nullptr);
			it = m_RetiredSwapchains.erase(it);
		}
	}

	TextureView* VulkanSwapchain::GetBackBufferView(uint32_t imageIndex) {
		return m_SwapchainImages[imageIndex]->GetTextureView();
	}

	FrameContext VulkanSwapchain::BeginFrame() {
		CollectRetiredSwapchains();
		if (m_FrameFailed) {
			return FrameContext{.imageIndex = std::numeric_limits<uint32_t>::max()};
		}
		if (m_Window->IsMinimized() || m_Window->GetWidth() == 0 || m_Window->GetHeight() == 0) {
			return FrameContext{.imageIndex = std::numeric_limits<uint32_t>::max()};
		}
		if (m_Window->GetWidth() != m_SwapchainExtent.width || m_Window->GetHeight() != m_SwapchainExtent.height) {
			if (RecreateSwapchain() != Result::Success) {
				return FrameContext{.imageIndex = std::numeric_limits<uint32_t>::max()};
			}
		}
		const uint32_t frameIdx = m_CurrentFrameIndex;

		auto& sync = m_FrameSync[frameIdx];
		if (sync.acquisitionId != 0) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"BeginFrame: Submit and present the acquired frame before acquiring another.");
			return FrameContext{.imageIndex = UINT32_MAX};
		}
		if (sync.submissionPending) {
			auto* queue = m_Device->GetCommandQueue(sync.completionToken.queue);
			if (queue == nullptr || !sync.completionToken.IsValid()) {
				m_Device->SendDebugMessage(
					DebugSeverity::Error, DebugMessageType::General, "Missing frame completion token.");
				return FrameContext{.imageIndex = std::numeric_limits<uint32_t>::max()};
			}
			queue->Wait(sync.completionToken);
			if (!queue->IsComplete(sync.completionToken)) {
				m_Device->SendDebugMessage(
					DebugSeverity::Error, DebugMessageType::General, "Failed to complete previous frame submission.");
				return FrameContext{.imageIndex = std::numeric_limits<uint32_t>::max()};
			}
			// The final frame token joins GPU work from every queue. Retire completed
			// foreign recordings too, so their transient allocations can be reused.
			for (auto role : {QueueType::Graphics, QueueType::Compute, QueueType::Transfer}) {
				static_cast<VulkanCommandQueue*>(m_Device->GetCommandQueue(role))->ProcessCompletedSubmissions();
			}
			sync.completionToken = {};
			sync.submissionPending = false;
		}
		sync.submitted = false;
		sync.acquireConsumed = false;

		if (m_Swapchain == VK_NULL_HANDLE || m_SwapchainExtent.width == 0 || m_SwapchainExtent.height == 0) {
			return FrameContext{.frameIndex = frameIdx, .imageIndex = std::numeric_limits<uint32_t>::max()};
		}

		uint32_t imageIndex = std::numeric_limits<uint32_t>::max();
		VkResult result = vkAcquireNextImageKHR(m_Device->GetDevice(),
			m_Swapchain,
			UINT64_MAX,
			m_FrameSync[frameIdx].imageAvailableSemaphore,
			VK_NULL_HANDLE,
			&imageIndex);
		if (result == VK_ERROR_OUT_OF_DATE_KHR) {
			if (RecreateSwapchain() != Result::Success) {
				return FrameContext{.imageIndex = std::numeric_limits<uint32_t>::max()};
			}
			if (m_Swapchain == VK_NULL_HANDLE || m_SwapchainExtent.width == 0 || m_SwapchainExtent.height == 0) {
				return FrameContext{.frameIndex = frameIdx, .imageIndex = std::numeric_limits<uint32_t>::max()};
			}

			result = vkAcquireNextImageKHR(m_Device->GetDevice(),
				m_Swapchain,
				UINT64_MAX,
				m_FrameSync[frameIdx].imageAvailableSemaphore,
				VK_NULL_HANDLE,
				&imageIndex);

			m_Device->SendDebugMessage(DebugSeverity::Trace,
				DebugMessageType::General,
				"Swapchain was out of date during image acquisition, recreated swapchain and retried.");

			if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
				m_Device->SendDebugMessage(DebugSeverity::Error,
					DebugMessageType::General,
					"Failed to acquire swapchain image after recreation.");
				return FrameContext{.frameIndex = frameIdx, .imageIndex = std::numeric_limits<uint32_t>::max()};
			}
		}
		else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
			m_Device->SendDebugMessage(
				DebugSeverity::Error, DebugMessageType::General, "Failed to acquire swapchain image.");
			return FrameContext{.frameIndex = frameIdx, .imageIndex = std::numeric_limits<uint32_t>::max()};
		}

		if (imageIndex >= m_SwapchainImages.size()) {
			m_FrameFailed = true;
			return FrameContext{.frameIndex = frameIdx, .imageIndex = std::numeric_limits<uint32_t>::max()};
		}

		sync.acquisitionId = s_NextAcquisitionId++;
		sync.imageIndex = imageIndex;
		return FrameContext{
			.frameIndex = frameIdx,
			.imageIndex = imageIndex,
			.backBufferView = GetBackBufferView(imageIndex),
			.width = m_SwapchainExtent.width,
			.height = m_SwapchainExtent.height,
			.acquisitionId = sync.acquisitionId,
		};
	}

	bool VulkanSwapchain::IsFrameValid(const FrameContext& frame) const {
		return frame.frameIndex < m_FrameSync.size() && frame.imageIndex < m_SwapchainImages.size() &&
			frame.acquisitionId != 0 && frame.acquisitionId == m_FrameSync[frame.frameIndex].acquisitionId &&
			frame.imageIndex == m_FrameSync[frame.frameIndex].imageIndex &&
			frame.backBufferView == m_SwapchainImages[frame.imageIndex]->GetTextureView() &&
			frame.width == m_SwapchainExtent.width && frame.height == m_SwapchainExtent.height;
	}

	bool VulkanSwapchain::IsFrameReady(const FrameContext& frame) const {
		return IsFrameValid(frame) && !m_FrameFailed && !m_FrameSync[frame.frameIndex].acquireConsumed;
	}

	Texture* VulkanSwapchain::GetBackBuffer(const FrameContext& frame) {
		if (!IsFrameValid(frame)) {
			return nullptr;
		}
		return m_SwapchainImages[frame.imageIndex].Raw();
	}

	bool VulkanSwapchain::CanSubmit(const SwapchainSubmitSync& request) const {
		if (!IsFrameValid(request.frame) || m_FrameFailed || (!request.waitForAcquire && !request.signalForPresent)) {
			return false;
		}
		const auto& sync = m_FrameSync[request.frame.frameIndex];
		return !sync.submitted && request.waitForAcquire != sync.acquireConsumed;
	}

	void VulkanSwapchain::FinishSubmission(const SwapchainSubmitSync& request, SubmissionToken token) {
		auto& sync = m_FrameSync[request.frame.frameIndex];
		if (!token.IsValid()) {
			m_FrameFailed = true;
			return;
		}
		sync.acquireConsumed = sync.acquireConsumed || request.waitForAcquire;
		sync.completionToken = token;
		sync.submissionPending = true;
		sync.submitted = request.signalForPresent;
		if (request.signalForPresent) {
			m_ImageLayouts[request.frame.imageIndex] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		}
	}

	bool VulkanSwapchain::PrepareFrameSubmission(uint32_t frameIndex) {
		if (frameIndex >= m_FrameSync.size()) {
			return false;
		}

		auto& sync = m_FrameSync[frameIndex];
		return !m_FrameFailed && sync.acquisitionId != 0 && !sync.acquireConsumed && !sync.submissionPending &&
			!sync.submitted;
	}

	void VulkanSwapchain::FinishFrameSubmission(uint32_t frameIndex, SubmissionToken token) {
		if (frameIndex >= m_FrameSync.size()) {
			return;
		}

		const bool submitted = token.IsValid();
		auto& sync = m_FrameSync[frameIndex];
		sync.completionToken = token;
		sync.submissionPending = submitted;
		sync.submitted = submitted;
		sync.acquireConsumed = submitted;

		if (!submitted) {
			sync.completionToken = {};
			// The acquired image and signaled semaphore cannot be reused after a failed submission.
			m_FrameFailed = true;
		}
	}

	Result VulkanSwapchain::Present(const FrameContext& frame) {
		if (!IsFrameValid(frame)) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"Present: Frame indices, backbuffer view, and extent must match this swapchain.");

			return Result::InvalidParameter;
		}

		if (frame.imageIndex == std::numeric_limits<uint32_t>::max() || frame.backBufferView == nullptr ||
			m_Swapchain == VK_NULL_HANDLE) {
			return Result::InvalidParameter;
		}

		if (m_FrameFailed || frame.frameIndex >= m_FrameSync.size() || !m_FrameSync[frame.frameIndex].submitted) {
			return Result::InvalidParameter;
		}
		const uint32_t imageIndex = frame.imageIndex;
		if (imageIndex >= m_RenderFinishedSemaphores.size()) {
			return Result::InvalidParameter;
		}

		VkSemaphore waitSemaphore = m_RenderFinishedSemaphores[imageIndex];

		VkPresentInfoKHR presentInfo{
			.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
			.waitSemaphoreCount = 1,
			.pWaitSemaphores = &waitSemaphore,
			.swapchainCount = 1,
			.pSwapchains = &m_Swapchain,
			.pImageIndices = &imageIndex,
		};

		VkResult result = vkQueuePresentKHR(m_Device->GetPresentQueue(), &presentInfo);
		if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
			// A minimized window cannot provide a drawable extent. Reacquisition retries after restore.
			if (!m_Window->IsMinimized() && m_Window->GetWidth() != 0 && m_Window->GetHeight() != 0 &&
				RecreateSwapchain() != Result::Success) {
				m_FrameFailed = true;
			}
		}
		else if (result != VK_SUCCESS) {
			m_FrameFailed = true;
		}
		m_CurrentFrameIndex = (m_CurrentFrameIndex + 1) % s_MaxFramesInFlight;
		m_FrameSync[frame.frameIndex].submitted = false;
		m_FrameSync[frame.frameIndex].acquisitionId = 0;
		if (m_FrameFailed) {
			m_Device->SendDebugMessage(DebugSeverity::Error,
				DebugMessageType::General,
				"Present: Vulkan presentation or swapchain recreation failed.");
			return Result::Failed;
		}
		return Result::Success;
	}

	VkCommandBuffer VulkanSwapchain::RecordPreTransitionCmd(uint32_t frameIndex, uint32_t imageIndex) {
		VkCommandBuffer cmd = m_FrameSync[frameIndex].preCmdBuf;
		vkResetCommandBuffer(cmd, 0);
		VkCommandBufferBeginInfo beginInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		vkBeginCommandBuffer(cmd, &beginInfo);
		TransitionToColorAttachment(cmd, imageIndex);
		vkEndCommandBuffer(cmd);
		return cmd;
	}

	VkCommandBuffer VulkanSwapchain::RecordPostTransitionCmd(uint32_t frameIndex, uint32_t imageIndex) {
		VkCommandBuffer cmd = m_FrameSync[frameIndex].postCmdBuf;
		vkResetCommandBuffer(cmd, 0);
		VkCommandBufferBeginInfo beginInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		vkBeginCommandBuffer(cmd, &beginInfo);
		TransitionToPresent(cmd, imageIndex);
		vkEndCommandBuffer(cmd);
		return cmd;
	}

	void VulkanSwapchain::TransitionToColorAttachment(VkCommandBuffer cmdBuf, uint32_t imageIndex) {
		constexpr VkImageSubresourceRange subresourceRange{
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.baseMipLevel = 0,
			.levelCount = 1,
			.baseArrayLayer = 0,
			.layerCount = 1,
		};

		VkImageMemoryBarrier2 barrier{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
			.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
			.srcAccessMask = VK_ACCESS_2_NONE,
			.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
			// The backbuffer may be loaded or blended, in addition to being written.
			.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
			.oldLayout = m_ImageLayouts[imageIndex],
			.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			.srcQueueFamilyIndex = m_Device->GetPresentQueueFamily(),
			.dstQueueFamilyIndex = m_Device->GetGraphicsQueueFamily(),
			.image = m_SwapchainImages[imageIndex]->GetImage(),
			.subresourceRange = subresourceRange,
		};

		VkDependencyInfo depInfo{
			.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			.imageMemoryBarrierCount = 1,
			.pImageMemoryBarriers = &barrier,
		};

		vkCmdPipelineBarrier2(cmdBuf, &depInfo);
		m_ImageLayouts[imageIndex] = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	}

	void VulkanSwapchain::TransitionToPresent(VkCommandBuffer cmdBuf, uint32_t imageIndex) {
		constexpr VkImageSubresourceRange subresourceRange{
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.baseMipLevel = 0,
			.levelCount = 1,
			.baseArrayLayer = 0,
			.layerCount = 1,
		};

		VkImageMemoryBarrier2 barrier{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
			.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
			.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_2_NONE,
			.dstAccessMask = VK_ACCESS_2_NONE,
			.oldLayout = m_ImageLayouts[imageIndex],
			.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
			.srcQueueFamilyIndex = m_Device->GetGraphicsQueueFamily(),
			.dstQueueFamilyIndex = m_Device->GetPresentQueueFamily(),
			.image = m_SwapchainImages[imageIndex]->GetImage(),
			.subresourceRange = subresourceRange,
		};

		VkDependencyInfo depInfo{
			.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			.imageMemoryBarrierCount = 1,
			.pImageMemoryBarriers = &barrier,
		};

		vkCmdPipelineBarrier2(cmdBuf, &depInfo);
		m_ImageLayouts[imageIndex] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	}
}
