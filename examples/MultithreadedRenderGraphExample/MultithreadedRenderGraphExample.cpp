#include "../MultithreadedSupport.h"
#include "RenderGraph/RenderGraph.h"

using namespace RealRHI;
using namespace RealRHI::Examples::Multithreaded;

namespace {
	struct GraphRenderer {
		static constexpr const char* Title = "RealRHI Multithreaded - Render Graph";
		static constexpr bool RawPresentation = false;

		static bool RecordTile(Device& device,
			CommandList& commands,
			Texture* tile,
			bool initialized,
			Shader* shader,
			uint32_t width,
			uint32_t height,
			uint32_t worker,
			float time) {
			// Each worker owns a separate graph. A single graph must not be accessed
			// concurrently, and Execute records that graph's passes serially on this worker.
			RenderGraph graph(Examples::CaptureDebugMessage);
			const auto target = graph.ImportTexture(tile,
				initialized ? TextureLayout::ShaderRead : TextureLayout::Undefined,
				initialized ? SampledState : ResourceState{});
			graph.AddPass({.name = "Worker tile", .resources = {RenderGraphUse::ColorAttachmentWrite(target)}},
				[=](RenderGraphContext& context) {
					DrawTile(context.GetCommands(), context.GetTexture(target), shader, width, height, worker, time);
				});
			// Export the actual state consumed by the main thread's composite graph.
			graph.SetFinalState(target, SampledState, TextureLayout::ShaderRead);
			return graph.Execute(device, commands) == Result::Success;
		}

		static bool Present(Device& device,
			Swapchain& swapchain,
			const FrameContext& frame,
			FrameResources&,
			const std::array<Texture*, WorkerCount>& tiles,
			Shader* shader,
			std::span<const SubmissionToken> ready,
			SubmissionToken& completion) {
			RenderGraph graph(Examples::CaptureDebugMessage);
			graph.SetOptions({.cullUnusedPasses = true});
			const auto backbuffer = graph.ImportBackBuffer(swapchain, frame);
			std::vector<RenderGraphUse> uses{RenderGraphUse::ColorAttachmentWrite(backbuffer)};
			std::array<RenderGraphResource, WorkerCount> inputs;
			for (uint32_t i = 0; i < WorkerCount; ++i) {
				inputs[i] = graph.ImportTexture(tiles[i], TextureLayout::ShaderRead, SampledState);
				uses.push_back(RenderGraphUse::SampledRead(inputs[i]));
			}
			graph.AddPass(
				{.name = "Composite worker tiles", .resources = std::move(uses)}, [=](RenderGraphContext& context) {
					std::array<Texture*, WorkerCount> resolved;
					for (uint32_t i = 0; i < WorkerCount; ++i) {
						resolved[i] = context.GetTexture(inputs[i]);
					}
					DrawComposite(context.GetCommands(), frame, resolved, shader);
				});
			RenderGraphSubmission submission;
			// Tokens connect the independent worker graphs to this graph. Submit
			// handles acquisition, backbuffer barriers, the frame join, and presentation.
			if (graph.Submit(device, submission, nullptr, ready) != Result::Success || submission.completions.empty()) {
				return false;
			}
			completion = submission.completions.back();
			return true;
		}
	};
}

int main(int argc, char** argv) {
	return Run<GraphRenderer>(argc, argv);
}
