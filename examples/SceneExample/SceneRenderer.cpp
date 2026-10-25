#include "SceneRenderer.h"

#include "RenderGraph/RenderGraph.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
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
			uint32_t shadowTextureIndex;
			uint32_t shadowSamplerIndex;
			float shadowTexelSize;
			uint32_t shadowsEnabled;
			float shadowPadding[2]{};
		};

		static_assert(sizeof(SceneParameters) == 80);

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

		const Result depthResult = SelectDepthFormat();
		if (depthResult != Result::Success) {
			return depthResult;
		}

		const Result pipelineResult = CreatePipelines(outputFormat);
		if (pipelineResult != Result::Success) {
			return pipelineResult;
		}

		return UploadAssets(outUpload);
	}

	Result SceneRenderer::SelectDepthFormat() {
		m_ShadowResolution = std::min(1024u, m_Device->GetCapabilities().limits.maxTextureDimension2D);

		for (auto format : {TextureFormat::D32_Float, TextureFormat::D16_UNorm, TextureFormat::D24_UNorm_S8_UInt}) {
			const TextureDesc desc{
				.width = m_ShadowResolution,
				.height = m_ShadowResolution,
				.format = format,
				.usage = TextureUsage::DepthStencil | TextureUsage::ShaderResource
			};

			if (m_Device->GetTextureFormatSupport(format).depthComparison && m_Device->SupportsTexture(desc)) {
				m_DepthFormat = format;
				return Result::Success;
			}
		}

		std::cerr << "Scene: No sampled depth format supports shadow comparisons.\n";
		return Result::Failed;
	}

	Result SceneRenderer::CreatePipelines(TextureFormat outputFormat) {
		Ref<Shader> shadowShader;
		Result result =
			m_Device->CreateShader({.moduleName = "shadow", .debugName = "Scene shadow shader"}, shadowShader);
		if (result != Result::Success) {
			return result;
		}

		// Only position is consumed, but the shadow pass shares the full scene vertex stream.
		result = m_Device->CreateGraphicsPipeline(
			{.shader = shadowShader.Raw(),
				.vertexLayout =
					{.bindings = {{.stride = sizeof(Vertex)}}, .attributes = {{.format = VertexFormat::Float3}}},
				.rasterState =
					{.cullMode = CullMode::None,
						.depthBias = {.enable = true, .constantFactor = 1.25f, .slopeFactor = 1.75f}},
				.renderTargetFormats = {.depthFormat = m_DepthFormat},
				.debugName = "Scene shadow depth pipeline"},
			m_ShadowPipeline);
		if (result != Result::Success) {
			return result;
		}

		Ref<Shader> sceneShader;
		result = m_Device->CreateShader({.moduleName = "scene", .debugName = "Scene geometry shader"}, sceneShader);
		if (result != Result::Success) {
			return result;
		}

		PipelineDesc sceneDesc{
			.shader = sceneShader.Raw(),

			.rasterState = {.cullMode = CullMode::None},
			.renderTargetFormats =
				{
					.colorFormats = {TextureFormat::RGBA8_UNorm},
					.depthFormat = m_DepthFormat,
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

		result = m_Device->CreateSampler({.minFilter = Filter::Nearest,
											 .magFilter = Filter::Nearest,
											 .mipFilter = Filter::Nearest,
											 .addressU = SamplerAddressMode::ClampToBorder,
											 .addressV = SamplerAddressMode::ClampToBorder,
											 .addressW = SamplerAddressMode::ClampToBorder,
											 .compareEnable = true,
											 .compareOp = CompareOp::LessEqual,
											 .borderColor = SamplerBorderColor::OpaqueWhite,
											 .debugName = "Scene shadow comparison sampler"},
			m_ShadowSampler);
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
				.format = m_DepthFormat,
				.usage = TextureUsage::DepthStencil,
				.debugName = depthName.c_str(),
			},
			targets.depth);
		if (result != Result::Success) {
			return result;
		}

		const std::string shadowName = "Scene shadow map slot " + std::to_string(frameIndex);
		result = m_Device->CreateTexture({.width = m_ShadowResolution,
											 .height = m_ShadowResolution,
											 .format = m_DepthFormat,
											 .usage = TextureUsage::DepthStencil | TextureUsage::ShaderResource,
											 .debugName = shadowName.c_str()},
			targets.shadow);
		if (result != Result::Success) {
			return result;
		}

		targets.width = width;
		targets.height = height;
		current = std::move(targets);

		return Result::Success;
	}

	void SceneRenderer::BeginScene(CommandList& commands, FrameTargets& targets) {
		commands.BeginRendering({
			.colorAttachments = {{.target = targets.color->GetTextureView(), .clearColor = {0.02f, 0.03f, 0.05f, 1}}},
			.hasDepth = true,
			.depthAttachment = {.target = targets.depth->GetTextureView()},
			.renderArea = {.width = targets.width, .height = targets.height},
		});
	}

	void SceneRenderer::DrawGeometry(CommandList& commands,
		const FrameTargets& targets,
		float rotation,
		GeometryPass pass,
		bool shadowsEnabled) {
		commands.BindVertexBuffer(m_VertexBuffer.Raw());
		commands.BindIndexBuffer(m_IndexBuffer.Raw());

		SceneParameters parameters{
			.translationAndScale = {-0.4f, 0, 0.6f, 1},
			.tint = {0.9f, 0.25f, 0.12f, 1},
			.aspectRatio = static_cast<float>(targets.width) / targets.height,
			.rotationSine = std::sin(rotation + 0.35f),
			.rotationCosine = std::cos(rotation + 0.35f),
			.textureIndex = m_Pattern->GetSampledImageDescriptorIndex(),
			.samplerIndex = m_Sampler->GetDescriptorIndex(),
			.shadowTextureIndex = targets.shadow->GetSampledImageDescriptorIndex(),
			.shadowSamplerIndex = m_ShadowSampler->GetDescriptorIndex(),
			.shadowTexelSize = 1.f / m_ShadowResolution,
			.shadowsEnabled = static_cast<uint32_t>(shadowsEnabled),
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

		if (pass == GeometryPass::Lighting) {
			commands.BindPipeline(m_TransparentPipeline.Raw());
			parameters.translationAndScale = {-0.9f, 0.1f, 1.7f, 1.1f};
			parameters.tint = {0.1f, 0.9f, 0.65f, 0.35f};
			commands.SetPushConstants(&parameters, sizeof(parameters));
			commands.DrawIndexed(6, 1, 42, 28);
		}
	}

	void SceneRenderer::DrawShadowMap(CommandList& commands, FrameTargets& targets, float rotation) {
		commands.SetViewport(
			{.width = static_cast<float>(m_ShadowResolution), .height = static_cast<float>(m_ShadowResolution)});
		commands.SetScissor({.width = m_ShadowResolution, .height = m_ShadowResolution});
		commands.BeginRendering(
			{.hasDepth = true,
				.depthAttachment = {.target = targets.shadow->GetTextureView()},
				.renderArea = {.width = m_ShadowResolution, .height = m_ShadowResolution}});
		commands.BindPipeline(m_ShadowPipeline.Raw());
		DrawGeometry(commands, targets, rotation, GeometryPass::Shadow, false);
		commands.EndRendering();
	}

	void SceneRenderer::DrawScene(CommandList& commands, FrameTargets& targets, float rotation, bool shadowsEnabled) {
		commands.BindPipeline(m_OpaquePipeline.Raw());
		commands.UseResource(m_Pattern.Raw());
		commands.UseResource(m_Sampler.Raw());
		commands.UseResource(targets.shadow.Raw());
		commands.UseResource(m_ShadowSampler.Raw());

		DrawGeometry(commands, targets, rotation, GeometryPass::Lighting, shadowsEnabled);
		commands.EndRendering();
	}

	void SceneRenderer::DrawPostprocess(CommandList& commands, FrameTargets& targets, TextureView* output) {
		commands.BeginRendering({
			.colorAttachments = {{.target = output}},
			.renderArea = {.width = targets.width, .height = targets.height},
		});
		commands.BindPipeline(m_PostprocessPipeline.Raw());
		commands.UseResource(targets.color.Raw());
		commands.UseResource(m_Sampler.Raw());

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
		const UploadSubmission& pendingUpload,
		bool shadowsEnabled) {
		commands.BeginDebugLabel("Scene frame");
		commands.WriteTimestamp("Frame setup");
		auto& targets = m_Frames[frameIndex];
		if (pendingUpload.completion.IsValid()) {
			commands.Barrier(pendingUpload.acquireBarriers);
			commands.GenerateMipmaps(
				m_Pattern.Raw(), {.finalState = {PipelineStage::FragmentShader, ResourceAccess::ShaderRead}});
		}

		const ResourceState sampled{PipelineStage::FragmentShader, ResourceAccess::ShaderRead};
		const ResourceState colorAttachment{
			PipelineStage::ColorAttachmentOutput,
			ResourceAccess::ColorAttachmentRead | ResourceAccess::ColorAttachmentWrite
		};
		const ResourceState depthAttachment{
			PipelineStage::EarlyFragmentTests | PipelineStage::LateFragmentTests,
			ResourceAccess::DepthStencilRead | ResourceAccess::DepthStencilWrite
		};
		TextureLayout colorLayout = TextureLayout::Undefined;
		TextureLayout depthLayout = TextureLayout::Undefined;
		TextureLayout shadowLayout = TextureLayout::Undefined;
		ResourceState beforeColor;
		ResourceState beforeDepth;
		ResourceState beforeShadow;
		if (targets.initialized) {
			colorLayout = TextureLayout::ShaderRead;
			depthLayout = TextureLayout::DepthStencilAttachment;
			beforeColor = sampled;
			beforeDepth = depthAttachment;
		}
		if (targets.shadowInitialized) {
			shadowLayout = TextureLayout::ShaderRead;
			beforeShadow = sampled;
		}

		RenderGraph graph;
		const auto color = graph.ImportTexture(targets.color.Raw(), colorLayout, beforeColor);
		const auto depth = graph.ImportTexture(targets.depth.Raw(), depthLayout, beforeDepth);
		const auto shadow = graph.ImportTexture(targets.shadow.Raw(), shadowLayout, beforeShadow);
		const auto pattern = graph.ImportTexture(m_Pattern.Raw(), TextureLayout::ShaderRead, sampled, {0, 6, 0, 1});
		const ResourceState vertexState{PipelineStage::VertexInput, ResourceAccess::VertexAttributeRead};
		const ResourceState indexState{PipelineStage::VertexInput, ResourceAccess::IndexRead};
		const auto vertices = graph.ImportBuffer(m_VertexBuffer.Raw(), vertexState);
		const auto indices = graph.ImportBuffer(m_IndexBuffer.Raw(), indexState);

		// Initialize the sampled map even when shadows are disabled on the first frame.
		if (shadowsEnabled || !targets.shadowInitialized) {
			graph.AddPass(
				{.name = "Shadows",
					.resources =
						{
							{shadow, depthAttachment, TextureLayout::DepthStencilAttachment, RenderGraphAccess::Write},
							{vertices, vertexState},
							{indices, indexState},
						}},
				[&](CommandList& list) {
					list.WriteTimestamp("Shadows");
					DrawShadowMap(list, targets, rotation);
				});
		}

		graph.AddPass(
			{.name = "Scene",
				.resources =
					{
						{color, colorAttachment, TextureLayout::ColorAttachment, RenderGraphAccess::Write},
						{depth, depthAttachment, TextureLayout::DepthStencilAttachment, RenderGraphAccess::Write},
						{shadow, sampled, TextureLayout::ShaderRead},
						{pattern, sampled, TextureLayout::ShaderRead},
						{vertices, vertexState},
						{indices, indexState},
					}},
			[&](CommandList& list) {
				list.WriteTimestamp("Scene");
				list.SetViewport(
					{.width = static_cast<float>(targets.width), .height = static_cast<float>(targets.height)});
				list.SetScissor({.width = targets.width, .height = targets.height});
				BeginScene(list, targets);
				DrawScene(list, targets, rotation, shadowsEnabled);
			});

		// The caller/Device::Submit manages the output view's attachment and presentation state.
		graph.AddPass({.name = "Postprocess",
						  .resources =
							  {
								  {color, sampled, TextureLayout::ShaderRead},
							  }},
			[&](CommandList& list) {
				list.WriteTimestamp("Postprocess");
				DrawPostprocess(list, targets, output);
			});
		graph.Record(commands);
		commands.WriteTimestamp("Frame end");
		commands.EndDebugLabel();
		targets.initialized = true;
		targets.shadowInitialized = true;
	}
}
