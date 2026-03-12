#include "VulkanSwapchain.h"
#include "VulkanConvertions.h"

#include <iostream>
#include <algorithm>

namespace RealRHI {
    VulkanSwapchain::VulkanSwapchain(const VulkanDevice* device) : m_Device(device) {
    }

    VulkanSwapchain::~VulkanSwapchain() {
        vkDeviceWaitIdle(m_Device->GetDevice());

        for (auto& sync : m_FrameSync) {
            vkDestroySemaphore(m_Device->GetDevice(), sync.imageAvailableSemaphore, nullptr);
            vkDestroyFence(m_Device->GetDevice(), sync.fence, nullptr);
        }
        for (auto semaphore : m_RenderFinishedSemaphores) {
            vkDestroySemaphore(m_Device->GetDevice(), semaphore, nullptr);
        }
        if (m_TransitionPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(m_Device->GetDevice(), m_TransitionPool, nullptr);
        }

        vkDestroySwapchainKHR(m_Device->GetDevice(), m_Swapchain, nullptr);
        m_Window->DestroyVulkanSurface(m_Surface);
    }

    Result VulkanSwapchain::Create(const VulkanDevice* device, const SwapchainDesc& desc, Ref<Swapchain>& outSwapchain) {
		Ref<VulkanSwapchain> swapchain = Ref<VulkanSwapchain>::Create(device);
        Result res = swapchain->Init(desc);
        if (res != Result::Success) {
            return res;
        }
        
        outSwapchain = Ref<Swapchain>(swapchain);
        return Result::Success;
	}

    Result VulkanSwapchain::Init(const SwapchainDesc& desc) {
        m_Window = static_cast<const VulkanWindow*>(desc.window);
        if (!m_Window->CreateVulkanSurface(&m_Surface)) {
            m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to create Vulkan surface for swapchain.");
            return Result::Failed;
        }

        SwapChainSupportDetails swapChainSupport = QuerySwapChainSupport();

        VkSurfaceFormatKHR surfaceFormat = ChooseSwapSurfaceFormat(swapChainSupport.Formats);
        VkPresentModeKHR presentMode = ChooseSwapPresentMode(swapChainSupport.PresentModes);
        VkExtent2D requestedExtent = VkExtent2D{ .width = m_Window->GetWidth(), .height = m_Window->GetHeight() };
        VkExtent2D extent = ChooseSwapExtent(swapChainSupport.Capabilities, requestedExtent);

        uint32_t imageCount = swapChainSupport.Capabilities.minImageCount + 1;
        if (swapChainSupport.Capabilities.maxImageCount > 0 &&
            imageCount > swapChainSupport.Capabilities.maxImageCount) {
            imageCount = swapChainSupport.Capabilities.maxImageCount;
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
            .preTransform = swapChainSupport.Capabilities.currentTransform,
            .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            .presentMode = presentMode,
            .clipped = VK_TRUE,
            .oldSwapchain = VK_NULL_HANDLE,
        };

        if (vkCreateSwapchainKHR(m_Device->GetDevice(), &createInfo, nullptr, &m_Swapchain) != VK_SUCCESS) {
            m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to create Vulkan swapchain.");
            return Result::Failed;
        }

        vkGetSwapchainImagesKHR(m_Device->GetDevice(), m_Swapchain, &imageCount, nullptr);
        std::vector<VkImage> swapchainVkImages(imageCount);
        vkGetSwapchainImagesKHR(m_Device->GetDevice(), m_Swapchain, &imageCount, swapchainVkImages.data());

        m_SwapchainImages.resize(imageCount);
        for (uint8_t i = 0; i < imageCount; i++) {
            if (VulkanTexture::CreateFromSwapChain(m_Device, surfaceFormat.format, swapchainVkImages[i], m_SwapchainImages[i]) != Result::Success) {
                return Result::Failed;
            }
        }

        m_SwapchainImageFormat = Utils::VkFormatToTextureFormat(surfaceFormat.format);
        m_SwapchainExtent = extent;

        m_ImageLayouts.assign(imageCount, VK_IMAGE_LAYOUT_UNDEFINED);

        // Create command pool for transition command buffers
        VkCommandPoolCreateInfo poolInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = m_Device->GetGraphicsQueueFamily(),
        };
        if (vkCreateCommandPool(m_Device->GetDevice(), &poolInfo, nullptr, &m_TransitionPool) != VK_SUCCESS) {
            m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to create Vulkan transition command pool for swapchain.");
            return Result::Failed;
        }

        // Allocate pre and post transition command buffers (2 per frame)
        std::vector<VkCommandBuffer> cmdBufs(MAX_FRAMES_IN_FLIGHT * 2);
        VkCommandBufferAllocateInfo allocInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = m_TransitionPool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = static_cast<uint32_t>(cmdBufs.size()),
        };
        if (vkAllocateCommandBuffers(m_Device->GetDevice(), &allocInfo, cmdBufs.data()) != VK_SUCCESS) {
            m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to allocate Vulkan transition command buffers for swapchain.");
            return Result::Failed;
        }

        // Create per-frame sync objects
        m_FrameSync.resize(MAX_FRAMES_IN_FLIGHT);
        VkSemaphoreCreateInfo semInfo{ .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkFenceCreateInfo fenceInfo{
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        };
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            m_FrameSync[i].preCmdBuf = cmdBufs[i * 2];
            m_FrameSync[i].postCmdBuf = cmdBufs[i * 2 + 1];
            if (vkCreateSemaphore(m_Device->GetDevice(), &semInfo, nullptr, &m_FrameSync[i].imageAvailableSemaphore) != VK_SUCCESS ||
                vkCreateFence(m_Device->GetDevice(), &fenceInfo, nullptr, &m_FrameSync[i].fence) != VK_SUCCESS) {
                m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to create Vulkan frame sync objects for swapchain.");
                return Result::Failed;
            }
        }

        // Create per-image renderFinished semaphores
        m_RenderFinishedSemaphores.resize(imageCount);
        for (uint32_t i = 0; i < imageCount; i++) {
            if (vkCreateSemaphore(m_Device->GetDevice(), &semInfo, nullptr, &m_RenderFinishedSemaphores[i]) != VK_SUCCESS) {
                m_Device->SendDebugMessage(DebugSeverity::Error, DebugMessageType::General, "Failed to create Vulkan render finished semaphore for swapchain.");
                return Result::Failed;
            }
        }

        return Result::Success;
    }

    SwapChainSupportDetails VulkanSwapchain::QuerySwapChainSupport() {
        SwapChainSupportDetails details;

        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_Device->GetPhysicalDevice(), m_Surface, &details.Capabilities);

        uint32_t formatCount;
        vkGetPhysicalDeviceSurfaceFormatsKHR(m_Device->GetPhysicalDevice(), m_Surface, &formatCount, nullptr);
        if (formatCount != 0) {
            details.Formats.resize(formatCount);
            vkGetPhysicalDeviceSurfaceFormatsKHR(m_Device->GetPhysicalDevice(), m_Surface, &formatCount, details.Formats.data());
        }

        uint32_t presentModeCount;
        vkGetPhysicalDeviceSurfacePresentModesKHR(m_Device->GetPhysicalDevice(), m_Surface, &presentModeCount, nullptr);
        if (presentModeCount != 0) {
            details.PresentModes.resize(presentModeCount);
            vkGetPhysicalDeviceSurfacePresentModesKHR(m_Device->GetPhysicalDevice(), m_Surface, &presentModeCount, details.PresentModes.data());
        }

        return details;
    }

    VkSurfaceFormatKHR VulkanSwapchain::ChooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats) {
        for (const auto& availableFormat : availableFormats) {
            if (availableFormat.format == VK_FORMAT_B8G8R8A8_SRGB &&
                availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                return availableFormat;
            }
        }

        return availableFormats[0];
    }

    VkPresentModeKHR VulkanSwapchain::ChooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes) {
        for (const auto& availablePresentMode : availablePresentModes) {
            if (availablePresentMode == VK_PRESENT_MODE_MAILBOX_KHR) {
                return availablePresentMode;
            }
        }

        return VK_PRESENT_MODE_FIFO_KHR;
    }

    VkExtent2D VulkanSwapchain::ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, VkExtent2D requestedExtent) {
        if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
            return capabilities.currentExtent;
        }
        else {
            requestedExtent.width = std::clamp(requestedExtent.width,
                capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
            requestedExtent.height = std::clamp(requestedExtent.height,
                capabilities.minImageExtent.height, capabilities.maxImageExtent.height);

            return requestedExtent;
        }
    }

    TextureView* VulkanSwapchain::GetBackBufferView(uint32_t imageIndex) {
        return m_SwapchainImages[imageIndex]->GetTextureView();
    }

    FrameContext VulkanSwapchain::BeginFrame() {
        const uint32_t frameIdx = m_CurrentFrameIndex;

        vkWaitForFences(m_Device->GetDevice(), 1, &m_FrameSync[frameIdx].fence, VK_TRUE, UINT64_MAX);
        vkResetFences(m_Device->GetDevice(), 1, &m_FrameSync[frameIdx].fence);

        uint32_t imageIndex = 0;
        vkAcquireNextImageKHR(m_Device->GetDevice(), m_Swapchain, UINT64_MAX,
            m_FrameSync[frameIdx].imageAvailableSemaphore, VK_NULL_HANDLE, &imageIndex);

        return FrameContext{
            .frameIndex = frameIdx,
            .imageIndex = imageIndex,
            .backBufferView = GetBackBufferView(imageIndex),
            .width = m_SwapchainExtent.width,
            .height = m_SwapchainExtent.height,
        };
    }

    void VulkanSwapchain::Present(const FrameContext& frame) {
        const uint32_t imageIndex = frame.imageIndex;
        VkSemaphore waitSemaphore = m_RenderFinishedSemaphores[imageIndex];

        VkPresentInfoKHR presentInfo{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &waitSemaphore,
            .swapchainCount = 1,
            .pSwapchains = &m_Swapchain,
            .pImageIndices = &imageIndex,
        };

        vkQueuePresentKHR(m_Device->GetPresentQueue(), &presentInfo);
        m_CurrentFrameIndex = (m_CurrentFrameIndex + 1) % MAX_FRAMES_IN_FLIGHT;
    }

    VkCommandBuffer VulkanSwapchain::RecordPreTransitionCmd(uint32_t frameIndex, uint32_t imageIndex) {
        VkCommandBuffer cmd = m_FrameSync[frameIndex].preCmdBuf;
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo beginInfo{ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        vkBeginCommandBuffer(cmd, &beginInfo);
        TransitionToColorAttachment(cmd, imageIndex);
        vkEndCommandBuffer(cmd);
        return cmd;
    }

    VkCommandBuffer VulkanSwapchain::RecordPostTransitionCmd(uint32_t frameIndex, uint32_t imageIndex) {
        VkCommandBuffer cmd = m_FrameSync[frameIndex].postCmdBuf;
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo beginInfo{ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
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
            .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
            .srcAccessMask = VK_ACCESS_2_NONE,
            .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            .oldLayout = m_ImageLayouts[imageIndex],
            .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
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
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
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
