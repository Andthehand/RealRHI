#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include "Vulkan/VulkanDevice.h"
#include "Vulkan/VulkanSwapchain.h"
#include "Vulkan/VulkanShader.h"
#include "Vulkan/VulkanPipeline.h"
#include "Vulkan/VulkanConvertions.h"

#include <iostream>
#include <vector>
#include <array>
#include <algorithm>
#include <limits>
#include <cstring>

constexpr uint32_t WIDTH = 800;
constexpr uint32_t HEIGHT = 600;
constexpr int MAX_FRAMES_IN_FLIGHT = 2;

struct Vertex {
    float pos[2];
    float color[3];
};

const std::vector<Vertex> vertices = {
    {{0.0f, -0.5f}, {1.0f, 0.0f, 0.0f}},
    {{0.5f, 0.5f},  {0.0f, 1.0f, 0.0f}},
    {{-0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}}
};

// Vulkan objects
std::unique_ptr<RealRHI::Window> window;
std::unique_ptr<RealRHI::Swapchain> baseSwapchain;
RealRHI::VulkanSwapchain* swapchain = nullptr;
std::vector<VkImageView> swapchainImageViews;

std::unique_ptr<RealRHI::Pipeline> basePipeline;
RealRHI::VulkanPipeline* pipeline = nullptr; //TODO: Remove jank

VkBuffer vertexBuffer = VK_NULL_HANDLE;
VkDeviceMemory vertexBufferMemory = VK_NULL_HANDLE;

std::vector<VkCommandBuffer> commandBuffers;
VkCommandPool commandPool = VK_NULL_HANDLE;

std::vector<VkSemaphore> imageAvailableSemaphores; // GPU->GPU synchronization
std::vector<VkSemaphore> renderFinishedSemaphores; // GPU->GPU synchronization
std::vector<VkFence> inFlightFences; // CPU->GPU synchronization
uint32_t currentFrame = 0;

std::unique_ptr<RealRHI::VulkanDevice> device;

void CreateSwapChain() {
	int width, height;

	window = device->CreateWindow({ WIDTH, HEIGHT });
	RealRHI::SwapchainDesc swapchainDesc {
        .window = window.get(),
    };

    baseSwapchain = device->CreateSwapchain(swapchainDesc);
	swapchain = static_cast<RealRHI::VulkanSwapchain*>(baseSwapchain.get()); //TODO: Remove jank
}

void CreateImageViews() {
    swapchainImageViews.resize(swapchain->GetSwapchainImages().size());

    for (size_t i = 0; i < swapchain->GetSwapchainImages().size(); i++) {
        constexpr VkComponentMapping componentMapping{
            .r = VK_COMPONENT_SWIZZLE_IDENTITY,
            .g = VK_COMPONENT_SWIZZLE_IDENTITY,
            .b = VK_COMPONENT_SWIZZLE_IDENTITY,
            .a = VK_COMPONENT_SWIZZLE_IDENTITY,
		};
        constexpr VkImageSubresourceRange subresourceRange{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
		};
        VkImageViewCreateInfo createInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = swapchain->GetSwapchainImages()[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = RealRHI::Utils::TextureFormatToVkFormat(baseSwapchain->GetBackBufferFormat()),
            .components = componentMapping,
            .subresourceRange = subresourceRange,
        };

        if (vkCreateImageView(device->GetDevice(), &createInfo, nullptr, &swapchainImageViews[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create image views!");
        }
    }
}

void CreateGraphicsPipeline() {
    const RealRHI::ShaderDesc shaderDesc{
        .moduleName = "shader",
        .entryPoints = {
			{ .entryPoint = "main", .stage = RealRHI::ShaderStage::Vertex },
            { .entryPoint = "main", .stage = RealRHI::ShaderStage::Fragment },
        }
	};
    std::unique_ptr<RealRHI::Shader> shader = device->CreateShader(shaderDesc);

    RealRHI::PipelineDesc desc{
        .shader = std::move(shader),
        .vertexLayout = {
            RealRHI::DataType::Float2, // pos
            RealRHI::DataType::Float3, // color
        },
        .rasterState = {
            .cullMode = RealRHI::CullMode::Back,
            .fillMode = RealRHI::FillMode::Solid,
            .frontCounterClockwise = false,
		},
        .depthState = {
            .depthTestEnable = false,
            .depthWriteEnable = false,
			.compareOp = RealRHI::CompareOp::Less,
        },
        .blendState = {
			.enable = false,
        },
        .renderTargetFormats = {
            .colorFormats = { baseSwapchain->GetBackBufferFormat() },
            .depthFormat = RealRHI::TextureFormat::Unknown,
        },
    };

	basePipeline = device->CreateGraphicsPipeline(desc);
	pipeline = static_cast<RealRHI::VulkanPipeline*>(basePipeline.get()); //TODO: Remove jank
}

uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(device->GetPhysicalDevice(), &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    throw std::runtime_error("Failed to find suitable memory type!");
}

void CreateVertexBuffer() {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = sizeof(vertices[0]) * vertices.size();
    bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device->GetDevice(), &bufferInfo, nullptr, &vertexBuffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create vertex buffer!");
    }

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(device->GetDevice(), vertexBuffer, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits, 
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(device->GetDevice(), &allocInfo, nullptr, &vertexBufferMemory) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate vertex buffer memory!");
    }

    vkBindBufferMemory(device->GetDevice(), vertexBuffer, vertexBufferMemory, 0);

    void* data;
    vkMapMemory(device->GetDevice(), vertexBufferMemory, 0, bufferInfo.size, 0, &data);
    memcpy(data, vertices.data(), (size_t) bufferInfo.size);
    vkUnmapMemory(device->GetDevice(), vertexBufferMemory);
}

void CreateCommandPool() {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = device->GetGraphicsQueueFamily();

    if (vkCreateCommandPool(device->GetDevice(), &poolInfo, nullptr, &commandPool) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create command pool!");
    }
}

void CreateCommandBuffers() {
    commandBuffers.resize(MAX_FRAMES_IN_FLIGHT);

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = (uint32_t) commandBuffers.size();

    if (vkAllocateCommandBuffers(device->GetDevice(), &allocInfo, commandBuffers.data()) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate command buffers!");
    }
}

void RecordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex) {
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
        throw std::runtime_error("Failed to begin recording command buffer!");
    }

    constexpr VkImageSubresourceRange subresourceRange {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = 1
	};

    VkImageMemoryBarrier2 imageBarrierToRender{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
        .srcAccessMask = VK_ACCESS_2_NONE,
        .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = swapchain->GetSwapchainImages()[imageIndex],
        .subresourceRange = subresourceRange
    };

    VkDependencyInfo depInfoToRender{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &imageBarrierToRender
    };

    vkCmdPipelineBarrier2(commandBuffer, &depInfoToRender);

    VkClearValue clearColor = { {{0.0f, 0.0f, 0.0f, 1.0f}} };
    VkRenderingAttachmentInfo colorAttachmentInfo {
		.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
		.imageView = swapchainImageViews[imageIndex],
		.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.resolveMode = VK_RESOLVE_MODE_NONE,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.clearValue = clearColor
    };

	VkRect2D renderArea {
        .offset = {0, 0},
        .extent = swapchain->GetSwapchainExtent()
	};

    VkRenderingInfo renderPassInfo{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = renderArea,
		.layerCount = 1,
		.colorAttachmentCount = 1,
		.pColorAttachments = &colorAttachmentInfo
    };

	vkCmdBeginRendering(commandBuffer, &renderPassInfo);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->GetPipeline());

	// Dynamic Viewport and Scissor
    VkViewport viewport{
        .width = static_cast<float>(swapchain->GetSwapchainExtent().width),
        .height = static_cast<float>(swapchain->GetSwapchainExtent().height),
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
	};
    VkRect2D scissor{
        .offset = {0, 0},
        .extent = swapchain->GetSwapchainExtent(),
	};
	vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
	vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    VkBuffer vertexBuffers[] = {vertexBuffer};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);

    vkCmdDraw(commandBuffer, static_cast<uint32_t>(vertices.size()), 1, 0, 0);
	vkCmdEndRendering(commandBuffer);

    VkImageMemoryBarrier2 imageBarrierToPresent{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_NONE,
        .dstAccessMask = VK_ACCESS_2_NONE,
        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = swapchain->GetSwapchainImages()[imageIndex],
        .subresourceRange = subresourceRange,
    };

    VkDependencyInfo depInfoToPresent{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &imageBarrierToPresent
    };

    vkCmdPipelineBarrier2(commandBuffer, &depInfoToPresent);

    if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to record command buffer!");
    }
}

void CreateSyncObjects() {
    // imageAvailableSemaphores and fences are per-frame-in-flight
    imageAvailableSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
    inFlightFences.resize(MAX_FRAMES_IN_FLIGHT);
    // renderFinishedSemaphores are per swapchain image to avoid reuse before presentation completes
    renderFinishedSemaphores.resize(swapchain->GetSwapchainImages().size());

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (vkCreateSemaphore(device->GetDevice(), &semaphoreInfo, nullptr, &imageAvailableSemaphores[i]) != VK_SUCCESS ||
            vkCreateFence(device->GetDevice(), &fenceInfo, nullptr, &inFlightFences[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create synchronization objects!");
        }
    }

    for (size_t i = 0; i < renderFinishedSemaphores.size(); i++) {
        if (vkCreateSemaphore(device->GetDevice(), &semaphoreInfo, nullptr, &renderFinishedSemaphores[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create render finished semaphores!");
        }
    }
}

void DrawFrame() {
    vkWaitForFences(device->GetDevice(), 1, &inFlightFences[currentFrame], VK_TRUE, UINT64_MAX);
    vkResetFences(device->GetDevice(), 1, &inFlightFences[currentFrame]);

    uint32_t imageIndex;
    vkAcquireNextImageKHR(device->GetDevice(), swapchain->GetSwapchain(), UINT64_MAX,
        imageAvailableSemaphores[currentFrame], VK_NULL_HANDLE, &imageIndex);

    vkResetCommandBuffer(commandBuffers[currentFrame], 0);
    RecordCommandBuffer(commandBuffers[currentFrame], imageIndex);

    VkSemaphoreSubmitInfo waitSemaphoreInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = imageAvailableSemaphores[currentFrame],
        .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    };

    VkSemaphoreSubmitInfo signalSemaphoreInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = renderFinishedSemaphores[imageIndex],
        .stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,
    };

    VkCommandBufferSubmitInfo cmdBufferInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = commandBuffers[currentFrame],
    };

    VkSubmitInfo2 submitInfo{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = 1,
        .pWaitSemaphoreInfos = &waitSemaphoreInfo,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &cmdBufferInfo,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos = &signalSemaphoreInfo,
    };

    if (vkQueueSubmit2(device->GetGraphicsQueue(), 1, &submitInfo, inFlightFences[currentFrame]) != VK_SUCCESS) {
        throw std::runtime_error("Failed to submit draw command buffer!");
    }

    VkPresentInfoKHR presentInfo{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &renderFinishedSemaphores[imageIndex],
    };

    VkSwapchainKHR swapChains[] = { swapchain->GetSwapchain() };
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = swapChains;
    presentInfo.pImageIndices = &imageIndex;

    vkQueuePresentKHR(device->GetPresentQueue(), &presentInfo);

    currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
}

void Cleanup() {
    vkDeviceWaitIdle(device->GetDevice());

    for (size_t i = 0; i < renderFinishedSemaphores.size(); i++) {
        vkDestroySemaphore(device->GetDevice(), renderFinishedSemaphores[i], nullptr);
    }

    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        vkDestroySemaphore(device->GetDevice(), imageAvailableSemaphores[i], nullptr);
        vkDestroyFence(device->GetDevice(), inFlightFences[i], nullptr);
    }

    vkDestroyCommandPool(device->GetDevice(), commandPool, nullptr);

	basePipeline.reset();

    for (auto imageView : swapchainImageViews) {
        vkDestroyImageView(device->GetDevice(), imageView, nullptr);
    }

    vkDestroyBuffer(device->GetDevice(), vertexBuffer, nullptr);
    vkFreeMemory(device->GetDevice(), vertexBufferMemory, nullptr);

	baseSwapchain.reset();
    device.reset();
}

int main() {
    std::cout << "RealRHI Triangle Example - Simple Triangle Rendering" << std::endl;
    std::cout << "====================================================" << std::endl;

    // Create and initialize device
    // NOTE: Validation layers may fail in headless/GPU-less environments
    RealRHI::DeviceDesc createInfo {
        .applicationName = "RealRHI Triangle Example",
        .shaderDirectory = "shaders",
        .enableDebug = true,
        .enableValidationLayers = true,
    };

	device = std::make_unique<RealRHI::VulkanDevice>(createInfo);

    std::cout << " Device created successfully" << std::endl;

    CreateSwapChain();
    std::cout << " Swapchain created" << std::endl;

    CreateImageViews();
    std::cout << " Image views created" << std::endl;

    CreateGraphicsPipeline();
    std::cout << " Graphics pipeline created" << std::endl;

    CreateCommandPool();
    std::cout << " Command pool created" << std::endl;

    CreateVertexBuffer();
    std::cout << " Vertex buffer created" << std::endl;

    CreateCommandBuffers();
    std::cout << " Command buffers created" << std::endl;

    CreateSyncObjects();
    std::cout << " Sync objects created" << std::endl;

    std::cout << std::endl << "Rendering triangle..." << std::endl;

    // Main loop
    bool running = true;
    SDL_Event event;
    while (running) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            }
        }
        DrawFrame();
    }

    Cleanup();

    std::cout << std::endl << "Program completed successfully!" << std::endl;
    return 0;
}
