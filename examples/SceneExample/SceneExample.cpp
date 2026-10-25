#include "../ExampleSupport.h"
#include "FrameContext.h"
#include "FrameResources.h"
#include "SceneRenderer.h"

#include <SDL3/SDL.h>

#include <iomanip>
#include <iostream>
#include <string_view>

namespace {
	using namespace RealRHI;

	class SceneApplication {
	public:
		~SceneApplication() {
			if (m_Device) {
				m_Device->WaitIdle();
			}
		}

		bool Init(bool validation) {
			DeviceDesc deviceDesc{
				.applicationName = "RealRHI Scene Example",
				.shaderDirectory = "shaders",
				.enableDebug = true,
				.enableValidationLayers = validation,
				.debugCallback = Examples::CaptureDebugMessage,
				.enableProfiling = true,
			};
			if (Device::Create(deviceDesc, m_Device) != Result::Success) {
				return false;
			}

			if (m_Device->CreateWindow(
					{.Title = "RealRHI Scene Example - S: toggle shadows", .Width = 960, .Height = 640}, m_Window) !=
					Result::Success ||
				m_Device->CreateSwapchain({.window = m_Window.Raw()}, m_Swapchain) != Result::Success) {
				return false;
			}

			const uint32_t frameCount = m_Swapchain->GetMaxFramesInFlight();
			return m_Frames.Initialize(*m_Device, QueueType::Graphics, frameCount) == Result::Success &&
				m_Renderer.Init(*m_Device, m_Swapchain->GetBackBufferFormat(), frameCount, m_PendingUpload) ==
				Result::Success;
		}

		bool Run(uint32_t frameLimit) {
			uint32_t framesRendered = 0;
			bool running = true;
			while (running && (frameLimit == 0 || framesRendered < frameLimit)) {
				SDL_Event event;
				while (SDL_PollEvent(&event)) {
					if (event.type == SDL_EVENT_QUIT) {
						running = false;
					}
					if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_S && !event.key.repeat) {
						m_ShadowsEnabled = !m_ShadowsEnabled;
					}
				}
				if (!running) {
					break;
				}

				if (m_Window->IsMinimized() || m_Window->GetWidth() == 0 || m_Window->GetHeight() == 0) {
					SDL_Delay(16);
					continue;
				}

				const FrameContext frame = m_Swapchain->BeginFrame();
				if (frame.backBufferView == nullptr) {
					// Minimization can happen between the drawable check and image acquisition.
					if (m_Window->IsMinimized() || m_Window->GetWidth() == 0 || m_Window->GetHeight() == 0) {
						SDL_Delay(16);
						continue;
					}
					return false;
				}

				if (!DrawFrame(frame, static_cast<float>(SDL_GetTicks()) * 0.0004f)) {
					return false;
				}
				++framesRendered;
			}

			m_Device->WaitIdle();
			PrintStatistics();
			return frameLimit == 0 || framesRendered == frameLimit;
		}

	private:
		void PrintStatistics() const {
			const auto statistics = m_Device->GetStatistics();
			std::cout << std::fixed << std::setprecision(4);
			std::cout << "CPU timings (includes any enabled validation):\n";
			PrintTiming("Upload staging", statistics.uploadStaging);
			PrintTiming("Queue submission", statistics.submissions);
			PrintTiming("Graphics pipeline creation", statistics.graphicsPipelineCreation);

			std::cout << "GPU intervals:\n";
			for (const auto& interval : statistics.gpuTimings) {
				std::cout << "  Queue " << interval.queue.familyIndex << ':' << interval.queue.queueIndex << ' ';
				PrintTiming(interval.name.c_str(), interval.timing);
			}

			std::cout << "Uploaded " << statistics.submittedUploadBytes << " bytes in " << statistics.uploadBatchCount
					  << " batches\n";
			std::cout << "Buffers: " << statistics.buffers.liveCount << " live, " << statistics.buffers.peakBytes
					  << " peak allocation bytes\n";
			std::cout << "Textures: " << statistics.textures.liveCount << " live, " << statistics.textures.peakBytes
					  << " peak allocation bytes\n";
			std::cout << "Resource descriptors: " << statistics.descriptors.resourceBytes << " live bytes, "
					  << statistics.descriptors.resourceReservedBytes << " reserved bytes, "
					  << statistics.descriptors.resourceCapacityBytes << " capacity bytes\n";
			std::cout << "Samplers: " << statistics.descriptors.samplerCount << " / "
					  << statistics.descriptors.samplerCapacity << '\n';
		}

		static void PrintTiming(const char* name, const TimingStatistics& timing) {
			if (timing.sampleCount != 0) {
				std::cout << "  " << name << ": " << timing.sampleCount << " samples, "
						  << timing.totalMilliseconds / timing.sampleCount << " ms average, "
						  << timing.maximumMilliseconds << " ms maximum\n";
			}
		}

		bool DrawFrame(const FrameContext& frame, float rotation) {
			CommandList* commands = nullptr;
			if (m_Frames.BeginSlot(frame.frameIndex, commands) != Result::Success ||
				m_Renderer.ResizeTargets(frame.frameIndex, frame.width, frame.height) != Result::Success ||
				commands->Begin() != Result::Success) {
				return false;
			}

			m_Renderer.Record(
				*commands, frame.frameIndex, frame.backBufferView, rotation, m_PendingUpload, m_ShadowsEnabled);
			if (commands->End() != Result::Success) {
				return false;
			}

			std::span<const SubmissionToken> waits;
			if (m_PendingUpload.completion.IsValid()) {
				waits = {&m_PendingUpload.completion, 1};
			}

			SubmissionToken completion;
			if (m_Device->Submit(commands, m_Swapchain.Raw(), frame, &completion, waits) != Result::Success ||
				m_Frames.TrackSubmission(frame.frameIndex, completion) != Result::Success) {
				return false;
			}
			m_PendingUpload = {};
			m_Swapchain->Present(frame);

			return true;
		}

	private:
		std::unique_ptr<Device> m_Device;
		Ref<Window> m_Window;
		Ref<Swapchain> m_Swapchain;
		FrameResources m_Frames;
		Examples::SceneRenderer m_Renderer;
		UploadSubmission m_PendingUpload;
		bool m_ShadowsEnabled = true;
	};
}

int main(int argc, char** argv) {
	bool validation = true;
	if (argc > 1 && std::string_view(argv[argc - 1]) == "--no-validation") {
		validation = false;
		--argc;
	}
	uint32_t frameLimit = 0;
	if (!RealRHI::Examples::ParseFrameLimit(argc, argv, frameLimit)) {
		return 1;
	}

	bool succeeded = false;
	{
		SceneApplication application;
		succeeded = application.Init(validation) && application.Run(frameLimit);
	}

	if (!succeeded || RealRHI::Examples::s_ErrorCount != 0) {
		std::cerr << "Scene example failed; see the validation output for details.\n";
		return 1;
	}
	return 0;
}
