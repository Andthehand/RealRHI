#include "ImGui/ImGuiRenderer.h"

#include <array>
#include <cstdlib>
#include <iostream>

namespace {
	using namespace RealRHI;
	void Expect(bool condition, const char* message) {
		if (!condition) {
			std::cerr << message << '\n';
			std::exit(1);
		}
	}
	uint32_t errors = 0;
	uint32_t callbacks = 0;
	void Capture(const DebugMessage& message) {
		DefaultDebugCallback(message);
		if (message.severity == DebugSeverity::Error) {
			++errors;
		}
	}
	void Callback(const ImDrawList*, const ImDrawCmd*) {
		++callbacks;
	}
}

int main() {
	using namespace RealRHI;
	std::unique_ptr<Device> device;
	Expect(Device::Create({.shaderDirectory = REALRHI_IMGUI_SHADER_DIRECTORY,
							  .enableDebug = true,
							  .enableValidationLayers = true,
							  .debugCallback = Capture},
			   device) == Result::Success,
		"Create ImGui test device");
	ImGui::CreateContext();
	ImGui::GetIO().IniFilename = nullptr;
	ImGuiRenderer renderer;
	Expect(renderer.Initialize(*device) == Result::Success, "Initialize renderer and upload atlas");
	Expect(renderer.Initialize(*device) == Result::InvalidParameter, "Reject double initialization");

	Ref<Texture> image, target;
	const uint32_t red = 0xff0000ff;
	Expect(device->CreateTexture({.width = 1,
									 .height = 1,
									 .format = TextureFormat::RGBA8_UNorm,
									 .usage = TextureUsage::ShaderResource | TextureUsage::TransferDst},
			   image) == Result::Success &&
			image->UploadData(&red, sizeof(red)) == Result::Success,
		"Upload custom red image");
	Expect(device->CreateTexture({.width = 32,
									 .height = 32,
									 .format = TextureFormat::RGBA8_UNorm,
									 .usage = TextureUsage::RenderTarget | TextureUsage::TransferSrc},
			   target) == Result::Success,
		"Create offscreen UI target");
	Ref<Buffer> readback;
	Expect(device->CreateBuffer(
			   {.size = 32 * 32 * 4, .usage = BufferUsage::TransferDst, .memoryUsage = MemoryUsage::GPUToCPU},
			   readback) == Result::Success,
		"Create readback");
	auto& io = ImGui::GetIO();
	io.DisplaySize = {16, 16};
	io.DisplayFramebufferScale = {2, 2};
	io.DeltaTime = 1.f / 60.f;
	ImGui::NewFrame();
	auto* draw = ImGui::GetBackgroundDrawList();
	draw->AddCallback(Callback, nullptr);
	draw->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
	draw->PushClipRect({4, 4}, {12, 12}, true);
	draw->AddImage(ImGuiRenderer::GetTextureID(image.Raw()), {0, 0}, {16, 16});
	draw->PopClipRect();
	// Uses the atlas's white texel, exercising the default font texture as well.
	draw->AddRectFilled({0, 0}, {2, 2}, IM_COL32(0, 255, 0, 255));
	ImGui::Render();
	Ref<CommandAllocator> allocator;
	Ref<CommandList> commands;
	Expect(device->CreateCommandAllocator(QueueType::Graphics, allocator) == Result::Success &&
			device->CreateCommandList(allocator.Raw(), commands) == Result::Success &&
			commands->Begin() == Result::Success,
		"Begin UI recording");
	commands->Barrier(
		{.textures = {
			 {.texture = target.Raw(),
				 .after = {PipelineStage::ColorAttachmentOutput, ResourceAccess::ColorAttachmentWrite},
				 .newLayout = TextureLayout::ColorAttachment}
		 }});
	commands->BeginRendering(
		{.colorAttachments = {{.target = target->GetTextureView(), .clearColor = {0, 0, 0, 0}}},
			.renderArea = {.width = 32, .height = 32}});
	Expect(renderer.RenderDrawData(*commands, *ImGui::GetDrawData()) == Result::Success, "Record ImGui draws");
	commands->EndRendering();
	commands->Barrier(
		{.textures = {
			 {.texture = target.Raw(),
				 .before = {PipelineStage::ColorAttachmentOutput, ResourceAccess::ColorAttachmentWrite},
				 .after = {PipelineStage::Transfer, ResourceAccess::TransferRead},
				 .oldLayout = TextureLayout::ColorAttachment,
				 .newLayout = TextureLayout::TransferSrc}
		 }});
	commands->CopyTextureToBuffer(target.Raw(), readback.Raw(), {.width = 32, .height = 32});
	Expect(commands->End() == Result::Success && device->ImmediateSubmit(commands.Raw()) == Result::Success,
		"Submit UI recording");
	std::array<uint32_t, 32 * 32> pixels{};
	Expect(readback->ReadData(pixels.data(), sizeof(pixels)) == Result::Success, "Read UI pixels");
	Expect(pixels[16 * 32 + 16] == red, "Custom image renders red inside scaled clip");
	Expect(pixels[6 * 32 + 6] == 0, "Clip preserves pixels outside its scaled rectangle");
	Expect(pixels[1 * 32 + 1] == 0xff00ff00, "Font atlas white texel renders green");
	Expect(callbacks == 1, "Custom callback executes once");
	Expect(errors == 0, "No RHI or Vulkan validation errors");
	device->WaitIdle();
	Expect(commands->Reset() == Result::Success, "Reset releases UI recording resources");
	renderer.Shutdown();
	Expect(io.BackendRendererUserData == nullptr && io.Fonts->TexID == 0, "Shutdown clears backend state");
	ImGui::DestroyContext();
	std::cout << "ImGui tests passed\n";
}
