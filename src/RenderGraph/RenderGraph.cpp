#include "RenderGraph.h"

#include <atomic>
#include <sstream>
#include <utility>

namespace RealRHI {
	namespace {
		const char* QueueName(QueueType queue) {
			switch (queue) {
				case QueueType::Graphics:
					return "Graphics";
				case QueueType::Compute:
					return "Compute";
				case QueueType::Transfer:
					return "Transfer";
			}

			return "Unknown";
		}

		const char* LayoutName(TextureLayout layout) {
			switch (layout) {
				case TextureLayout::Undefined:
					return "Undefined";
				case TextureLayout::General:
					return "General";
				case TextureLayout::ColorAttachment:
					return "ColorAttachment";
				case TextureLayout::DepthStencilAttachment:
					return "DepthStencilAttachment";
				case TextureLayout::ShaderRead:
					return "ShaderRead";
				case TextureLayout::TransferSrc:
					return "TransferSrc";
				case TextureLayout::TransferDst:
					return "TransferDst";
				case TextureLayout::Present:
					return "Present";
				case TextureLayout::DepthStencilReadOnly:
					return "DepthStencilReadOnly";
			}

			return "Unknown";
		}

		std::string StateName(ResourceState state) {
			std::string stages;
			std::string access;
			auto append = [](std::string& text, const char* name) {
				if (!text.empty()) {
					text += "|";
				}

				text += name;
			};

			for (const auto& [flag, name] : {
					 std::pair{PipelineStage::AllCommands, "AllCommands"},
					 std::pair{PipelineStage::VertexShader, "VertexShader"},
					 std::pair{PipelineStage::FragmentShader, "FragmentShader"},
					 std::pair{PipelineStage::ComputeShader, "ComputeShader"},
					 std::pair{PipelineStage::ColorAttachmentOutput, "ColorAttachmentOutput"},
					 std::pair{PipelineStage::EarlyFragmentTests, "EarlyFragmentTests"},
					 std::pair{PipelineStage::LateFragmentTests, "LateFragmentTests"},
					 std::pair{PipelineStage::Transfer, "Transfer"},
					 std::pair{PipelineStage::DrawIndirect, "DrawIndirect"},
					 std::pair{PipelineStage::VertexInput, "VertexInput"},
				 }) {
				if ((static_cast<uint64_t>(state.stages) & static_cast<uint64_t>(flag)) != 0) {
					append(stages, name);
				}
			}

			for (const auto& [flag, name] : {
					 std::pair{ResourceAccess::MemoryRead, "MemoryRead"},
					 std::pair{ResourceAccess::MemoryWrite, "MemoryWrite"},
					 std::pair{ResourceAccess::ShaderRead, "ShaderRead"},
					 std::pair{ResourceAccess::ShaderWrite, "ShaderWrite"},
					 std::pair{ResourceAccess::ColorAttachmentRead, "ColorAttachmentRead"},
					 std::pair{ResourceAccess::ColorAttachmentWrite, "ColorAttachmentWrite"},
					 std::pair{ResourceAccess::DepthStencilRead, "DepthStencilRead"},
					 std::pair{ResourceAccess::DepthStencilWrite, "DepthStencilWrite"},
					 std::pair{ResourceAccess::TransferRead, "TransferRead"},
					 std::pair{ResourceAccess::TransferWrite, "TransferWrite"},
					 std::pair{ResourceAccess::IndexRead, "IndexRead"},
					 std::pair{ResourceAccess::VertexAttributeRead, "VertexAttributeRead"},
					 std::pair{ResourceAccess::IndirectCommandRead, "IndirectCommandRead"},
				 }) {
				if ((static_cast<uint64_t>(state.access) & static_cast<uint64_t>(flag)) != 0) {
					append(access, name);
				}
			}

			if (stages.empty()) {
				stages = "None";
			}

			if (access.empty()) {
				access = "None";
			}

			return stages + " / " + access;
		}

		void DescribeBarriers(std::ostringstream& text, const BarrierDesc& barriers) {
			auto describe = [&](const auto& barrier) {
				text << " [" << StateName(barrier.before) << "] -> [" << StateName(barrier.after) << "]";
				if (barrier.sourceQueue && barrier.destinationQueue) {
					text << " ownership " << QueueName(*barrier.sourceQueue) << " -> "
						 << QueueName(*barrier.destinationQueue);
				}

				text << '\n';
			};

			for (const auto& barrier : barriers.buffers) {
				text << "    buffer offset=" << barrier.offset << " size=" << barrier.size;
				describe(barrier);
			}

			for (const auto& barrier : barriers.textures) {
				text << "    texture mips=" << barrier.subresources.baseMipLevel << "+"
					 << barrier.subresources.mipLevelCount << " layers=" << barrier.subresources.baseArrayLayer << "+"
					 << barrier.subresources.arrayLayerCount << " " << LayoutName(barrier.oldLayout) << " -> "
					 << LayoutName(barrier.newLayout);
				if (barrier.acquireFromPresent) {
					text << " acquire from Present";
				}

				if (barrier.releaseToPresent) {
					text << " release to Present";
				}

				describe(barrier);
			}
		}

		std::string EscapeDot(const std::string& value) {
			std::string result;

			for (char character : value) {
				if (character == '\\' || character == '"') {
					result += '\\';
				}

				if (character == '\n' || character == '\r') {
					result += "\\n";
				}
				else {
					result += character;
				}
			}

			return result;
		}

		std::atomic<uint64_t> s_NextGraphId{1};

	}

	RenderGraph::RenderGraph(DebugCallback callback)
		: m_Callback(callback), m_Id(s_NextGraphId++) {
		if (!m_Callback) {
			m_Callback = DefaultDebugCallback;
		}
	}

	void RenderGraph::Invalidate() {
		m_Compiled = {};

		// Preserve resolved outputs until the next execution or Reset.
		m_Execution.plan = {};
		m_Execution.hasSnapshot = false;
		m_Execution.steps.clear();
		m_Execution.finalBarriers = {};
		m_Execution.presentationBarriers = {};
		m_Execution.aliasSources.clear();
	}

	void RenderGraph::SetOptions(RenderGraphOptions options) {
		Invalidate();
		m_Options = options;
	}

	void RenderGraph::Reset() {
		Invalidate();
		m_Passes.clear();
		m_Resources.clear();
		ReleaseCreatedResources();
		m_Dependencies.clear();
		m_Id = s_NextGraphId++;
		m_FrameSwapchain.Reset();
		m_Frame = {};
		m_BackBuffer = UINT32_MAX;
	}

	bool RenderGraph::IsTexture(const Resource& resource) {
		return resource.texture || resource.textureDesc.has_value();
	}

	bool RenderGraph::IsCreated(const Resource& resource) {
		return resource.textureDesc.has_value() || resource.bufferDesc.has_value();
	}

	RenderGraphResource RenderGraph::CreateTexture(const TextureDesc& desc) {
		Invalidate();
		Resource resource;
		resource.textureDesc = desc;
		resource.textureDesc->debugName = nullptr;
		if (desc.debugName) {
			resource.debugName = desc.debugName;
		}

		resource.subresources = {0, desc.mipLevels, 0, desc.arrayLayers};
		const auto index = static_cast<uint32_t>(m_Resources.size());
		m_Resources.push_back(std::move(resource));

		return {index, m_Id};
	}

	RenderGraphResource RenderGraph::CreateBuffer(const BufferDesc& desc) {
		Invalidate();
		Resource resource;
		resource.bufferDesc = desc;
		resource.bufferDesc->debugName = nullptr;
		if (desc.debugName) {
			resource.debugName = desc.debugName;
		}

		resource.size = desc.size;
		const auto index = static_cast<uint32_t>(m_Resources.size());
		m_Resources.push_back(std::move(resource));

		return {index, m_Id};
	}

	Texture* RenderGraph::GetTexture(RenderGraphResource handle) {
		if (!IsValid(handle) || !IsTexture(m_Resources[handle.index]) || !ResolveTexture(handle.index)) {
			Report("GetTexture requires a valid texture handle; Execute must allocate created resources before "
				   "resolution.");
			return nullptr;
		}

		return ResolveTexture(handle.index);
	}

	Buffer* RenderGraph::GetBuffer(RenderGraphResource handle) {
		if (!IsValid(handle) || IsTexture(m_Resources[handle.index]) || !ResolveBuffer(handle.index)) {
			Report(
				"GetBuffer requires a valid buffer handle; Execute must allocate created resources before resolution.");
			return nullptr;
		}

		return ResolveBuffer(handle.index);
	}

	RenderGraphResource RenderGraph::ImportTexture(Texture* texture,
		TextureLayout layout,
		ResourceState state,
		TextureSubresourceRange subresources,
		std::optional<QueueType> owner) {
		Invalidate();
		const auto index = static_cast<uint32_t>(m_Resources.size());
		m_Resources.push_back(
			{.texture = Ref<Texture>(texture),
				.subresources = subresources,
				.initialState = state,
				.initialLayout = layout,
				.initialQueue = owner});

		return {index, m_Id};
	}

	RenderGraphResource RenderGraph::ImportBuffer(Buffer* buffer,
		ResourceState state,
		uint64_t offset,
		uint64_t size,
		std::optional<QueueType> owner) {
		Invalidate();
		if (size == 0 && buffer && offset < buffer->GetSize()) {
			size = buffer->GetSize() - offset;
		}

		const auto index = static_cast<uint32_t>(m_Resources.size());
		m_Resources.push_back(
			{.buffer = Ref<Buffer>(buffer),
				.offset = offset,
				.size = size,
				.initialState = state,
				.initialQueue = owner});

		return {index, m_Id};
	}

	bool RenderGraph::IsValid(RenderGraphResource resource) const {
		return resource.graph == m_Id && resource.index < m_Resources.size();
	}

	bool RenderGraph::IsValid(RenderGraphPass pass) const {
		return pass.graph == m_Id && pass.index < m_Passes.size();
	}

	RenderGraphResource RenderGraph::ImportBackBuffer(Swapchain& swapchain, const FrameContext& frame) {
		if (m_FrameSwapchain || !swapchain.IsFrameReady(frame)) {
			Report("ImportBackBuffer requires a current, unsubmitted frame and at most one swapchain frame per graph.");
			return {};
		}

		auto* texture = swapchain.GetBackBuffer(frame);
		if (!texture) {
			Report("ImportBackBuffer could not resolve the acquired image.");
			return {};
		}

		const auto resource = ImportTexture(texture, TextureLayout::Undefined);
		SetFinalState(resource, {PipelineStage::AllCommands, ResourceAccess::MemoryRead}, TextureLayout::Present);
		m_FrameSwapchain = &swapchain;
		m_Frame = frame;
		m_BackBuffer = resource.index;

		return resource;
	}

	void RenderGraph::SetFinalState(RenderGraphResource resource, ResourceState state, TextureLayout layout) {
		Invalidate();
		if (!IsValid(resource)) {
			Report("SetFinalState requires a resource declared in this graph.");
			return;
		}

		auto& value = m_Resources[resource.index];
		value.hasFinalState = true;
		value.finalState = state;
		value.finalLayout = layout;
	}

	RenderGraphPass RenderGraph::AddPass(RenderGraphPassDesc desc, std::function<void(RenderGraphContext&)> record) {
		Invalidate();
		const auto index = static_cast<uint32_t>(m_Passes.size());
		m_Passes.push_back({std::move(desc), std::move(record)});

		return {index, m_Id};
	}

	void RenderGraph::AddDependency(RenderGraphPass pass, RenderGraphPass prerequisite) {
		Invalidate();
		m_Dependencies.push_back({pass, prerequisite});
	}

	RenderGraphDiagnostics RenderGraph::GetDiagnostics() const {
		EnsureSnapshot();
		RenderGraphDiagnostics diagnostics{.compiled = m_Compiled.valid, .multiQueue = m_Execution.plan.multiQueue};
		if (!m_Compiled.valid) {
			return diagnostics;
		}

		for (uint32_t i = 0; i < m_Passes.size(); ++i) {
			const auto& pass = m_Passes[i].desc;
			RenderGraphPassInfo info{.pass = {i, m_Id}, .name = pass.name, .queue = pass.queue};

			for (auto prerequisite : (m_Execution.plan.multiQueue ? m_Execution.plan.prerequisites : m_Compiled.prerequisites)[i]) {
				info.prerequisites.push_back({prerequisite, m_Id});
			}

			diagnostics.passes.push_back(std::move(info));
		}

		for (uint32_t i = 0; i < m_Resources.size(); ++i) {
			const auto& resource = m_Resources[i];
			diagnostics.resources.push_back(
				{.resource = {i, m_Id},
					.name = resource.debugName,
					.texture = IsTexture(resource),
					.imported = !IsCreated(resource),
					.exported = resource.hasFinalState});
		}

		for (uint32_t i = 0; i < m_Resources.size(); ++i) {
			const auto& resource = m_Resources[i];
			auto& info = diagnostics.resources[i];
			if (auto* texture = ResolveTexture(i)) {
				info.allocationBytes = texture->GetAllocationSize();
			}
			else if (auto* buffer = ResolveBuffer(i)) {
				info.allocationBytes = buffer->GetAllocationSize();
			}

			if (!m_Execution.aliasSources.empty() && m_Execution.aliasSources[i] != i) {
				info.aliasOf = RenderGraphResource{m_Execution.aliasSources[i], m_Id};
				diagnostics.aliasSavedBytes += info.allocationBytes;
			}
			else if (IsCreated(resource)) {
				diagnostics.transientBytes += info.allocationBytes;
			}
		}

		for (uint32_t position = 0; position < m_Compiled.order.size(); ++position) {
			const auto index = m_Compiled.order[position];
			diagnostics.passes[index].executionIndex = position;
			diagnostics.passes[index].nativeQueue = m_Execution.steps[position].nativeQueue;
			diagnostics.passes[index].batchIndex = m_Execution.steps[position].batchIndex;

			for (const auto& use : m_Passes[index].desc.resources) {
				auto& resource = diagnostics.resources[use.resource.index];
				if (resource.firstUse == UINT32_MAX) {
					resource.firstUse = position;
				}

				resource.lastUse = position;
			}
		}

		for (const auto& release : m_Execution.plan.initialReleases) {
			RenderGraphInitialBarriers info{.queue = release.queue, .barriers = ResolveBarriers(release.barriers)};

			for (auto consumer : release.consumers) {
				info.consumers.push_back({consumer, m_Id});
			}

			diagnostics.initialBarriers.push_back(std::move(info));
		}

		diagnostics.batches = m_Execution.plan.batches;
		diagnostics.presentationBarriers = m_Execution.presentationBarriers;

		return diagnostics;
	}

	std::string RenderGraph::GetDebugDescription() const {
		if (!m_Compiled.valid) {
			return "Render graph is not compiled.\n";
		}

		const auto diagnostics = GetDiagnostics();
		std::ostringstream text;
		text << "Render graph: " << m_Compiled.order.size() << " scheduled, " << m_Passes.size() - m_Compiled.order.size() << " culled";
		if (m_Execution.plan.multiQueue) {
			text << " (multi-queue)";
		}

		text << '\n';

		for (size_t i = 0; i < diagnostics.batches.size(); ++i) {
			const auto& batch = diagnostics.batches[i];
			text << "Batch " << i << " on " << QueueName(batch.queue) << " native queue "
				 << batch.nativeQueue.familyIndex << ":" << batch.nativeQueue.queueIndex << " steps:";

			for (auto step : batch.steps) {
				text << " " << step;
			}

			if (batch.initialRelease) {
				text << " initial barriers " << *batch.initialRelease;
			}

			if (batch.waitForAcquire) {
				text << " waits for acquire";
			}

			if (batch.presentation) {
				text << " signals presentation";
			}

			text << " waits:";

			for (auto dependency : batch.prerequisites) {
				text << " " << dependency;
			}

			text << '\n';
		}

		for (const auto& release : diagnostics.initialBarriers) {
			text << "Initial barriers on " << QueueName(release.queue) << " consumers:";

			for (auto consumer : release.consumers) {
				text << " p" << consumer.index;
			}

			text << '\n';
			DescribeBarriers(text, release.barriers);
		}

		for (const auto& pass : diagnostics.passes) {
			text << "Pass p" << pass.pass.index << " \"" << pass.name << "\" on " << QueueName(pass.queue);
			if (pass.executionIndex == UINT32_MAX) {
				text << " [culled]\n";
				continue;
			}

			if (pass.nativeQueue) {
				text << " native queue " << pass.nativeQueue->familyIndex << ":" << pass.nativeQueue->queueIndex;
			}

			text << " [step " << pass.executionIndex << "]";
			if (pass.batchIndex != UINT32_MAX) {
				text << " [batch " << pass.batchIndex << "]";
			}

			text << "\n  prerequisites:";

			for (auto prerequisite : pass.prerequisites) {
				text << " p" << prerequisite.index;
			}

			text << '\n';

			for (const auto& use : m_Passes[pass.pass.index].desc.resources) {
				const char* access = "read";
				if (use.access == RenderGraphAccess::Write) {
					access = "write";
				}
				else if (use.access == RenderGraphAccess::ReadWrite) {
					access = "read/write";
				}

				text << "  r" << use.resource.index << " " << access << " [" << StateName(use.state) << "]";
				if (IsTexture(m_Resources[use.resource.index])) {
					text << " " << LayoutName(use.layout);
				}

				text << '\n';
			}

			const auto& step = m_Execution.steps[pass.executionIndex];
			text << "  barriers before:\n";
			DescribeBarriers(text, step.barriers);
			text << "  barriers after:\n";
			DescribeBarriers(text, step.afterBarriers);
		}

		if (m_FrameSwapchain && m_Execution.plan.multiQueue) {
			text << "Presentation barriers:\n";
			DescribeBarriers(text, m_Execution.presentationBarriers);
		}

		// Multi-queue exports already appear after their last-use passes or in prefix batches.
		if (!m_Execution.plan.multiQueue) {
			text << "Final barriers:\n";
			DescribeBarriers(text, m_Execution.finalBarriers);
		}

		text << "Transient allocation bytes: " << diagnostics.transientBytes
			 << ", saved by aliasing: " << diagnostics.aliasSavedBytes << '\n';
		text << "Resource lifetimes (schedule positions; not GPU overlap):\n";

		for (const auto& resource : diagnostics.resources) {
			text << "  r" << resource.resource.index << " \"" << resource.name << "\" ";
			if (resource.imported) {
				text << "imported ";
			}
			else {
				text << "created ";
			}

			if (resource.texture) {
				text << "texture";
			}
			else {
				text << "buffer";
			}

			if (resource.exported) {
				text << " exported";
			}

			if (resource.aliasOf) {
				text << " aliases r" << resource.aliasOf->index;
			}

			if (resource.firstUse == UINT32_MAX) {
				text << " unused";
			}
			else {
				text << " [" << resource.firstUse << ", " << resource.lastUse << "]";
			}

			text << '\n';
		}

		return text.str();
	}

	std::string RenderGraph::GetDependencyGraphDot() const {
		std::ostringstream text;
		text << "digraph RenderGraph {\n  rankdir=LR;\n";
		const auto diagnostics = GetDiagnostics();

		for (size_t i = 0; i < diagnostics.initialBarriers.size(); ++i) {
			const auto& release = diagnostics.initialBarriers[i];
			text << "  initial" << i << " [shape=box,label=\"Initial barriers\\n"
				 << QueueName(release.queue) << "\"];\n";

			for (auto consumer : release.consumers) {
				text << "  initial" << i << " -> p" << consumer.index << ";\n";
			}
		}

		for (const auto& pass : diagnostics.passes) {
			text << "  p" << pass.pass.index << " [label=\"" << EscapeDot(pass.name) << "\\n" << QueueName(pass.queue);
			if (pass.executionIndex == UINT32_MAX) {
				text << "\\nculled\",style=dashed];\n";
			}
			else {
				text << "\\nstep " << pass.executionIndex;
				if (pass.batchIndex != UINT32_MAX) {
					text << "\\nbatch " << pass.batchIndex;
				}

				text << "\"];\n";
			}

			for (auto dependency : pass.prerequisites) {
				text << "  p" << dependency.index << " -> p" << pass.pass.index << ";\n";
			}
		}

		if (m_FrameSwapchain && !diagnostics.batches.empty()) {
			text << "  present [shape=box,label=\"Present\"];\n";

			for (const auto& batch : diagnostics.batches) {
				if (!batch.steps.empty()) {
					text << "  p" << m_Compiled.order[batch.steps.back()] << " -> present;\n";
				}
				else if (batch.initialRelease) {
					text << "  initial" << *batch.initialRelease << " -> present;\n";
				}
			}
		}

		text << "}\n";

		return text.str();
	}

	void RenderGraph::Report(const std::string& error) const {
		const std::string message = "RenderGraph: " + error;
		m_Callback({DebugSeverity::Error, DebugMessageType::General, message.c_str()});
	}

}
