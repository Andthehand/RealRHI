#include "../ExampleSupport.h"
#include "Device.h"
#include "RenderGraph/RenderGraph.h"

#include <SDL3/SDL.h>

#include <iostream>
#include <memory>

namespace {
	using namespace RealRHI;

	bool Run(uint32_t frameLimit) {
		std::unique_ptr<Device> device;
		if (Device::Create({.applicationName = "Compute backbuffer example",
							   .shaderDirectory = "shaders",
							   .enableDebug = true,
							   .enableValidationLayers = true,
							   .debugCallback = Examples::CaptureDebugMessage},
				device) != Result::Success) {
			return false;
		}
		Ref<Window> window;
		Ref<Swapchain> swapchain;
		Ref<Shader> shader;
		Ref<Pipeline> pipeline;
		if (device->CreateWindow({.title = "RealRHI Compute Backbuffer", .width = 960, .height = 640}, window) !=
				Result::Success ||
			device->CreateSwapchain({.window = window.Raw(), .enableStorage = true}, swapchain) != Result::Success ||
			device->CreateShader({.moduleName = "backbuffer", .entryPoints = {{"renderMain"}}}, shader) !=
				Result::Success ||
			device->CreateComputePipeline({.shader = shader.Raw()}, pipeline) != Result::Success) {
			return false;
		}
		uint32_t frames = 0;
		bool running = true;
		while (running && (frameLimit == 0 || frames < frameLimit)) {
			SDL_Event event;
			while (SDL_PollEvent(&event)) {
				if (event.type == SDL_EVENT_QUIT) {
					running = false;
				}
			}
			if (!running) {
				break;
			}
			if (window->IsMinimized() || window->GetWidth() == 0 || window->GetHeight() == 0) {
				SDL_Delay(16);
				continue;
			}
			const auto frame = swapchain->BeginFrame();
			if (!frame.backBufferView) {
				device->WaitIdle();
				return false;
			}
			RenderGraph graph(Examples::CaptureDebugMessage);
			const auto backbuffer = graph.ImportBackBuffer(*swapchain, frame);
			graph.AddPass({
				.name = "Compute backbuffer",
				.resources = {{backbuffer, {PipelineStage::ComputeShader, ResourceAccess::ShaderWrite},
								TextureLayout::General, RenderGraphAccess::Write}},
				.queue = QueueType::Compute},
			[&](CommandList& commands) {
				commands.BindPipeline(pipeline.Raw());
				commands.UseResource(frame.backBufferView);
				const uint32_t parameters[] = {
					frame.backBufferView->GetStorageImageDescriptorIndex(),
					frame.width,
					frame.height,
					0,
				};
				commands.SetPushConstants(parameters, sizeof(parameters));
				commands.DispatchCompute((frame.width + 7) / 8, (frame.height + 7) / 8, 1);
			});
			RenderGraphSubmission completion;
			if (graph.Submit(*device, completion) != Result::Success) {
				device->WaitIdle();
				return false;
			}
			++frames;
		}
		device->WaitIdle();
		std::cout << "Rendered " << frames << " frames directly with compute\n";
		return frameLimit == 0 || frames == frameLimit;
	}
}

int main(int argc, char** argv) {
	uint32_t frameLimit = 0;
	if (!RealRHI::Examples::ParseFrameLimit(argc, argv, frameLimit)) {
		return 1;
	}
	const bool succeeded = Run(frameLimit);
	return succeeded && RealRHI::Examples::s_ErrorCount == 0 ? 0 : 1;
}
