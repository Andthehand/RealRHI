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
		m_TransientResources = std::make_unique<TransientResourcePool>(device);
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

	Result SceneRenderer::CreateGeometry(UploadContext& uploads, Ref<Buffer>& vertices, Ref<Buffer>& indices) {
		const Geometry geometry = BuildGeometry();
		const uint64_t vertexBytes = geometry.vertices.size() * sizeof(Vertex);
		const uint64_t indexBytes = geometry.indices.size() * sizeof(uint32_t);

		Result result = m_Device->CreateBuffer(
			{
				.size = vertexBytes,
				.usage = BufferUsage::Vertex | BufferUsage::TransferDst,
				.debugName = "Scene vertices",
			},
			vertices);
		if (result != Result::Success) {
			return result;
		}

		result = m_Device->CreateBuffer(
			{.size = indexBytes, .usage = BufferUsage::Index | BufferUsage::TransferDst, .debugName = "Scene indices"},
			indices);
		if (result != Result::Success) {
			return result;
		}

		result = uploads.UploadBuffer(
			{
				.destination = vertices.Raw(),
				.finalState = {PipelineStage::VertexInput, ResourceAccess::VertexAttributeRead},
			},
			geometry.vertices.data(),
			vertexBytes);
		if (result != Result::Success) {
			return result;
		}

		return uploads.UploadBuffer(
			{
				.destination = indices.Raw(),
				.finalState = {PipelineStage::VertexInput, ResourceAccess::IndexRead},
			},
			geometry.indices.data(),
			indexBytes);
	}

	Result SceneRenderer::CreatePattern(UploadContext& uploads, Ref<Texture>& pattern, bool mipmapped, uint32_t seed) {
		uint32_t mipLevels = 1;
		if (mipmapped) {
			mipLevels = 6;
		}
		Result result = m_Device->CreateTexture(
			{
				.width = 32,
				.height = 32,
				.mipLevels = mipLevels,
				.format = TextureFormat::RGBA8_UNorm,
				.usage = TextureUsage::ShaderResource | TextureUsage::TransferSrc | TextureUsage::TransferDst,
				.debugName = "Scene checkerboard",
			},
			pattern);
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
				pixels[offset] = static_cast<uint8_t>(shade - seed % 64);
				pixels[offset + 1] = shade;
				pixels[offset + 2] = shade;
				pixels[offset + 3] = 255;
			}
		}

		TextureLayout finalLayout = TextureLayout::ShaderRead;
		ResourceState finalState{PipelineStage::FragmentShader, ResourceAccess::ShaderRead};
		if (mipmapped) {
			finalLayout = TextureLayout::TransferDst;
			finalState = {PipelineStage::Transfer, ResourceAccess::TransferWrite};
		}
		return uploads.UploadTexture(
			{
				.destination = pattern.Raw(),
				.finalLayout = finalLayout,
				.finalState = finalState,
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

		result = CreateGeometry(*uploads, m_VertexBuffer, m_IndexBuffer);
		if (result != Result::Success) {
			return result;
		}

		result = CreatePattern(*uploads, m_Pattern);
		if (result != Result::Success) {
			return result;
		}

		if (m_ObjectCount != 0) {
			uint32_t assetCount = 1;
			if (m_UniqueAssets) {
				assetCount = m_ObjectCount;
			}
			m_ObjectAssets.resize(assetCount);
			for (uint32_t i = 0; i < assetCount; ++i) {
				auto& assets = m_ObjectAssets[i];
				result = CreateGeometry(*uploads, assets.vertices, assets.indices);
				if (result != Result::Success) {
					return result;
				}
				result = CreatePattern(*uploads, assets.pattern, false, i);
				if (result != Result::Success) {
					return result;
				}
			}
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

	void SceneRenderer::ConfigureWorkload(uint32_t objectCount, bool uniqueAssets) {
		m_ObjectCount = objectCount;
		m_UniqueAssets = uniqueAssets;
	}

	TransientResourcePoolStatistics SceneRenderer::GetPoolStatistics() const {
		return m_TransientResources->GetStatistics();
	}

	Result SceneRenderer::StreamAssets(uint32_t count, uint32_t frame, UploadSubmission& outUpload) {
		outUpload = {};
		if (count == 0 || m_ObjectAssets.empty()) {
			return Result::Success;
		}
		if (!m_StreamingUploads) {
			const auto result = m_Device->CreateUploadContext({.queueType = QueueType::Graphics}, m_StreamingUploads);
			if (result != Result::Success) {
				return result;
			}
		}
		auto result = m_StreamingUploads->BeginBatch();
		if (result != Result::Success) {
			return result;
		}
		const auto geometry = BuildGeometry();
		std::array<uint8_t, 32 * 32 * 4> pixels;
		pixels.fill(static_cast<uint8_t>(100 + frame % 120));
		for (size_t i = 3; i < pixels.size(); i += 4) {
			pixels[i] = 255;
		}
		const uint32_t assetCount = static_cast<uint32_t>(m_ObjectAssets.size());
		count = std::min(count, assetCount);
		for (uint32_t i = 0; i < count; ++i) {
			auto& assets = m_ObjectAssets[(static_cast<uint64_t>(frame) * count + i) % assetCount];
			result = m_StreamingUploads->UploadBuffer(
				{.destination = assets.vertices.Raw(),
					.before = {PipelineStage::VertexInput, ResourceAccess::VertexAttributeRead},
					.finalState = {PipelineStage::VertexInput, ResourceAccess::VertexAttributeRead}},
				geometry.vertices.data(),
				geometry.vertices.size() * sizeof(Vertex));
			if (result == Result::Success) {
				result = m_StreamingUploads->UploadTexture(
					{.destination = assets.pattern.Raw(),
						.oldLayout = TextureLayout::ShaderRead,
						.before = {PipelineStage::FragmentShader, ResourceAccess::ShaderRead},
						.finalState = {PipelineStage::FragmentShader, ResourceAccess::ShaderRead}},
					pixels.data(),
					pixels.size());
			}
			if (result != Result::Success) {
				m_StreamingUploads->CancelBatch();
				return result;
			}
		}
		return m_StreamingUploads->Submit(outUpload);
	}

	Result SceneRenderer::ResizeTargets(uint32_t frameIndex, uint32_t width, uint32_t height) {
		if (frameIndex >= m_Frames.size() || width == 0 || height == 0) {
			return Result::InvalidParameter;
		}
		if (m_Frames[frameIndex].width != width || m_Frames[frameIndex].height != height) {
			m_TransientResources->Trim();
		}
		m_Frames[frameIndex] = {width, height};
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
		if (m_ObjectCount != 0) {
			const uint32_t side = static_cast<uint32_t>(std::ceil(std::sqrt(m_ObjectCount)));
			const float spacing = 4.5f / side;
			for (uint32_t i = 0; i < m_ObjectCount; ++i) {
				auto& assets = m_ObjectAssets[i % m_ObjectAssets.size()];
				commands.BindVertexBuffer(assets.vertices.Raw());
				commands.BindIndexBuffer(assets.indices.Raw());
				// Keep declarations local to each draw so tracking reflects its actual material.
				commands.ClearResourceDeclarations();
				if (pass == GeometryPass::Lighting) {
					commands.UseResource(assets.pattern.Raw());
					commands.UseResource(m_Sampler.Raw());
					commands.UseResource(targets.shadow);
					commands.UseResource(m_ShadowSampler.Raw());
				}
				parameters.textureIndex = assets.pattern->GetSampledImageDescriptorIndex();
				parameters.translationAndScale = {
					(i % side + 0.5f) * spacing - 2.25f, -0.4f, (i / side + 0.5f) * spacing - 2.25f, spacing * 0.7f
				};
				parameters.tint = {0.3f + (i % 7) * 0.1f, 0.4f + (i % 5) * 0.1f, 0.8f, 1};
				commands.SetPushConstants(&parameters, sizeof(parameters));
				commands.DrawIndexed(36, 1, 6, 4);
			}
			return;
		}
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

	void SceneRenderer::DrawShadowMap(CommandList& commands,
		FrameTargets& targets,
		float rotation,
		bool shadowsEnabled) {
		commands.SetViewport(
			{.width = static_cast<float>(m_ShadowResolution), .height = static_cast<float>(m_ShadowResolution)});
		commands.SetScissor({.width = m_ShadowResolution, .height = m_ShadowResolution});
		commands.BeginRendering(
			{.hasDepth = true,
				.depthAttachment = {.target = targets.shadow->GetTextureView()},
				.renderArea = {.width = m_ShadowResolution, .height = m_ShadowResolution}});
		if (shadowsEnabled) {
			commands.BindPipeline(m_ShadowPipeline.Raw());
			DrawGeometry(commands, targets, rotation, GeometryPass::Shadow, false);
		}
		commands.EndRendering();
	}

	void SceneRenderer::DrawScene(CommandList& commands, FrameTargets& targets, float rotation, bool shadowsEnabled) {
		commands.BindPipeline(m_OpaquePipeline.Raw());
		commands.UseResource(m_Pattern.Raw());
		commands.UseResource(m_Sampler.Raw());
		commands.UseResource(targets.shadow);
		commands.UseResource(m_ShadowSampler.Raw());

		DrawGeometry(commands, targets, rotation, GeometryPass::Lighting, shadowsEnabled);
		commands.EndRendering();
	}

	void SceneRenderer::DrawPostprocess(CommandList& commands, FrameTargets& targets, TextureView* output) {
		commands.SetViewport(
			{.width = static_cast<float>(targets.width), .height = static_cast<float>(targets.height)});
		commands.SetScissor({.width = targets.width, .height = targets.height});

		commands.BeginRendering({
			.colorAttachments = {{.target = output}},
			.renderArea = {.width = targets.width, .height = targets.height},
		});
		commands.BindPipeline(m_PostprocessPipeline.Raw());
		commands.UseResource(targets.color);
		commands.UseResource(m_Sampler.Raw());

		const uint32_t parameters[] = {
			targets.color->GetSampledImageDescriptorIndex(),
			m_Sampler->GetDescriptorIndex(),
		};
		commands.SetPushConstants(parameters, sizeof(parameters));
		commands.Draw(3);
		commands.EndRendering();
	}

	Result SceneRenderer::Record(CommandList& commands,
		uint32_t frameIndex,
		TextureView* output,
		float rotation,
		const UploadSubmission& pendingUpload,
		bool shadowsEnabled,
		RenderGraphExecutionTimings* timings) {
		RenderGraph graph;
		const auto built = BuildGraph(graph, frameIndex, output, rotation, pendingUpload, shadowsEnabled);
		if (built != Result::Success) {
			return built;
		}
		commands.BeginDebugLabel("Scene frame");
		const auto result = graph.Execute(*m_Device, commands, m_TransientResources.get(), timings);
		commands.EndDebugLabel();
		return result;
	}

	Result SceneRenderer::Submit(Swapchain& swapchain,
		const FrameContext& frame,
		float rotation,
		const UploadSubmission& pendingUpload,
		RenderGraphSubmission& outSubmission,
		bool shadowsEnabled) {
		RenderGraph graph;
		const auto output = graph.ImportBackBuffer(swapchain, frame);
		if (output.index == UINT32_MAX) {
			return Result::InvalidParameter;
		}
		const auto built =
			BuildGraph(graph, frame.frameIndex, frame.backBufferView, rotation, pendingUpload, shadowsEnabled, output);
		if (built != Result::Success) {
			return built;
		}
		std::span<const SubmissionToken> waits;
		if (pendingUpload.completion.IsValid()) {
			waits = {&pendingUpload.completion, 1};
		}
		return graph.Submit(*m_Device, outSubmission, m_TransientResources.get(), waits);
	}

	Result SceneRenderer::BuildGraph(RenderGraph& graph,
		uint32_t frameIndex,
		TextureView* output,
		float rotation,
		const UploadSubmission& pendingUpload,
		bool shadowsEnabled,
		std::optional<RenderGraphResource> outputResource) {
		if (frameIndex >= m_Frames.size() || m_Frames[frameIndex].width == 0 || m_Frames[frameIndex].height == 0) {
			return Result::InvalidParameter;
		}
		const auto& size = m_Frames[frameIndex];

		const ResourceState sampled{PipelineStage::FragmentShader, ResourceAccess::ShaderRead};
		const ResourceState colorAttachment{
			PipelineStage::ColorAttachmentOutput,
			ResourceAccess::ColorAttachmentRead | ResourceAccess::ColorAttachmentWrite
		};
		const ResourceState depthAttachment{
			PipelineStage::EarlyFragmentTests | PipelineStage::LateFragmentTests,
			ResourceAccess::DepthStencilRead | ResourceAccess::DepthStencilWrite
		};
		graph.SetOptions({.cullUnusedPasses = true, .enableGpuTimings = true});
		const auto color = graph.CreateTexture(
			{.width = size.width,
				.height = size.height,
				.format = TextureFormat::RGBA8_UNorm,
				.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource,
				.debugName = "Scene color"});
		const auto depth = graph.CreateTexture(
			{.width = size.width,
				.height = size.height,
				.format = m_DepthFormat,
				.usage = TextureUsage::DepthStencil,
				.debugName = "Scene depth"});
		const auto shadow = graph.CreateTexture(
			{.width = m_ShadowResolution,
				.height = m_ShadowResolution,
				.format = m_DepthFormat,
				.usage = TextureUsage::DepthStencil | TextureUsage::ShaderResource,
				.debugName = "Scene shadow map"});
		auto resolveTargets = [&graph, color, depth, shadow, width = size.width, height = size.height] {
			return FrameTargets{
				.color = graph.GetTexture(color),
				.depth = graph.GetTexture(depth),
				.shadow = graph.GetTexture(shadow),
				.width = width,
				.height = height
			};
		};
		const auto pattern = graph.ImportTexture(m_Pattern.Raw(), TextureLayout::ShaderRead, sampled, {0, 6, 0, 1});
		const ResourceState vertexState{PipelineStage::VertexInput, ResourceAccess::VertexAttributeRead};
		const ResourceState indexState{PipelineStage::VertexInput, ResourceAccess::IndexRead};
		const auto vertices = graph.ImportBuffer(m_VertexBuffer.Raw(), vertexState);
		const auto indices = graph.ImportBuffer(m_IndexBuffer.Raw(), indexState);
		std::vector<RenderGraphUse> objectGeometry;
		std::vector<RenderGraphUse> objectTextures;
		for (auto& assets : m_ObjectAssets) {
			objectGeometry.push_back({graph.ImportBuffer(assets.vertices.Raw(), vertexState), vertexState});
			objectGeometry.push_back({graph.ImportBuffer(assets.indices.Raw(), indexState), indexState});
			objectTextures.push_back(
				{graph.ImportTexture(assets.pattern.Raw(), TextureLayout::ShaderRead, sampled),
					sampled,
					TextureLayout::ShaderRead});
		}
		auto withObjects = [&](std::vector<RenderGraphUse> resources, bool textures) {
			resources.insert(resources.end(), objectGeometry.begin(), objectGeometry.end());
			if (textures) {
				resources.insert(resources.end(), objectTextures.begin(), objectTextures.end());
			}
			return resources;
		};

		const auto setup = graph.AddPass({.name = "Frame setup"}, [this, pendingUpload, sampled](CommandList& list) {
			if (pendingUpload.completion.IsValid()) {
				list.Barrier(pendingUpload.acquireBarriers);
				list.GenerateMipmaps(m_Pattern.Raw(), {.finalState = sampled});
			}
		});

		// Initialize the sampled map for every execution, including disabled shadows.
		graph.AddPass(
			{.name = "Shadows",
				.resources = withObjects(
					{
						{shadow, depthAttachment, TextureLayout::DepthStencilAttachment, RenderGraphAccess::Write},
						{vertices, vertexState},
						{indices, indexState},
					},
					false),
				.dependsOn = {setup}},
			[this, resolveTargets, rotation, shadowsEnabled](CommandList& list) {
				auto targets = resolveTargets();
				DrawShadowMap(list, targets, rotation, shadowsEnabled);
			});

		graph.AddPass(
			{.name = "Scene",
				.resources = withObjects(
					{
						{color, colorAttachment, TextureLayout::ColorAttachment, RenderGraphAccess::Write},
						{depth, depthAttachment, TextureLayout::DepthStencilAttachment, RenderGraphAccess::Write},
						{shadow, sampled, TextureLayout::ShaderRead},
						{pattern, sampled, TextureLayout::ShaderRead},
						{vertices, vertexState},
						{indices, indexState},
					},
					true)},
			[this, resolveTargets, rotation, shadowsEnabled](CommandList& list) {
				auto targets = resolveTargets();
				list.SetViewport(
					{.width = static_cast<float>(targets.width), .height = static_cast<float>(targets.height)});
				list.SetScissor({.width = targets.width, .height = targets.height});
				BeginScene(list, targets);
				DrawScene(list, targets, rotation, shadowsEnabled);
			});

		// Offscreen Record callers manage the output; frame submission declares it here.
		std::vector<RenderGraphUse> outputs{{color, sampled, TextureLayout::ShaderRead}};
		if (outputResource) {
			outputs.push_back(
				{*outputResource, colorAttachment, TextureLayout::ColorAttachment, RenderGraphAccess::Write});
		}
		graph.AddPass({.name = "Postprocess", .resources = std::move(outputs), .hasSideEffects = true},
			[this, resolveTargets, output](CommandList& list) {
				auto targets = resolveTargets();
				DrawPostprocess(list, targets, output);
			});
		return Result::Success;
	}
}
