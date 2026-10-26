#include "Device.h"
#include "RenderGraph/RenderGraph.h"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace {
	using namespace RealRHI;
	uint32_t s_ExpectedErrors = 0;
	uint32_t s_UnexpectedErrors = 0;
	bool s_ExpectError = false;

	void Check(bool condition, const char* message) {
		if (!condition) {
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	void CaptureDebugMessage(const DebugMessage& message) {
		if (message.severity == DebugSeverity::Error) {
			if (s_ExpectError && message.type == DebugMessageType::General &&
				(std::string_view(message.message).starts_with("DrawIndirect:") ||
					std::string_view(message.message).starts_with("DrawIndexedIndirect:") ||
					std::string_view(message.message).starts_with("DispatchComputeIndirect:"))) {
				++s_ExpectedErrors;
				return;
			}
			++s_UnexpectedErrors;
		}
		DefaultDebugCallback(message);
	}

	// Prefixes and padding exercise byte offsets and strides rather than assuming packed arguments.
	struct DrawRecord {
		DrawIndirectArguments draw;
		DrawIndexedIndirectArguments indexed;
		std::array<uint32_t, 3> padding{};
	};
	static_assert(sizeof(DrawRecord) == 48);
	constexpr uint64_t s_ArgumentSize = 16 + 4 * sizeof(DrawRecord);

	void RunCulling(Device& device, bool indexed, float viewHalfWidth, QueueType computeQueue, bool multiDraw) {
		Ref<Shader> cullShader;
		Ref<Shader> drawShader;
		Ref<Pipeline> cullPipeline;
		Ref<Pipeline> drawPipeline;
		Check(device.CreateShader({.moduleName = "cull"}, cullShader) == Result::Success, "Compile culling shader");
		Check(device.CreateShader({.moduleName = "culled_draw"}, drawShader) == Result::Success, "Compile draw shader");
		Check(device.CreateComputePipeline({.shader = cullShader.Raw()}, cullPipeline) == Result::Success,
			"Create culling pipeline");
		Check(device.CreateGraphicsPipeline(
				  {
					  .shader = drawShader.Raw(),
					  .rasterState = {.cullMode = CullMode::None},
					  .depthState = {.depthTestEnable = false, .depthWriteEnable = false},
					  .renderTargetFormats = {.colorFormats = {TextureFormat::RGBA8_UNorm}},
				  },
				  drawPipeline) == Result::Success,
			"Create indirect graphics pipeline");

		Ref<Buffer> arguments;
		Ref<Buffer> dispatch;
		Ref<Buffer> indices;
		Ref<Buffer> readback;
		const std::array<uint32_t, 4> dispatchData{0, 1, 1, 1};
		const std::array<uint32_t, 12> indexData{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
		Check(device.CreateBuffer({.size = s_ArgumentSize,
									  .usage = BufferUsage::Storage | BufferUsage::Indirect | BufferUsage::TransferSrc},
				  arguments) == Result::Success,
			"Create GPU-generated draw arguments");
		Check(device.CreateBuffer({.size = sizeof(dispatchData),
									  .usage = BufferUsage::Indirect,
									  .memoryUsage = MemoryUsage::CPUToGPU,
									  .initialData = dispatchData.data()},
				  dispatch) == Result::Success,
			"Create indirect dispatch arguments");
		Check(device.CreateBuffer({.size = sizeof(indexData),
									  .usage = BufferUsage::Index,
									  .memoryUsage = MemoryUsage::CPUToGPU,
									  .initialData = indexData.data()},
				  indices) == Result::Success,
			"Create triangle indices");
		Check(device.CreateBuffer({.size = 64 * 64 * 4 + s_ArgumentSize,
									  .usage = BufferUsage::TransferDst,
									  .memoryUsage = MemoryUsage::GPUToCPU},
				  readback) == Result::Success,
			"Create result readback");

		Ref<Texture> color;
		Check(device.CreateTexture({.width = 64,
									   .height = 64,
									   .format = TextureFormat::RGBA8_UNorm,
									   .usage = TextureUsage::RenderTarget | TextureUsage::TransferSrc},
				  color) == Result::Success,
			"Create offscreen target");

		RenderGraph graph;
		const auto argumentResource = graph.ImportBuffer(arguments.Raw());
		const auto dispatchResource = graph.ImportBuffer(dispatch.Raw());
		const auto indexResource = graph.ImportBuffer(indices.Raw());
		const auto outputResource = graph.ImportBuffer(readback.Raw());
		const auto colorResource = graph.ImportTexture(color.Raw(), TextureLayout::Undefined);
		const ResourceState indirect{PipelineStage::DrawIndirect, ResourceAccess::IndirectCommandRead};
		graph.AddPass({.name = "Cull",
						  .resources =
							  {{argumentResource,
								   {PipelineStage::ComputeShader, ResourceAccess::ShaderWrite},
								   TextureLayout::Undefined,
								   RenderGraphAccess::Write},
								  {dispatchResource, indirect}},
						  .queue = computeQueue},
			[&](CommandList& list) {
				list.BindPipeline(cullPipeline.Raw());
				list.UseResource(arguments.Raw());
				struct CullParameters {
					uint32_t output;
					float viewHalfWidth;
				} parameters{arguments->GetDescriptorIndex(), viewHalfWidth};
				list.SetPushConstants(&parameters, sizeof(parameters));
#ifdef REALRHI_INDIRECT_TESTS
				s_ExpectError = true;
				list.DispatchComputeIndirect(nullptr);
				list.DispatchComputeIndirect(dispatch.Raw(), 2);
				list.DispatchComputeIndirect(dispatch.Raw(), 8);
				list.DispatchComputeIndirect(readback.Raw());
				s_ExpectError = false;
#endif
				list.DispatchComputeIndirect(dispatch.Raw(), 4);
			});
		graph.AddPass({.name = "Draw visible objects",
						  .resources =
							  {{argumentResource, indirect},
								  {indexResource, {PipelineStage::VertexInput, ResourceAccess::IndexRead}},
								  {colorResource,
									  {PipelineStage::ColorAttachmentOutput, ResourceAccess::ColorAttachmentWrite},
									  TextureLayout::ColorAttachment,
									  RenderGraphAccess::Write}},
						  .queue = QueueType::Graphics},
			[&](CommandList& list) {
				list.BeginRendering(
					{.colorAttachments = {{.target = color->GetTextureView()}},
						.renderArea = {.width = 64, .height = 64}});
				list.SetViewport({.width = 64, .height = 64});
				list.SetScissor({.width = 64, .height = 64});
				list.BindPipeline(drawPipeline.Raw());
#ifdef REALRHI_INDIRECT_TESTS
				s_ExpectError = true;
				list.DispatchComputeIndirect(dispatch.Raw());
				list.DrawIndexedIndirect(arguments.Raw(), 32);
				list.DrawIndirect(nullptr);
				list.DrawIndirect(arguments.Raw(), 2);
				list.DrawIndirect(arguments.Raw(), s_ArgumentSize - 4);
				list.DrawIndirect(arguments.Raw(), 16, 2, 12);
				list.DrawIndirect(arguments.Raw(), 16, 2, 17);
				list.DrawIndirect(arguments.Raw(), 16, UINT32_MAX, UINT32_MAX - 3);
				s_ExpectError = false;
#endif
				list.BindIndexBuffer(indices.Raw());
#ifdef REALRHI_INDIRECT_TESTS
				s_ExpectError = true;
				list.DrawIndexedIndirect(nullptr);
				list.DrawIndexedIndirect(arguments.Raw(), 2);
				list.DrawIndexedIndirect(arguments.Raw(), s_ArgumentSize - 4);
				list.DrawIndexedIndirect(arguments.Raw(), 32, 2, 16);
				list.DrawIndexedIndirect(arguments.Raw(), 32, 2, 21);
				s_ExpectError = false;
#endif
				list.DrawIndirect(arguments.Raw(), s_ArgumentSize, 0);
				list.DrawIndexedIndirect(arguments.Raw(), s_ArgumentSize, 0);

				if (multiDraw && device.GetCapabilities().features.multiDrawIndirect &&
					device.GetCapabilities().limits.maxDrawIndirectCount >= 4) {
					if (indexed) {
						list.DrawIndexedIndirect(arguments.Raw(), 32, 4, sizeof(DrawRecord));
					}
					else {
						list.DrawIndirect(arguments.Raw(), 16, 4, sizeof(DrawRecord));
					}
				}
				else {
					for (uint32_t i = 0; i < 4; ++i) {
						if (indexed) {
							list.DrawIndexedIndirect(arguments.Raw(), 32 + i * sizeof(DrawRecord));
						}
						else {
							list.DrawIndirect(arguments.Raw(), 16 + i * sizeof(DrawRecord));
						}
					}
				}
				list.EndRendering();
			});
		graph.AddPass({.name = "Readback",
						  .resources =
							  {{argumentResource, {PipelineStage::Transfer, ResourceAccess::TransferRead}},
								  {colorResource,
									  {PipelineStage::Transfer, ResourceAccess::TransferRead},
									  TextureLayout::TransferSrc},
								  {outputResource,
									  {PipelineStage::Transfer, ResourceAccess::TransferWrite},
									  TextureLayout::Undefined,
									  RenderGraphAccess::Write}},
						  .queue = QueueType::Transfer},
			[&](CommandList& list) {
				list.CopyTextureToBuffer(color.Raw(), readback.Raw(), {.width = 64, .height = 64});
				list.CopyBuffer(
					arguments.Raw(), readback.Raw(), {.destinationOffset = 64 * 64 * 4, .size = s_ArgumentSize});
			});
		RenderGraphSubmission submission;
		Check(graph.Submit(device, submission) == Result::Success, "Schedule culling, graphics, and readback queues");
		graph.Reset();
		arguments.Reset();
		dispatch.Reset();
		indices.Reset();
		submission.Wait(device);

		std::array<uint8_t, 64 * 64 * 4> pixels{};
		std::array<DrawRecord, 4> records{};
		Check(readback->ReadData(pixels.data(), pixels.size()) == Result::Success, "Read rendered pixels");
		Check(readback->ReadData(records.data(), sizeof(records), pixels.size() + 16) == Result::Success,
			"Read GPU culling decisions");
		for (uint32_t i = 0; i < 4; ++i) {
			uint32_t visible = 0;
			if (i < 2 && viewHalfWidth > 0) {
				visible = 1;
			}
			Check(records[i].draw.vertexCount == 3 && records[i].draw.instanceCount == visible &&
					records[i].draw.firstVertex == i * 3 && records[i].draw.firstInstance == 0 &&
					records[i].indexed.indexCount == 3 && records[i].indexed.instanceCount == visible &&
					records[i].indexed.firstIndex == i * 3 && records[i].indexed.vertexOffset == 0 &&
					records[i].indexed.firstInstance == 0,
				"GPU emits complete arguments and culls invisible objects");
		}
		uint8_t green = 0;
		if (viewHalfWidth > 0) {
			green = 255;
		}
		for (uint32_t x : {16u, 48u}) {
			const auto offset = (32 * 64 + x) * 4;
			Check(pixels[offset] == 0 && pixels[offset + 1] == green && pixels[offset + 2] == 0,
				"Only visible triangles produce green pixels");
		}
		Check(pixels[0] == 0 && pixels[1] == 0 && pixels[2] == 0, "Background retains clear color");
	}
}

int main() {
	std::filesystem::path shaderDirectory = std::filesystem::current_path() / "shaders";
#ifdef REALRHI_INDIRECT_TESTS
	shaderDirectory = REALRHI_CULLING_SHADER_DIRECTORY;
#endif
	std::unique_ptr<RealRHI::Device> device;
	Check(RealRHI::Device::Create({.applicationName = "CullingExample",
											  .shaderDirectory = shaderDirectory,
											  .enableDebug = true,
											  .enableProfiling = false,
											  .enableValidationLayers = true,
											  .debugCallback = CaptureDebugMessage},
			  device) == RealRHI::Result::Success,
		"Create culling device");
	for (auto queue : {RealRHI::QueueType::Graphics, RealRHI::QueueType::Compute}) {
		for (bool indexed : {false, true}) {
			for (bool multiDraw : {false, true}) {
				RunCulling(*device, indexed, 1.f, queue, multiDraw);
				RunCulling(*device, indexed, 0.f, queue, multiDraw);
			}
		}
	}
	device->WaitIdle();
	device.reset();
#ifdef REALRHI_INDIRECT_TESTS
	Check(s_ExpectedErrors == 16 * 17, "Invalid indirect operations were all rejected");
#endif
	Check(s_UnexpectedErrors == 0, "Culling completes without unexpected errors");
	std::cout
		<< "CullingExample passed: compute-generated indexed and non-indexed draws, two visible / four candidates.\n";
}
