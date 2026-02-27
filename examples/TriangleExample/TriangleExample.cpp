#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include "Vulkan/VulkanDevice.h"
#include "Vulkan/VulkanSwapchain.h"
#include "Vulkan/VulkanShader.h"
#include "Vulkan/VulkanPipeline.h"
#include "Vulkan/VulkanConvertions.h"
#include "Vulkan/VulkanBuffer.h"
#include "Vulkan/VulkanTexture.h"
#include "Vulkan/VulkanCommandList.h"

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
std::vector<RealRHI::TextureView*> swapchainImageViews;

std::unique_ptr<RealRHI::Pipeline> basePipeline;
RealRHI::VulkanPipeline* pipeline = nullptr; //TODO: Remove jank

std::unique_ptr<RealRHI::Buffer> baseVertexBuffer;
RealRHI::VulkanBuffer* vertexBuffer = nullptr; //TODO: Remove jank

std::vector<std::unique_ptr<RealRHI::VulkanCommandList>> commandLists;

std::vector<VkSemaphore> imageAvailableSemaphores; // GPU->GPU synchronization
std::vector<VkSemaphore> renderFinishedSemaphores; // GPU->GPU synchronization
std::vector<VkFence> inFlightFences; // CPU->GPU synchronization
uint32_t currentFrame = 0;

std::unique_ptr<RealRHI::VulkanDevice> device;

void CreateSwapChain() {
	int width, height;

	window = device->CreateWindow({
		.Title = "RealRHI Triangle Example",
		.Width = WIDTH, 
        .Height = HEIGHT 
    });
	RealRHI::SwapchainDesc swapchainDesc {
        .window = window.get(),
    };

    baseSwapchain = device->CreateSwapchain(swapchainDesc);
	swapchain = static_cast<RealRHI::VulkanSwapchain*>(baseSwapchain.get()); //TODO: Remove jank
}

void CreateImageViews() {
    const uint32_t count = static_cast<uint32_t>(swapchain->GetSwapchainImages().size());
    swapchainImageViews.resize(count);
    for (uint32_t i = 0; i < count; i++) {
        swapchainImageViews[i] = swapchain->GetBackBufferView(i);
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
        .shader = shader.get(),
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
    RealRHI::BufferDesc bufferDesc{
        .size = sizeof(vertices[0]) * vertices.size(),
        .usage = RealRHI::BufferUsage::Vertex,
        .memoryUsage = RealRHI::MemoryUsage::CPUToGPU,
        .initialData = vertices.data(),
    };

	baseVertexBuffer = device->CreateBuffer(bufferDesc);
	vertexBuffer = static_cast<RealRHI::VulkanBuffer*>(baseVertexBuffer.get()); //TODO: Remove jank
}

void CreateCommandLists() {
	commandLists.resize(MAX_FRAMES_IN_FLIGHT);
	for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		commandLists[i] = std::make_unique<RealRHI::VulkanCommandList>(device.get());
	}
}

void RecordCommandBuffer(RealRHI::VulkanCommandList* commandList, uint32_t imageIndex) {
	commandList->Begin();

	VkCommandBuffer commandBuffer = commandList->GetCommandBuffer();

	swapchain->TransitionToColorAttachment(commandBuffer, imageIndex);

	RealRHI::RenderingInfo renderingInfo{
		.colorAttachments = {
			{
				.target = swapchainImageViews[imageIndex],
				.loadOp = RealRHI::LoadOp::Clear,
				.storeOp = RealRHI::StoreOp::Store,
				.clearColor = {0.0f, 0.0f, 0.0f, 1.0f}
			}
		},
		.renderArea = {
			.x = 0,
			.y = 0,
			.width = swapchain->GetSwapchainExtent().width,
			.height = swapchain->GetSwapchainExtent().height
		}
	};

	commandList->BeginRendering(renderingInfo);
	commandList->BindPipeline(pipeline);

	RealRHI::Viewport viewport{
		.x = 0.0f,
		.y = 0.0f,
		.width = static_cast<float>(swapchain->GetSwapchainExtent().width),
		.height = static_cast<float>(swapchain->GetSwapchainExtent().height),
		.minDepth = 0.0f,
		.maxDepth = 1.0f,
	};
	commandList->SetViewport(viewport);

	RealRHI::Rect scissor{
		.x = 0,
		.y = 0,
		.width = swapchain->GetSwapchainExtent().width,
		.height = swapchain->GetSwapchainExtent().height,
	};
	commandList->SetScissor(scissor);

	commandList->BindVertexBuffer(vertexBuffer);
	commandList->Draw(static_cast<uint32_t>(vertices.size()));
	commandList->EndRenderPass();

	swapchain->TransitionToPresent(commandBuffer, imageIndex);

    commandList->End();
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

    RecordCommandBuffer(commandLists[currentFrame].get(), imageIndex);

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
        .commandBuffer = commandLists[currentFrame]->GetCommandBuffer(),
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

	basePipeline.reset();

	baseVertexBuffer.reset();
	commandLists.clear();
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

    CreateVertexBuffer();
    std::cout << " Vertex buffer created" << std::endl;

    CreateCommandLists();
    std::cout << " Command lists created" << std::endl;

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
