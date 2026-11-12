#include "../MultithreadedSupport.h"

using namespace RealRHI;
using namespace RealRHI::Examples::Multithreaded;

namespace {
	struct RawRenderer {
		static constexpr const char* Title = "RealRHI Multithreaded - Raw Calls";
		static constexpr bool RawPresentation = true;

		static bool RecordTile(Device&,
			CommandList& commands,
			Texture* tile,
			bool initialized,
			Shader* shader,
			uint32_t width,
			uint32_t height,
			uint32_t worker,
			float time) {
			// This slot's last composite has completed. Track the real incoming layout
			// and order its previous fragment reads before this frame's attachment writes.
			commands.Barrier(
				{.textures = {{
					 .texture = tile,
					 .before = initialized ? SampledState : ResourceState{},
					 .after = ColorState,
					 .oldLayout = initialized ? TextureLayout::ShaderRead : TextureLayout::Undefined,
					 .newLayout = TextureLayout::ColorAttachment,
				 }}});
			DrawTile(commands, tile, shader, width, height, worker, time);
			commands.Barrier(
				{.textures = {{
					 .texture = tile,
					 .before = ColorState,
					 .after = SampledState,
					 .oldLayout = TextureLayout::ColorAttachment,
					 .newLayout = TextureLayout::ShaderRead,
				 }}});
			return true;
		}

		static bool Present(Device& device,
			Swapchain& swapchain,
			const FrameContext& frame,
			FrameResources& frames,
			const std::array<Texture*, WorkerCount>& tiles,
			Shader* shader,
			std::span<const SubmissionToken> ready,
			SubmissionToken& completion) {
			CommandList* commands = nullptr;
			if (frames.BeginSlot(frame.frameIndex, commands) != Result::Success ||
				commands->Begin() != Result::Success) {
				return false;
			}
			DrawComposite(*commands, frame, tiles, shader);
			if (commands->End() != Result::Success ||
				// Device::Submit handles acquire/present backbuffer transitions. The
				// worker barriers above handle the offscreen attachment dependencies.
				device.Submit(commands, &swapchain, frame, &completion, ready) != Result::Success ||
				frames.TrackSubmission(frame.frameIndex, completion) != Result::Success) {
				return false;
			}
			return swapchain.Present(frame) == Result::Success;
		}
	};
}

int main(int argc, char** argv) {
	return Run<RawRenderer>(argc, argv);
}
