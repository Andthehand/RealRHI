#pragma once

#include "Device.h"
#include "ExampleSupport.h"
#include "FrameResources.h"

#include <SDL3/SDL.h>

#include <array>
#include <condition_variable>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace RealRHI::Examples::Multithreaded {

	constexpr uint32_t WorkerCount = 4;
	constexpr ResourceState SampledState{PipelineStage::FragmentShader, ResourceAccess::ShaderRead};
	constexpr ResourceState ColorState{PipelineStage::ColorAttachmentOutput, ResourceAccess::ColorAttachmentWrite};

	// One persistent thread per recorder. A job is dispatched only after its previous
	// future has joined, so neither command allocators nor graphs are shared concurrently.
	class Worker {
	public:
		Worker()
			: m_Thread([this] {
				  Run();
			  }) { }
		Worker(const Worker&) = delete;
		Worker& operator=(const Worker&) = delete;
		~Worker() {
			{
				std::lock_guard lock(m_Mutex);
				m_Stopping = true;
			}
			m_Ready.notify_one();
			m_Thread.join();
		}

		std::future<bool> Dispatch(std::packaged_task<bool()> job) {
			auto result = job.get_future();
			{
				std::lock_guard lock(m_Mutex);
				m_Job = std::move(job);
			}
			m_Ready.notify_one();
			return result;
		}

	private:
		void Run() {
			for (;;) {
				std::packaged_task<bool()> job;
				{
					std::unique_lock lock(m_Mutex);
					m_Ready.wait(lock, [this] {
						return m_Stopping || m_Job.valid();
					});
					if (!m_Job.valid()) {
						return;
					}
					job = std::move(m_Job);
				}
				job(); // packaged_task transports exceptions to the main thread.
			}
		}

	private:
		std::mutex m_Mutex;
		std::condition_variable m_Ready;
		std::packaged_task<bool()> m_Job;
		bool m_Stopping = false;
		std::thread m_Thread;
	};

	inline void DrawTile(CommandList& commands,
		Texture* target,
		Shader* shader,
		uint32_t width,
		uint32_t height,
		uint32_t worker,
		float time) {
		commands.BeginRendering({
			.colorAttachments =
				{{.target = target->GetTextureView(), .clearColor = {0.02f + worker * 0.025f, 0.025f, 0.06f, 1.0f}}},
			.renderArea = {.width = width, .height = height},
		});
		commands.SetViewport({.width = static_cast<float>(width), .height = static_cast<float>(height)});
		commands.SetScissor({.width = width, .height = height});
		commands.BindShaderProgram(shader);
		commands.SetGraphicsState(
			{.raster = {.cullMode = CullMode::None}, .depth = {.depthTestEnable = false, .depthWriteEnable = false}});
		const float parameters[] = {time, static_cast<float>(worker)};
		commands.SetPushConstants(parameters, sizeof(parameters));
		// Each worker records a grid of animated triangles into its own attachment.
		commands.Draw(3, 256);
		commands.EndRendering();
	}

	inline void DrawComposite(CommandList& commands,
		const FrameContext& frame,
		const std::array<Texture*, WorkerCount>& tiles,
		Shader* shader) {
		commands.BeginRendering({
			.colorAttachments = {{.target = frame.backBufferView}},
			.renderArea = {.width = frame.width, .height = frame.height},
		});
		commands.BindShaderProgram(shader);
		commands.SetGraphicsState(
			{.raster = {.cullMode = CullMode::None}, .depth = {.depthTestEnable = false, .depthWriteEnable = false}});
		for (uint32_t i = 0; i < WorkerCount; ++i) {
			const uint32_t x = (i % 2) * (frame.width / 2);
			const uint32_t y = (i / 2) * (frame.height / 2);
			const uint32_t width = i % 2 ? frame.width - x : frame.width / 2;
			const uint32_t height = i / 2 ? frame.height - y : frame.height / 2;
			commands.SetViewport(
				{.x = static_cast<float>(x),
					.y = static_cast<float>(y),
					.width = static_cast<float>(width),
					.height = static_cast<float>(height)});
			commands.SetScissor(
				{.x = static_cast<int32_t>(x), .y = static_cast<int32_t>(y), .width = width, .height = height});
			// A declaration belongs to this recording, even when a graph tracks barriers.
			commands.UseResource(tiles[i]);
			const uint32_t parameters[] = {
				tiles[i]->GetSampledImageDescriptorIndex(), tiles[i]->GetSamplerDescriptorIndex()
			};
			commands.SetPushConstants(parameters, sizeof(parameters));
			commands.Draw(3);
		}
		commands.EndRendering();
	}

	// The policies in the two examples contain all barrier/graph and presentation
	// differences. Window handling, worker scheduling, and draw contents are identical.
	template <typename Renderer>
	class Application {
		struct Tile {
			Ref<Texture> texture;
			bool initialized = false;
			uint32_t width = 0;
			uint32_t height = 0;
		};
		struct Recorder {
			FrameResources frames;
			std::vector<Tile> tiles;
			CommandList* commands = nullptr;
			std::unique_ptr<Worker> worker;
		};

	public:
		~Application() {
			// Join CPU jobs before waiting for GPU work, including on an error path.
			for (auto& recorder : m_Recorders) {
				recorder.worker.reset();
			}
			if (m_Device) {
				m_Device->WaitIdle();
			}
		}

		bool Init() {
			if (Device::Create({.applicationName = Renderer::Title,
								   .shaderDirectory = "shaders",
								   .enableDebug = true,
								   .enableValidationLayers = true,
								   .debugCallback = CaptureDebugMessage},
					m_Device) != Result::Success ||
				m_Device->CreateWindow({.title = Renderer::Title, .width = 960, .height = 640}, m_Window) !=
					Result::Success ||
				m_Device->CreateSwapchain({.window = m_Window.Raw()}, m_Swapchain) != Result::Success ||
				m_Device->CreateShader({.moduleName = "tiles"}, m_TileShader) != Result::Success ||
				m_Device->CreateShader({.moduleName = "composite"}, m_CompositeShader) != Result::Success) {
				return false;
			}
			const auto slots = m_Swapchain->GetMaxFramesInFlight();
			m_Completions.resize(slots);
			if constexpr (Renderer::RawPresentation) {
				if (m_PresentFrames.Initialize(*m_Device, QueueType::Graphics, slots) != Result::Success) {
					return false;
				}
			}
			for (auto& recorder : m_Recorders) {
				if (recorder.frames.Initialize(*m_Device, QueueType::Graphics, slots) != Result::Success) {
					return false;
				}
				recorder.tiles.resize(slots);
				recorder.worker = std::make_unique<Worker>();
			}
			return true;
		}

		bool Run(uint32_t frameLimit) {
			uint32_t rendered = 0;
			const auto start = SDL_GetTicks();
			while (frameLimit == 0 || rendered < frameLimit) {
				SDL_Event event;
				while (SDL_PollEvent(&event)) {
					if (event.type == SDL_EVENT_QUIT) {
						return frameLimit == 0;
					}
				}
				if (m_Window->IsMinimized() || m_Window->GetWidth() < 2 || m_Window->GetHeight() < 2) {
					SDL_Delay(16);
					continue;
				}
				const auto frame = m_Swapchain->BeginFrame();
				if (!frame.backBufferView) {
					if (m_Window->IsMinimized() || m_Window->GetWidth() < 2 || m_Window->GetHeight() < 2) {
						SDL_Delay(16);
						continue;
					}
					return false;
				}
				const float time = static_cast<float>(SDL_GetTicks() - start) * 0.001f;
				if (!DrawFrame(frame, time)) {
					std::cerr << "Multithreaded frame recording/submission failed.\n";
					return false;
				}
				++rendered;
			}
			std::cout << "Rendered " << rendered << " frames with " << WorkerCount << " recording threads.\n";
			return true;
		}

	private:
		bool RecordTile(uint32_t worker, const FrameContext& frame, float time) {
			auto& recorder = m_Recorders[worker];
			if (recorder.frames.BeginSlot(frame.frameIndex, recorder.commands) != Result::Success) {
				return false;
			}
			auto& tile = recorder.tiles[frame.frameIndex];
			const uint32_t width = frame.width / 2;
			const uint32_t height = frame.height / 2;
			if (!tile.texture || tile.width != width || tile.height != height) {
				Ref<Texture> texture;
				if (m_Device->CreateTexture({.width = width,
												.height = height,
												.format = TextureFormat::RGBA8_UNorm,
												.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource,
												.debugName = "Worker tile"},
						texture) != Result::Success) {
					return false;
				}
				tile = {std::move(texture), false, width, height};
			}
			auto& commands = *recorder.commands;
			if (commands.Begin() != Result::Success) {
				return false;
			}
			const bool recorded = Renderer::RecordTile(*m_Device,
				commands,
				tile.texture.Raw(),
				tile.initialized,
				m_TileShader.Raw(),
				width,
				height,
				worker,
				time);
			const auto ended = commands.End();
			return recorded && ended == Result::Success;
		}

		bool DrawFrame(const FrameContext& frame, float time) {
			auto* queue = m_Device->GetGraphicsCommandQueue();
			const auto previous = m_Completions[frame.frameIndex];
			if (previous.IsValid()) {
				// Wait for the composite reader as well as the worker writers before
				// reusing this slot's textures. Other frame slots can remain in flight.
				queue->Wait(previous);
				if (!queue->IsComplete(previous)) {
					return false;
				}
			}
			std::array<std::future<bool>, WorkerCount> jobs;
			for (uint32_t i = 0; i < WorkerCount; ++i) {
				jobs[i] = m_Recorders[i].worker->Dispatch(std::packaged_task<bool()>([this, i, frame, time] {
					return RecordTile(i, frame, time);
				}));
			}
			bool recorded = true;
			for (auto& job : jobs) {
				// Join every worker, even when an earlier one failed.
				recorded = job.get() && recorded;
			}
			if (!recorded) {
				return false;
			}

			std::array<Texture*, WorkerCount> tiles;
			std::array<SubmissionToken, WorkerCount> ready;
			for (uint32_t i = 0; i < WorkerCount; ++i) {
				auto& recorder = m_Recorders[i];
				// Only the main thread submits/presents. CPU recording is parallel;
				// GPU work uses the graphics queue in deterministic worker order.
				ready[i] = queue->Submit(*recorder.commands);
				if (!ready[i].IsValid() ||
					recorder.frames.TrackSubmission(frame.frameIndex, ready[i]) != Result::Success) {
					return false;
				}
				auto& tile = recorder.tiles[frame.frameIndex];
				tile.initialized = true;
				tiles[i] = tile.texture.Raw();
			}
			return Renderer::Present(*m_Device,
				*m_Swapchain,
				frame,
				m_PresentFrames,
				tiles,
				m_CompositeShader.Raw(),
				ready,
				m_Completions[frame.frameIndex]);
		}

	private:
		std::unique_ptr<Device> m_Device;
		Ref<Window> m_Window;
		Ref<Swapchain> m_Swapchain;
		Ref<Shader> m_TileShader;
		Ref<Shader> m_CompositeShader;
		FrameResources m_PresentFrames;
		std::vector<SubmissionToken> m_Completions;
		std::array<Recorder, WorkerCount> m_Recorders;
	};

	template <typename Renderer>
	int Run(int argc, char** argv) {
		uint32_t limit = 0;
		if (!ParseFrameLimit(argc, argv, limit)) {
			return 1;
		}
		bool succeeded = false;
		try {
			Application<Renderer> application;
			succeeded = application.Init() && application.Run(limit);
		}
		catch (const std::exception& error) {
			std::cerr << error.what() << '\n';
		}
		// Include diagnostics from worker jobs and GPU/resource teardown.
		return succeeded && s_ErrorCount == 0 ? 0 : 1;
	}
}
