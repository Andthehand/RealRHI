#include "Device.h"
#include "RenderGraph/RenderGraph.h"

#include <volk.h>

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {
	using namespace RealRHI;
	uint32_t s_Errors = 0;
	uint32_t s_ExpectedErrors = 0;
	PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR s_SurfaceCapabilities;
	PFN_vkGetPhysicalDeviceSurfaceFormatsKHR s_SurfaceFormats;
	PFN_vkCreateSwapchainKHR s_CreateSwapchain;
	uint32_t s_SwapchainCreations = 0;
	VkPresentModeKHR s_LastPresentMode = VK_PRESENT_MODE_MAX_ENUM_KHR;

	VKAPI_ATTR VkResult VKAPI_CALL CaptureSwapchainCreation(VkDevice device,
		const VkSwapchainCreateInfoKHR* info,
		const VkAllocationCallbacks* allocator,
		VkSwapchainKHR* swapchain) {
		++s_SwapchainCreations;
		s_LastPresentMode = info->presentMode;
		return s_CreateSwapchain(device, info, allocator, swapchain);
	}

	void Expect(bool condition, const char* message) {
		if (!condition) {
			std::cerr << "Test failed: " << message << '\n';
			std::exit(1);
		}
	}

	void Capture(const DebugMessage& message) {
		DefaultDebugCallback(message);
		if (message.severity == DebugSeverity::Error) {
			++s_Errors;
			Expect(message.type == DebugMessageType::General &&
					std::string_view(message.message).starts_with("CreateSwapchain:"),
				"Only intentional swapchain errors occur");
		}
	}

	VKAPI_ATTR VkResult VKAPI_CALL WithoutStorage(VkPhysicalDevice device,
		VkSurfaceKHR surface,
		VkSurfaceCapabilitiesKHR* capabilities) {
		const auto result = s_SurfaceCapabilities(device, surface, capabilities);
		capabilities->supportedUsageFlags &= ~VK_IMAGE_USAGE_STORAGE_BIT;
		return result;
	}

	VKAPI_ATTR VkResult VKAPI_CALL OnlySrgb(VkPhysicalDevice,
		VkSurfaceKHR,
		uint32_t* count,
		VkSurfaceFormatKHR* formats) {
		if (formats && *count) {
			formats[0] = {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
		}
		*count = 1;
		return VK_SUCCESS;
	}

	void TestUnsupportedSurfaces(Device& device, Window* window) {
		Ref<Swapchain> swapchain;
		s_SurfaceCapabilities = vkGetPhysicalDeviceSurfaceCapabilitiesKHR;
		vkGetPhysicalDeviceSurfaceCapabilitiesKHR = WithoutStorage;
		++s_ExpectedErrors;
		Expect(device.CreateSwapchain({.window = window, .enableStorage = true}, swapchain) == Result::Failed &&
				!swapchain,
			"Storage requires surface usage support and publishes no failed swapchain");
		vkGetPhysicalDeviceSurfaceCapabilitiesKHR = s_SurfaceCapabilities;
		s_SurfaceFormats = vkGetPhysicalDeviceSurfaceFormatsKHR;
		vkGetPhysicalDeviceSurfaceFormatsKHR = OnlySrgb;
		++s_ExpectedErrors;
		Expect(device.CreateSwapchain({.window = window, .enableStorage = true}, swapchain) == Result::Failed &&
				!swapchain,
			"Storage rejects surfaces exposing only sRGB formats");
		vkGetPhysicalDeviceSurfaceFormatsKHR = s_SurfaceFormats;
		Expect(s_Errors == s_ExpectedErrors, "Unsupported surfaces report their cause once");
	}

	Ref<Shader> MakeProgram(Device& device, const char* entry) {
		Ref<Shader> shader;
		Expect(device.CreateShader({.moduleName = "backbuffer", .entryPoints = {{entry}}}, shader) == Result::Success,
			"Create backbuffer compute shader");
		return shader;
	}

	void TestRendering(Device& device, Window* window) {
		s_CreateSwapchain = vkCreateSwapchainKHR;
		vkCreateSwapchainKHR = CaptureSwapchainCreation;
		Ref<Swapchain> swapchain;
		Expect(device.CreateSwapchain({.window = window, .enableStorage = true}, swapchain) == Result::Success,
			"Create storage backbuffer swapchain");
		Expect(swapchain->IsVSyncEnabled() && s_LastPresentMode == VK_PRESENT_MODE_FIFO_KHR,
			"VSync defaults to FIFO presentation");
		Expect(swapchain->GetBackBufferFormat() == TextureFormat::RGBA8_UNorm ||
				swapchain->GetBackBufferFormat() == TextureFormat::BGRA8_UNorm,
			"Select a UNorm storage format");
		auto render = MakeProgram(device, "renderMain");
		auto read = MakeProgram(device, "readMain");
		for (uint32_t iteration = 0; iteration < 8; ++iteration) {
			// Change between frames, repeat a setting, and cancel a pending change.
			if (iteration == 1 || iteration == 2 || iteration == 5) {
				swapchain->SetVSync(false);
			}
			if (iteration == 6 || iteration == 7) {
				swapchain->SetVSync(false);
				swapchain->SetVSync(true);
			}
			if (iteration == 4) {
				int count = 0;
				auto** windows = SDL_GetWindows(&count);
				for (int i = 0; i < count; ++i) {
					if (std::string_view(SDL_GetWindowTitle(windows[i])) == "Compute backbuffer tests") {
						Expect(SDL_SetWindowSize(windows[i], 73, 61) && SDL_SyncWindow(windows[i]),
							"Resize storage backbuffer");
					}
				}
				SDL_free(windows);
				SDL_PumpEvents();
			}
			const uint32_t creationsBeforeAcquire = s_SwapchainCreations;
			const auto frame = swapchain->BeginFrame();
			const bool shouldRecreate = iteration == 1 || iteration == 4 || iteration == 5 || iteration == 6;
			Expect(s_SwapchainCreations == creationsBeforeAcquire + (shouldRecreate ? 1u : 0u),
				"VSync changes recreate once at acquisition; repeated and cancelled changes do not");
			const bool expectedVSync = iteration == 0 || iteration == 4 || iteration >= 6;
			Expect(swapchain->IsVSyncEnabled() == expectedVSync, "VSync query reports the requested setting");
			if (expectedVSync) {
				Expect(s_LastPresentMode == VK_PRESENT_MODE_FIFO_KHR, "Enabling VSync restores FIFO presentation");
			}
			if (iteration == 3) {
				const uint32_t creationsBeforeChange = s_SwapchainCreations;
				swapchain->SetVSync(true);
				Expect(swapchain->IsVSyncEnabled() && s_SwapchainCreations == creationsBeforeChange &&
						swapchain->IsFrameReady(frame),
					"Changing VSync with an acquired frame defers recreation and preserves that frame");
			}
			Expect(frame.backBufferView && frame.backBufferView->GetStorageImageDescriptorIndex() != UINT32_MAX,
				"Every acquired backbuffer exposes a storage descriptor, including after resize");
			Expect(swapchain->GetBackBuffer(frame)->GetStorageImageDescriptorIndex() ==
					frame.backBufferView->GetStorageImageDescriptorIndex(),
				"Texture and view expose the same descriptor");
			if (iteration >= 4) {
				Expect(frame.width == 73 && frame.height == 61, "Recreated storage backbuffer follows the window size");
			}
			Ref<Buffer> pixels;
			Expect(device.CreateBuffer({.size = uint64_t(frame.width) * frame.height * sizeof(uint32_t),
										   .usage = BufferUsage::Storage,
										   .memoryUsage = MemoryUsage::GPUToCPU},
					   pixels) == Result::Success,
				"Create host-visible pixel verification buffer");
			RenderGraph graph(Capture);
			graph.SetOptions({.cullUnusedPasses = true});
			const auto image = graph.ImportBackBuffer(*swapchain, frame);
			const auto output = graph.ImportBuffer(pixels.Raw());
			const QueueType queue = iteration % 2 ? QueueType::Compute : QueueType::Graphics;
			const uint32_t parameters[] = {
				frame.backBufferView->GetStorageImageDescriptorIndex(),
				frame.width,
				frame.height,
				pixels->GetDescriptorIndex()
			};
			graph.AddPass({.name = "Compute writes backbuffer",
							  .resources =
								  {{image,
									  {PipelineStage::ComputeShader, ResourceAccess::ShaderWrite},
									  TextureLayout::General,
									  RenderGraphAccess::Write}},
							  .queue = queue},
				[&](CommandList& list) {
					list.BindShaderProgram(render.Raw());
					list.UseResource(frame.backBufferView);
					list.SetPushConstants(parameters, sizeof(parameters));
					list.DispatchCompute((frame.width + 7) / 8, (frame.height + 7) / 8, 1);
				});
			graph.AddPass(
				{.name = "Verify backbuffer pixels",
					.resources =
						{{image, {PipelineStage::ComputeShader, ResourceAccess::ShaderRead}, TextureLayout::General},
							{output,
								{PipelineStage::ComputeShader, ResourceAccess::ShaderWrite},
								TextureLayout::Undefined,
								RenderGraphAccess::Write}},
					.queue = queue},
				[&](CommandList& list) {
					list.BindShaderProgram(read.Raw());
					list.UseResource(frame.backBufferView);
					list.UseResource(pixels.Raw());
					list.SetPushConstants(parameters, sizeof(parameters));
					list.DispatchCompute((frame.width + 7) / 8, (frame.height + 7) / 8, 1);
				});
			graph.SetFinalState(output, {PipelineStage::AllCommands, ResourceAccess::MemoryRead});
			RenderGraphSubmission completion;
			Expect(graph.Submit(device, completion) == Result::Success, "Compute backbuffer submits and presents");
			completion.Wait(device);
			std::vector<uint32_t> values(size_t(frame.width) * frame.height);
			Expect(pixels->ReadData(values.data(), values.size() * sizeof(uint32_t)) == Result::Success,
				"Read backbuffer pixels");
			for (uint32_t y = 0; y < frame.height; ++y) {
				for (uint32_t x = 0; x < frame.width; ++x) {
					const auto value = values[y * frame.width + x];
					const auto red = static_cast<int>(std::lround(255.0 * x / (frame.width - 1)));
					const auto green = static_cast<int>(std::lround(255.0 * y / (frame.height - 1)));
					Expect(std::abs(int(value & 255) - red) <= 1 && std::abs(int((value >> 8) & 255) - green) <= 1 &&
							((value >> 16) & 255) == 64 && (value >> 24) == 255,
						"Compute writes the expected color into every backbuffer pixel");
				}
			}
		}
		device.WaitIdle();
		vkCreateSwapchainKHR = s_CreateSwapchain;
	}
}

int main() {
	std::unique_ptr<RealRHI::Device> device;
	Expect(RealRHI::Device::Create({.applicationName = "Compute backbuffer tests",
									   .shaderDirectory = REALRHI_TEST_SHADER_DIRECTORY,
									   .enableDebug = true,
									   .enableValidationLayers = true,
									   .debugCallback = Capture},
			   device) == RealRHI::Result::Success,
		"Create test device");
	{
		Ref<Window> window;
		Expect(device->CreateWindow({.title = "Compute backbuffer tests", .width = 65, .height = 57}, window) ==
				Result::Success,
			"Create backbuffer test window");
		TestUnsupportedSurfaces(*device, window.Raw());
		TestRendering(*device, window.Raw());
	}
	device.reset();
	Expect(s_Errors == s_ExpectedErrors, "No unexpected errors, including resource cleanup");
	std::cout << "ComputeBackbufferTests passed\n";
}
