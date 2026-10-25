#include "SceneRenderer.h"

#include <array>
#include <cmath>
#include <string>
#include <utility>

namespace RealRHI::Examples {
	namespace {
		struct Vector3 {
			float x;
			float y;
			float z;
		};

		struct Vertex {
			Vector3 position;
			float uv[2];
			Vector3 normal;
		};

		struct Geometry {
			std::vector<Vertex> vertices;
			std::vector<uint32_t> indices;
		};

		struct SceneParameters {
			std::array<float, 4> translationAndScale;
			std::array<float, 4> tint;
			float aspectRatio;
			float rotationSine;
			float rotationCosine;
			float padding = 0;
			uint32_t textureIndex;
			uint32_t samplerIndex;
			float descriptorPadding[2]{};
		};

		Geometry BuildGeometry() {
			Geometry geometry;
			geometry.vertices = {
				{{-3, -0.65f, -3}, {0, 0}, {0, 1, 0}},
				{{3, -0.65f, -3}, {6, 0}, {0, 1, 0}},
				{{3, -0.65f, 3}, {6, 6}, {0, 1, 0}},
				{{-3, -0.65f, 3}, {0, 6}, {0, 1, 0}},
			};
			geometry.indices = {0, 1, 2, 2, 3, 0};

			constexpr Vector3 corners[] = {
				{-0.5f, -0.5f, -0.5f},
				{0.5f, -0.5f, -0.5f},
				{0.5f, 0.5f, -0.5f},
				{-0.5f, 0.5f, -0.5f},
				{-0.5f, -0.5f, 0.5f},
				{0.5f, -0.5f, 0.5f},
				{0.5f, 0.5f, 0.5f},
				{-0.5f, 0.5f, 0.5f},
			};
			constexpr uint32_t faces[][4] = {
				{4, 5, 6, 7},
				{1, 0, 3, 2},
				{0, 4, 7, 3},
				{5, 1, 2, 6},
				{7, 6, 2, 3},
				{0, 1, 5, 4},
			};
			constexpr Vector3 normals[] = {{0, 0, 1}, {0, 0, -1}, {-1, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
			constexpr float uv[][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};

			for (uint32_t face = 0; face < 6; ++face) {
				for (uint32_t corner = 0; corner < 4; ++corner) {
					geometry.vertices.push_back(
						{corners[faces[face][corner]], {uv[corner][0], uv[corner][1]}, normals[face]});
				}

				for (uint32_t index : {0u, 1u, 2u, 2u, 3u, 0u}) {
					geometry.indices.push_back(face * 4 + index);
				}
			}

			geometry.vertices.insert(geometry.vertices.end(),
				{
					{{-0.5f, -0.5f, 0}, {0, 0}, {0, 0, 1}},
					{{0.5f, -0.5f, 0}, {1, 0}, {0, 0, 1}},
					{{0.5f, 0.5f, 0}, {1, 1}, {0, 0, 1}},
					{{-0.5f, 0.5f, 0}, {0, 1}, {0, 0, 1}},
				});
			geometry.indices.insert(geometry.indices.end(), {0, 1, 2, 2, 3, 0});

			return geometry;
		}
	}

	Result SceneRenderer::Init(Device& device,
		TextureFormat outputFormat,
		uint32_t frameCount,
		UploadSubmission& outUpload) {
		if (frameCount == 0) {
			return Result::InvalidParameter;
		}

		m_Device = &device;
		m_Frames.resize(frameCount);

		const Result pipelineResult = CreatePipelines(outputFormat);
		if (pipelineResult != Result::Success) {
			return pipelineResult;
		}

		return UploadAssets(outUpload);
	}

	Result SceneRenderer::CreatePipelines(TextureFormat outputFormat) {
		Ref<Shader> sceneShader;
		Result result = m_Device->CreateShader({.moduleName = "scene",

												   .debugName = "Scene geometry shader"},
			sceneShader);
		if (result != Result::Success) {
			return result;
		}

		PipelineDesc sceneDesc{
			.shader = sceneShader.Raw(),

			.rasterState = {.cullMode = CullMode::None},
			.renderTargetFormats =
				{
					.colorFormats = {TextureFormat::RGBA8_UNorm},
					.depthFormat = TextureFormat::D32_Float,
				},
			.debugName = "Scene opaque pipeline",
		};
		result = m_Device->CreateGraphicsPipeline(sceneDesc, m_OpaquePipeline);
		if (result != Result::Success) {
			return result;
		}

		sceneDesc.depthState.depthWriteEnable = false;
		sceneDesc.blendState.enable = true;
		sceneDesc.debugName = "Scene transparent pipeline";
		result = m_Device->CreateGraphicsPipeline(sceneDesc, m_TransparentPipeline);
		if (result != Result::Success) {
			return result;
		}

		Ref<Shader> postprocessShader;
		result = m_Device->CreateShader(
			{.moduleName = "postprocess", .debugName = "Scene postprocess shader"}, postprocessShader);
		if (result != Result::Success) {
			return result;
		}

		return m_Device->CreateGraphicsPipeline(
			{
				.shader = postprocessShader.Raw(),
				.rasterState = {.cullMode = CullMode::None},
				.depthState = {.depthTestEnable = false, .depthWriteEnable = false},
				.renderTargetFormats = {.colorFormats = {outputFormat}},
				.debugName = "Scene postprocess pipeline",
			},
			m_PostprocessPipeline);
	}

	Result SceneRenderer::CreateGeometry(UploadContext& uploads) {
		const Geometry geometry = BuildGeometry();
		const uint64_t vertexBytes = geometry.vertices.size() * sizeof(Vertex);
		const uint64_t indexBytes = geometry.indices.size() * sizeof(uint32_t);

		Result result = m_Device->CreateBuffer(
			{
				.size = vertexBytes,
				.usage = BufferUsage::Vertex | BufferUsage::TransferDst,
				.debugName = "Scene vertices",
			},
			m_VertexBuffer);
		if (result != Result::Success) {
			return result;
		}

		result = m_Device->CreateBuffer(
			{.size = indexBytes, .usage = BufferUsage::Index | BufferUsage::TransferDst, .debugName = "Scene indices"},
			m_IndexBuffer);
		if (result != Result::Success) {
			return result;
		}

		result = uploads.UploadBuffer(
			{
				.destination = m_VertexBuffer.Raw(),
				.finalState = {PipelineStage::VertexInput, ResourceAccess::VertexAttributeRead},
			},
			geometry.vertices.data(),
			vertexBytes);
		if (result != Result::Success) {
			return result;
		}

		return uploads.UploadBuffer(
			{
				.destination = m_IndexBuffer.Raw(),
				.finalState = {PipelineStage::VertexInput, ResourceAccess::IndexRead},
			},
			geometry.indices.data(),
			indexBytes);
	}

	Result SceneRenderer::CreatePattern(UploadContext& uploads) {
		Result result = m_Device->CreateTexture(
			{
				.width = 32,
				.height = 32,
				.mipLevels = 6,
				.format = TextureFormat::RGBA8_UNorm,
				.usage = TextureUsage::ShaderResource | TextureUsage::TransferSrc | TextureUsage::TransferDst,
				.debugName = "Scene checkerboard",
			},
			m_Pattern);
		if (result != Result::Success) {
			return result;
		}

		std::array<uint8_t, 32 * 32 * 4> pixels{};
		for (uint32_t y = 0; y < 32; ++y) {
			for (uint32_t x = 0; x < 32; ++x) {
				uint8_t shade = 220;
				if (((x / 4) + (y / 4)) % 2 == 0) {
					shade = 100;
				}

				const size_t offset = (y * 32 + x) * 4;
				pixels[offset] = shade;
				pixels[offset + 1] = shade;
				pixels[offset + 2] = shade;
				pixels[offset + 3] = 255;
			}
		}

		return uploads.UploadTexture(
			{
				.destination = m_Pattern.Raw(),
				.finalLayout = TextureLayout::TransferDst,
				.finalState = {PipelineStage::Transfer, ResourceAccess::TransferWrite},
			},
			pixels.data(),
			pixels.size());
	}

	Result SceneRenderer::UploadAssets(UploadSubmission& outUpload) {
		std::unique_ptr<UploadContext> uploads;
		Result result = m_Device->CreateUploadContext({}, uploads);
		if (result != Result::Success) {
			return result;
		}

		result = uploads->BeginBatch();
		if (result != Result::Success) {
			return result;
		}

		result = CreateGeometry(*uploads);
		if (result != Result::Success) {
			return result;
		}

		result = CreatePattern(*uploads);
		if (result != Result::Success) {
			return result;
		}

		result = m_Device->CreateSampler({.debugName = "Scene linear sampler"}, m_Sampler);
		if (result != Result::Success) {
			return result;
		}

		return uploads->Submit(outUpload);
	}

	Result SceneRenderer::ResizeTargets(uint32_t frameIndex, uint32_t width, uint32_t height) {
		if (frameIndex >= m_Frames.size() || width == 0 || height == 0) {
			return Result::InvalidParameter;
		}

		auto& current = m_Frames[frameIndex];
		if (current.width == width && current.height == height) {
			return Result::Success;
		}

		FrameTargets targets;
		const std::string colorName = "Scene color slot " + std::to_string(frameIndex);
		const std::string depthName = "Scene depth slot " + std::to_string(frameIndex);
		Result result = m_Device->CreateTexture(
			{
				.width = width,
				.height = height,
				.format = TextureFormat::RGBA8_UNorm,
				.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource,
				.debugName = colorName.c_str(),
			},
			targets.color);
		if (result != Result::Success) {
			return result;
		}

		result = m_Device->CreateTexture(
			{
				.width = width,
				.height = height,
				.format = TextureFormat::D32_Float,
				.usage = TextureUsage::DepthStencil,
				.debugName = depthName.c_str(),
			},
			targets.depth);
		if (result != Result::Success) {
			return result;
		}

		targets.width = width;
		targets.height = height;
		current = std::move(targets);

		return Result::Success;
	}

	void SceneRenderer::BeginScene(CommandList& commands, FrameTargets& targets) {
		TextureLayout oldColorLayout = TextureLayout::Undefined;
		TextureLayout oldDepthLayout = TextureLayout::Undefined;
		ResourceState beforeColor;
		ResourceState beforeDepth;
		if (targets.initialized) {
			oldColorLayout = TextureLayout::ShaderRead;
			oldDepthLayout = TextureLayout::DepthStencilAttachment;
			beforeColor = {PipelineStage::FragmentShader, ResourceAccess::ShaderRead};
			beforeDepth = {
				PipelineStage::EarlyFragmentTests | PipelineStage::LateFragmentTests,
				ResourceAccess::DepthStencilRead | ResourceAccess::DepthStencilWrite,
			};
		}

		BarrierDesc barriers;
		barriers.textures.push_back({
			.texture = targets.color.Raw(),
			.before = beforeColor,
			.after =
				{
					PipelineStage::ColorAttachmentOutput,
					ResourceAccess::ColorAttachmentRead | ResourceAccess::ColorAttachmentWrite,
				},
			.oldLayout = oldColorLayout,
			.newLayout = TextureLayout::ColorAttachment,
		});
		barriers.textures.push_back({
			.texture = targets.depth.Raw(),
			.before = beforeDepth,
			.after =
				{
					PipelineStage::EarlyFragmentTests | PipelineStage::LateFragmentTests,
					ResourceAccess::DepthStencilRead | ResourceAccess::DepthStencilWrite,
				},
			.oldLayout = oldDepthLayout,
			.newLayout = TextureLayout::DepthStencilAttachment,
		});
		commands.Barrier(barriers);

		commands.BeginRendering({
			.colorAttachments = {{.target = targets.color->GetTextureView(), .clearColor = {0.02f, 0.03f, 0.05f, 1}}},
			.hasDepth = true,
			.depthAttachment = {.target = targets.depth->GetTextureView()},
			.renderArea = {.width = targets.width, .height = targets.height},
		});
	}

	void SceneRenderer::DrawScene(CommandList& commands, const FrameTargets& targets, float rotation) {
		commands.BindPipeline(m_OpaquePipeline.Raw());
		commands.BindVertexBuffer(m_VertexBuffer.Raw());
		commands.BindIndexBuffer(m_IndexBuffer.Raw());
		commands.UseResource(m_Pattern.Raw());
		commands.UseResource(m_Sampler.Raw());

		SceneParameters parameters{
			.translationAndScale = {-0.4f, 0, 0.6f, 1},
			.tint = {0.9f, 0.25f, 0.12f, 1},
			.aspectRatio = static_cast<float>(targets.width) / targets.height,
			.rotationSine = std::sin(rotation + 0.35f),
			.rotationCosine = std::cos(rotation + 0.35f),
			.textureIndex = m_Pattern->GetSampledImageDescriptorIndex(),
			.samplerIndex = m_Sampler->GetDescriptorIndex(),
		};
		commands.SetPushConstants(&parameters, sizeof(parameters));
		commands.DrawIndexed(36, 1, 6, 4);

		parameters.translationAndScale = {0.4f, 0, -0.5f, 1.1f};
		parameters.tint = {0.12f, 0.4f, 0.9f, 1};
		parameters.rotationSine = std::sin(-rotation - 0.45f);
		parameters.rotationCosine = std::cos(-rotation - 0.45f);
		commands.SetPushConstants(&parameters, sizeof(parameters));
		commands.DrawIndexed(36, 1, 6, 4);

		parameters.translationAndScale = {0, 0, 0, 1};
		parameters.tint = {0.3f, 0.34f, 0.4f, 1};
		parameters.rotationSine = 0;
		parameters.rotationCosine = 1;
		commands.SetPushConstants(&parameters, sizeof(parameters));
		commands.DrawIndexed(6);

		commands.BindPipeline(m_TransparentPipeline.Raw());
		parameters.translationAndScale = {-0.9f, 0.1f, 1.7f, 1.1f};
		parameters.tint = {0.1f, 0.9f, 0.65f, 0.35f};
		commands.SetPushConstants(&parameters, sizeof(parameters));
		commands.DrawIndexed(6, 1, 42, 28);
		commands.EndRendering();
	}

	void SceneRenderer::DrawPostprocess(CommandList& commands, FrameTargets& targets, TextureView* output) {
		BarrierDesc barriers;
		barriers.textures.push_back({
			.texture = targets.color.Raw(),
			.before = {PipelineStage::ColorAttachmentOutput, ResourceAccess::ColorAttachmentWrite},
			.after = {PipelineStage::FragmentShader, ResourceAccess::ShaderRead},
			.oldLayout = TextureLayout::ColorAttachment,
			.newLayout = TextureLayout::ShaderRead,
		});
		commands.Barrier(barriers);

		commands.BeginRendering({
			.colorAttachments = {{.target = output}},
			.renderArea = {.width = targets.width, .height = targets.height},
		});
		commands.BindPipeline(m_PostprocessPipeline.Raw());
		commands.UseResource(targets.color.Raw());

		const uint32_t parameters[] = {
			targets.color->GetSampledImageDescriptorIndex(),
			m_Sampler->GetDescriptorIndex(),
		};
		commands.SetPushConstants(parameters, sizeof(parameters));
		commands.Draw(3);
		commands.EndRendering();
	}

	void SceneRenderer::Record(CommandList& commands,
		uint32_t frameIndex,
		TextureView* output,
		float rotation,
		const UploadSubmission& pendingUpload) {
		commands.BeginDebugLabel("Scene frame");
		commands.WriteTimestamp("Frame setup");
		auto& targets = m_Frames[frameIndex];
		if (pendingUpload.completion.IsValid()) {
			commands.Barrier(pendingUpload.acquireBarriers);
			commands.GenerateMipmaps(
				m_Pattern.Raw(), {.finalState = {PipelineStage::FragmentShader, ResourceAccess::ShaderRead}});
		}

		commands.SetViewport(
			{.width = static_cast<float>(targets.width), .height = static_cast<float>(targets.height)});
		commands.SetScissor({.width = targets.width, .height = targets.height});

		commands.WriteTimestamp("Scene");
		commands.BeginDebugLabel("Opaque and transparent geometry");
		BeginScene(commands, targets);
		DrawScene(commands, targets, rotation);
		commands.EndDebugLabel();
		commands.WriteTimestamp("Postprocess");
		commands.BeginDebugLabel("Fullscreen postprocess");
		DrawPostprocess(commands, targets, output);
		commands.EndDebugLabel();
		commands.WriteTimestamp("Frame end");
		commands.EndDebugLabel();
		targets.initialized = true;
	}
}
