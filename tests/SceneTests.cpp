#include "../examples/SceneExample/SceneRenderer.h"
#include "FrameContext.h"
#include "FrameResources.h"

#include <SDL3/SDL.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {
	using namespace RealRHI;

	void Expect(bool condition, const char* message) {
		if (!condition) {
			std::cerr << "Test failed: " << message << '\n';
			std::exit(1);
		}
	}

	void CaptureDebugMessage(const DebugMessage& message) {
		DefaultDebugCallback(message);
		Expect(message.severity != DebugSeverity::Error, "Scene has no RHI or Vulkan validation errors");
	}

	void Transition(CommandList& commands, Texture* texture, TextureLayout before, TextureLayout after) {
		BarrierDesc barriers;
		barriers.textures.push_back({
			.texture = texture,
			.before = {PipelineStage::AllCommands, ResourceAccess::MemoryRead | ResourceAccess::MemoryWrite},
			.after = {PipelineStage::AllCommands, ResourceAccess::MemoryRead | ResourceAccess::MemoryWrite},
			.oldLayout = before,
			.newLayout = after,
		});
		commands.Barrier(barriers);
	}

	struct Output {
		Ref<Texture> color;
		Ref<Buffer> readback;
		bool initialized = false;
	};

	std::vector<uint8_t> ReadPixels(const Output& output) {
		std::vector<uint8_t> pixels(output.readback->GetSize());
		Expect(output.readback->ReadData(pixels.data(), pixels.size()) == Result::Success, "Read postprocessed pixels");
		return pixels;
	}

	void CheckScene(const std::vector<uint8_t>& pixels, uint32_t width, uint32_t height) {
		uint32_t redPixels = 0;
		uint32_t bluePixels = 0;
		uint32_t greenPixels = 0;
		for (size_t offset = 0; offset < pixels.size(); offset += 4) {
			const int red = pixels[offset];
			const int green = pixels[offset + 1];
			const int blue = pixels[offset + 2];
			redPixels += red > green * 2 && red > blue * 2 && red > 30;
			bluePixels += blue > red * 2 && blue > green && blue > 30;
			greenPixels += green > red * 1.3f && green > blue && green > 20;
			Expect(pixels[offset + 3] == 255, "Postprocess writes opaque alpha across the output");
		}
		Expect(redPixels > width * height / 200, "Near cube appears in the postprocessed output");
		Expect(bluePixels > width * height / 500, "Far cube appears in the postprocessed output");
		Expect(greenPixels > width * height / 500, "Transparent plane appears in the postprocessed output");

		// The upper image is clear color: check the postprocess color adjustment and vignette analytically.
		const uint32_t x = width / 2;
		const uint32_t y = height / 8;
		const float centeredX = (x + 0.5f) * 2 / width - 1;
		const float centeredY = (y + 0.5f) * 2 / height - 1;
		const float vignette = 1 - 0.22f * (centeredX * centeredX + centeredY * centeredY);
		const float adjustedClear[] = {5 * 1.08f, 8 * 0.97f, 13 * 0.9f};
		for (uint32_t channel = 0; channel < 3; ++channel) {
			const int actual = pixels[(y * width + x) * 4 + channel];
			Expect(std::abs(actual - adjustedClear[channel] * vignette) <= 2,
				"Fullscreen pass samples and transforms the offscreen color");
		}
	}

	bool IsFloorPixel(const std::vector<uint8_t>& pixels, size_t offset) {
		const int red = pixels[offset];
		const int green = pixels[offset + 1];
		const int blue = pixels[offset + 2];
		return red > 15 && green >= red - 2 && blue >= green && blue < red * 1.5f;
	}

	std::vector<bool> CheckShadows(const std::vector<uint8_t>& shadowed,
		const std::vector<uint8_t>& unshadowed,
		uint32_t width,
		uint32_t height) {
		std::vector<bool> floorShadows(width * height);
		uint32_t shadowPixels = 0;
		uint32_t litFloorPixels = 0;

		for (size_t offset = 0; offset < shadowed.size(); offset += 4) {
			int reduction = 0;
			for (uint32_t channel = 0; channel < 3; ++channel) {
				Expect(shadowed[offset + channel] <= unshadowed[offset + channel] + 1,
					"Shadows reduce direct light without brightening pixels or changing geometry");
				reduction += unshadowed[offset + channel] - shadowed[offset + channel];
			}

			// The floor is neutral blue-gray; exclude the saturated cubes, transparent panel, and clear color.
			const bool floor = IsFloorPixel(unshadowed, offset);
			litFloorPixels += floor && reduction <= 3;
			if (floor && reduction > 15) {
				floorShadows[offset / 4] = true;
				++shadowPixels;
			}
		}

		Expect(shadowPixels > width * height / 2000, "Opaque cubes cast visible shadows onto the floor");
		Expect(litFloorPixels > shadowPixels, "Unoccluded floor remains lit outside the cast shadows");
		return floorShadows;
	}

	void TestOffscreenScene(Device& device) {
		constexpr uint32_t slotCount = 3;
		FrameResources frames;
		Expect(frames.Initialize(device, QueueType::Graphics, slotCount) == Result::Success,
			"Initialize three frame slots");

		Examples::SceneRenderer renderer;
		UploadSubmission upload;
		Expect(renderer.Init(device, TextureFormat::RGBA8_UNorm, slotCount, upload) == Result::Success,
			"Initialize scene assets asynchronously");

		std::array<Output, slotCount> outputs;
		std::vector<uint8_t> previous;
		std::vector<uint8_t> staticShadowed;
		std::vector<uint8_t> animatedShadowed;
		std::vector<uint8_t> animatedUnshadowed;
		std::vector<bool> animatedShadowMask;
		uint32_t width = 160;
		uint32_t height = 120;
		for (uint32_t round = 0; round < 8; ++round) {
			if (round == 2) {
				width = 192;
				height = 128;
			}

			for (uint32_t slot = 0; slot < slotCount; ++slot) {
				CommandList* commands = nullptr;
				Expect(frames.BeginSlot(slot, commands) == Result::Success, "Recycle only the completed frame slot");
				Expect(renderer.ResizeTargets(slot, width, height) == Result::Success,
					"Resize per-frame scene attachments");
				auto& output = outputs[slot];
				if (round == 0 || round == 2) {
					output = {};
					Expect(device.CreateTexture(
							   {
								   .width = width,
								   .height = height,
								   .format = TextureFormat::RGBA8_UNorm,
								   .usage = TextureUsage::RenderTarget | TextureUsage::TransferSrc,
							   },
							   output.color) == Result::Success,
						"Create postprocess output");
					Expect(device.CreateBuffer(
							   {
								   .size = static_cast<uint64_t>(width) * height * 4,
								   .usage = BufferUsage::TransferDst,
								   .memoryUsage = MemoryUsage::GPUToCPU,
							   },
							   output.readback) == Result::Success,
						"Create scene readback");
				}

				Expect(commands->Begin() == Result::Success, "Begin scene frame");
				TextureLayout oldLayout = TextureLayout::Undefined;
				if (output.initialized) {
					oldLayout = TextureLayout::TransferSrc;
				}
				Transition(*commands, output.color.Raw(), oldLayout, TextureLayout::ColorAttachment);
				float rotation = 0;
				if (round >= 3 && round <= 5) {
					rotation = 0.8f;
				}
				const bool shadowsEnabled = round >= 2 && round != 4 && round != 6;
				Expect(renderer.Record(
						   *commands, slot, output.color->GetTextureView(), rotation, upload, shadowsEnabled) ==
						Result::Success,
					"Allocate graph targets and record scene");
				Transition(*commands, output.color.Raw(), TextureLayout::ColorAttachment, TextureLayout::TransferSrc);
				commands->CopyTextureToBuffer(
					output.color.Raw(), output.readback.Raw(), {.width = width, .height = height});
				Expect(commands->End() == Result::Success, "End scene frame");

				std::span<const SubmissionToken> waits;
				if (upload.completion.IsValid()) {
					waits = {&upload.completion, 1};
				}
				CommandList* lists[] = {commands};
				const SubmissionToken completion =
					device.GetGraphicsCommandQueue()->Submit({.commandLists = lists, .waitFor = waits});
				Expect(completion.IsValid(), "Submit a scene frame");
				Expect(frames.TrackSubmission(slot, completion) == Result::Success, "Track scene submission");
				upload = {};
				output.initialized = true;
			}

			// Queue all three frames before waiting so each slot's attachments are exercised independently.
			frames.WaitAll();
			const auto pixels = ReadPixels(outputs[0]);
			CheckScene(pixels, width, height);
			Expect(pixels == ReadPixels(outputs[1]) && pixels == ReadPixels(outputs[2]),
				"All frame slots produce identical images");
			if (round == 1) {
				Expect(pixels == previous, "Reusing color and depth attachments preserves deterministic output");
			}
			if (round == 3) {
				Expect(pixels != previous, "Animation changes the rendered geometry");
			}
			if (round == 2) {
				staticShadowed = pixels;
			}
			if (round == 3) {
				animatedShadowed = pixels;
			}
			if (round == 4) {
				animatedShadowMask = CheckShadows(animatedShadowed, pixels, width, height);
				animatedUnshadowed = pixels;
			}
			if (round == 5) {
				Expect(pixels == animatedShadowed,
					"Re-enabling shadows refreshes each frame slot's shadow map deterministically");
			}
			if (round == 6) {
				const auto staticShadowMask = CheckShadows(staticShadowed, pixels, width, height);
				uint32_t movedShadowPixels = 0;
				for (size_t pixel = 0; pixel < staticShadowMask.size(); ++pixel) {
					if (IsFloorPixel(pixels, pixel * 4) && IsFloorPixel(animatedUnshadowed, pixel * 4)) {
						movedShadowPixels += staticShadowMask[pixel] != animatedShadowMask[pixel];
					}
				}
				Expect(movedShadowPixels > 5, "Rotating the cubes moves their floor shadows");
			}
			if (round == 7) {
				Expect(
					pixels == staticShadowed, "Shadow maps are regenerated after animation while shadows are disabled");
			}

			previous = pixels;
		}
	}

	SDL_Window* FindWindow(const char* title) {
		int count = 0;
		SDL_Window** windows = SDL_GetWindows(&count);
		SDL_Window* result = nullptr;
		for (int index = 0; index < count; ++index) {
			if (std::string_view(SDL_GetWindowTitle(windows[index])) == title) {
				result = windows[index];
				break;
			}
		}
		SDL_free(windows);
		Expect(result != nullptr, "Locate test window through SDL's public API");
		return result;
	}

	template <typename Condition>
	void WaitForWindow(Condition condition) {
		const uint64_t deadline = SDL_GetTicks() + 5000;
		while (!condition() && SDL_GetTicks() < deadline) {
			SDL_Event event;
			while (SDL_PollEvent(&event)) {
			}
			SDL_Delay(10);
		}
		Expect(condition(), "Window state changes within five seconds");
	}

	void TestWindowLifecycle(Device& device) {
		constexpr const char* title = "RealRHI Scene Lifecycle Test";
		Ref<Window> window;
		Ref<Swapchain> swapchain;
		Expect(device.CreateWindow({.title = title, .width = 320, .height = 240}, window) == Result::Success,
			"Create lifecycle window");
		Expect(device.CreateSwapchain({.window = window.Raw()}, swapchain) == Result::Success,
			"Create lifecycle swapchain");
		SDL_Window* nativeWindow = FindWindow(title);

		FrameResources frames;
		const uint32_t frameCount = swapchain->GetMaxFramesInFlight();
		Expect(frames.Initialize(device, QueueType::Graphics, frameCount) == Result::Success,
			"Initialize swapchain frame slots");
		Examples::SceneRenderer renderer;
		UploadSubmission upload;
		Expect(renderer.Init(device, swapchain->GetBackBufferFormat(), frameCount, upload) == Result::Success,
			"Initialize swapchain scene");

		for (uint32_t frameNumber = 0; frameNumber < 36; ++frameNumber) {
			if (frameNumber == 8 || frameNumber == 24) {
				int width = 480;
				int height = 320;
				if (frameNumber == 24) {
					width = 256;
					height = 192;
				}
				Expect(SDL_SetWindowSize(nativeWindow, width, height), "Request window resize");
				WaitForWindow([&] {
					int actualWidth = 0;
					int actualHeight = 0;
					return SDL_GetWindowSize(nativeWindow, &actualWidth, &actualHeight) && actualWidth == width &&
						actualHeight == height;
				});
			}
			if (frameNumber == 16) {
				Expect(SDL_MinimizeWindow(nativeWindow), "Request minimize with frames in flight");
				WaitForWindow([&] {
					return window->IsMinimized();
				});
				for (uint32_t attempt = 0; attempt < 3; ++attempt) {
					Expect(swapchain->BeginFrame().backBufferView == nullptr,
						"Minimized swapchain does not acquire or consume frame synchronization");
				}

				Expect(SDL_RestoreWindow(nativeWindow), "Request restore");
				WaitForWindow([&] {
					return !window->IsMinimized();
				});
			}

			const FrameContext frame = swapchain->BeginFrame();
			Expect(frame.backBufferView != nullptr, "Acquire after resize or restore");
			Expect(frame.width == window->GetWidth() && frame.height == window->GetHeight(),
				"Backbuffer extent follows the window");
			CommandList* commands = nullptr;
			Expect(frames.BeginSlot(frame.frameIndex, commands) == Result::Success, "Recycle lifecycle frame slot");
			Expect(renderer.ResizeTargets(frame.frameIndex, frame.width, frame.height) == Result::Success,
				"Match scene targets to resized backbuffer");
			Expect(commands->Begin() == Result::Success, "Begin lifecycle frame");
			Expect(renderer.Record(*commands, frame.frameIndex, frame.backBufferView, frameNumber * 0.03f, upload) ==
					Result::Success,
				"Allocate graph targets and record lifecycle frame");
			Expect(commands->End() == Result::Success, "End lifecycle frame");

			std::span<const SubmissionToken> waits;
			if (upload.completion.IsValid()) {
				waits = {&upload.completion, 1};
			}
			SubmissionToken completion;
			Expect(device.Submit(commands, swapchain.Raw(), frame, &completion, waits) == Result::Success,
				"Submit lifecycle frame");
			Expect(frames.TrackSubmission(frame.frameIndex, completion) == Result::Success, "Track lifecycle frame");
			upload = {};
			swapchain->Present(frame);
		}
		frames.WaitAll();
		device.WaitIdle();
	}
}

int main() {
	std::unique_ptr<RealRHI::Device> device;
	Expect(RealRHI::Device::Create(
			   {
				   .applicationName = "SceneTests",
				   .shaderDirectory = REALRHI_SCENE_SHADER_DIRECTORY,
				   .enableDebug = true,
				   .enableProfiling = false,
				   .enableValidationLayers = true,
				   .debugCallback = CaptureDebugMessage,
			   },
			   device) == RealRHI::Result::Success,
		"Create validated scene test device");

	TestOffscreenScene(*device);
	TestWindowLifecycle(*device);
	device.reset();
	std::cout << "SceneTests passed\n";
}
