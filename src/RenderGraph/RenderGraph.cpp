#include "RenderGraph.h"

#include "Device.h"
#include "TransientResourcePool.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <numeric>
#include <queue>
#include <sstream>

namespace RealRHI {
	namespace {
		CommandQueue* FindQueue(Device& device, QueueId id) {
			for (auto role : {QueueType::Graphics, QueueType::Compute, QueueType::Transfer}) {
				auto* queue = device.GetCommandQueue(role);
				if (queue && queue->GetQueueId() == id) {
					return queue;
				}
			}
			return nullptr;
		}

		void AppendBarriers(BarrierDesc& destination, const BarrierDesc& source) {
			destination.buffers.insert(destination.buffers.end(), source.buffers.begin(), source.buffers.end());
			destination.textures.insert(destination.textures.end(), source.textures.begin(), source.textures.end());
		}

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
		constexpr auto s_WriteAccess = ResourceAccess::MemoryWrite | ResourceAccess::ShaderWrite |
			ResourceAccess::ColorAttachmentWrite | ResourceAccess::DepthStencilWrite | ResourceAccess::TransferWrite;
		constexpr auto s_ReadAccess = ResourceAccess::MemoryRead | ResourceAccess::ShaderRead |
			ResourceAccess::ColorAttachmentRead | ResourceAccess::DepthStencilRead | ResourceAccess::TransferRead |
			ResourceAccess::IndexRead | ResourceAccess::VertexAttributeRead | ResourceAccess::IndirectCommandRead;

		bool HasAccess(ResourceAccess value, ResourceAccess mask) {
			return (static_cast<uint64_t>(value) & static_cast<uint64_t>(mask)) != 0;
		}

		bool Overlaps(uint32_t start, uint32_t count, uint32_t otherStart, uint32_t otherCount) {
			return uint64_t(start) < uint64_t(otherStart) + otherCount &&
				uint64_t(otherStart) < uint64_t(start) + count;
		}

		bool ValidLayout(TextureLayout layout) {
			return layout > TextureLayout::Undefined && layout <= TextureLayout::DepthStencilReadOnly;
		}

		bool ValidStateMasks(ResourceState state) {
			constexpr auto knownStages = PipelineStage::AllCommands | PipelineStage::VertexShader |
				PipelineStage::FragmentShader | PipelineStage::ComputeShader | PipelineStage::ColorAttachmentOutput |
				PipelineStage::EarlyFragmentTests | PipelineStage::LateFragmentTests | PipelineStage::Transfer |
				PipelineStage::DrawIndirect | PipelineStage::VertexInput;
			return (static_cast<uint64_t>(state.stages) & ~static_cast<uint64_t>(knownStages)) == 0 &&
				(static_cast<uint64_t>(state.access) & ~static_cast<uint64_t>(s_ReadAccess | s_WriteAccess)) == 0 &&
				(state.access == ResourceAccess::None || state.stages != PipelineStage::None);
		}

		bool ValidState(ResourceState state) {
			const auto access = static_cast<uint64_t>(state.access);
			const auto stages = static_cast<uint64_t>(state.stages);
			return stages != 0 && access != 0 && ValidStateMasks(state);
		}
	}

	RenderGraph::RenderGraph(DebugCallback callback)
		: m_Callback(callback), m_Id(s_NextGraphId++) {
		if (!m_Callback) {
			m_Callback = DefaultDebugCallback;
		}
	}

	void RenderGraph::Invalidate() {
		m_Compiled = false;
		m_MultiQueue = false;
		m_InitialReleases.clear();
		m_Batches.clear();
		m_UsedResources.clear();
		m_AliasSources.clear();
		m_Order.clear();
		m_Prerequisites.clear();
		m_Plan.clear();
		m_FinalBarriers = {};
		m_PresentationBarriers = {};
	}

	void RenderGraph::SetOptions(RenderGraphOptions options) {
		Invalidate();
		m_Options = options;
	}

	void RenderGraph::Reset() {
		Invalidate();
		m_Passes.clear();
		m_Resources.clear();
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

	void RenderGraph::ReleaseCreatedResources() {
		for (auto& resource : m_Resources) {
			if (IsCreated(resource)) {
				resource.texture.Reset();
				resource.buffer.Reset();
				resource.initialState = {};
				resource.aliasSource = UINT32_MAX;
			}
		}
	}

	void RenderGraph::BuildAliasPlan(std::span<const QueueId> queues, std::span<const uint8_t> poolingAllowed) {
		m_AliasSources.resize(m_Resources.size());
		std::iota(m_AliasSources.begin(), m_AliasSources.end(), 0u);
		if (!m_Options.enableAliasing) {
			return;
		}
		std::vector<uint32_t> firstUse(m_Resources.size(), UINT32_MAX);
		std::vector<uint32_t> lastUse(m_Resources.size(), UINT32_MAX);
		for (uint32_t position = 0; position < m_Order.size(); ++position) {
			for (const auto& use : m_Passes[m_Order[position]].desc.resources) {
				const auto index = use.resource.index;
				if (firstUse[index] == UINT32_MAX) {
					firstUse[index] = position;
				}
				lastUse[index] = position;
			}
		}
		std::vector<uint32_t> ordered(m_Resources.size());
		std::iota(ordered.begin(), ordered.end(), 0u);
		std::stable_sort(ordered.begin(), ordered.end(), [&](uint32_t left, uint32_t right) {
			return firstUse[left] < firstUse[right];
		});
		std::vector<uint32_t> occupants;
		for (uint32_t index : ordered) {
			const auto& resource = m_Resources[index];
			if (!IsCreated(resource) || resource.hasFinalState || firstUse[index] == UINT32_MAX ||
				(!poolingAllowed.empty() && !poolingAllowed[index]) ||
				(resource.bufferDesc && resource.bufferDesc->memoryUsage != MemoryUsage::GPUOnly)) {
				continue;
			}
			bool shared = false;
			for (auto& occupant : occupants) {
				const auto& previous = m_Resources[occupant];
				if (lastUse[occupant] >= firstUse[index] || queues[occupant] != queues[index]) {
					continue;
				}
				bool matches = false;
				if (resource.textureDesc && previous.textureDesc) {
					matches = TransientResourcePool::Matches(*resource.textureDesc, *previous.textureDesc);
				}
				else if (resource.bufferDesc && previous.bufferDesc) {
					matches = TransientResourcePool::Matches(*resource.bufferDesc, *previous.bufferDesc);
				}
				if (matches) {
					m_AliasSources[index] = m_AliasSources[occupant];
					occupant = index;
					shared = true;
					break;
				}
			}
			if (!shared) {
				occupants.push_back(index);
			}
		}
	}

	Result RenderGraph::AllocateResources(Device& device,
		std::span<const QueueId> queues,
		TransientResourcePool* pool,
		std::span<const uint8_t> poolingAllowed) {
		Invalidate();
		ReleaseCreatedResources();
		std::string error;
		if (!Compile(error)) {
			Report(error);
			return Result::InvalidParameter;
		}

		BuildAliasPlan(queues, poolingAllowed);

		// Commit only after the entire allocation batch succeeds. In-flight executions
		// retain their old resources through CommandList, independently of this graph.
		std::vector<Ref<Texture>> textures(m_Resources.size());
		std::vector<Ref<Buffer>> buffers(m_Resources.size());
		for (size_t i = 0; i < m_Resources.size(); ++i) {
			const auto& resource = m_Resources[i];
			if (!m_UsedResources[i] || m_AliasSources[i] != i) {
				continue;
			}
			const bool usePool = pool && (poolingAllowed.empty() || poolingAllowed[i]);
			Result result = Result::Success;
			if (resource.textureDesc) {
				auto desc = *resource.textureDesc;
				desc.debugName = resource.debugName.c_str();
				if (usePool) {
					result = pool->AcquireTexture(queues[i], desc, textures[i]);
				}
				else {
					result = device.CreateTexture(desc, textures[i]);
				}
			}
			else if (resource.bufferDesc) {
				auto desc = *resource.bufferDesc;
				desc.debugName = resource.debugName.c_str();
				if (usePool) {
					result = pool->AcquireBuffer(queues[i], desc, buffers[i]);
				}
				else {
					result = device.CreateBuffer(desc, buffers[i]);
				}
			}
			if (result != Result::Success) {
				Invalidate();
				return result;
			}
		}
		for (size_t i = 0; i < m_Resources.size(); ++i) {
			if (m_AliasSources[i] != i) {
				textures[i] = textures[m_AliasSources[i]];
				buffers[i] = buffers[m_AliasSources[i]];
			}
		}
		for (size_t i = 0; i < m_Resources.size(); ++i) {
			if (IsCreated(m_Resources[i]) && m_UsedResources[i]) {
				m_Resources[i].texture = std::move(textures[i]);
				m_Resources[i].buffer = std::move(buffers[i]);
				if (m_AliasSources[i] != i) {
					m_Resources[i].aliasSource = m_AliasSources[i];
				}
				if (m_AliasSources[i] != i || (pool && (poolingAllowed.empty() || poolingAllowed[i]))) {
					// Reused allocations stay on the same queue. Order previous accesses
					// before initialization, including image layout metadata writes.
					m_Resources[i].initialState = {
						PipelineStage::AllCommands, ResourceAccess::MemoryRead | ResourceAccess::MemoryWrite
					};
				}
			}
		}
		m_Plan.clear();
		m_FinalBarriers = {};
		if (!BuildBarriers(error)) {
			Invalidate();
			ReleaseCreatedResources();
			Report(error);
			return Result::InvalidParameter;
		}
		return Result::Success;
	}

	Texture* RenderGraph::GetTexture(RenderGraphResource handle) {
		if (!IsValid(handle) || !IsTexture(m_Resources[handle.index]) || !m_Resources[handle.index].texture) {
			Report("GetTexture requires a valid texture handle; Execute must allocate created resources before "
				   "resolution.");
			return nullptr;
		}
		return m_Resources[handle.index].texture.Raw();
	}

	Buffer* RenderGraph::GetBuffer(RenderGraphResource handle) {
		if (!IsValid(handle) || IsTexture(m_Resources[handle.index]) || !m_Resources[handle.index].buffer) {
			Report(
				"GetBuffer requires a valid buffer handle; Execute must allocate created resources before resolution.");
			return nullptr;
		}
		return m_Resources[handle.index].buffer.Raw();
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

	RenderGraphPass RenderGraph::AddPass(RenderGraphPassDesc desc, std::function<void(CommandList&)> record) {
		Invalidate();
		const auto index = static_cast<uint32_t>(m_Passes.size());
		m_Passes.push_back({std::move(desc), std::move(record)});
		return {index, m_Id};
	}

	void RenderGraph::AddDependency(RenderGraphPass pass, RenderGraphPass prerequisite) {
		Invalidate();
		m_Dependencies.push_back({pass, prerequisite});
	}

	bool RenderGraph::ValidateResources(std::string& error) const {
		if (m_FrameSwapchain &&
			(!m_Resources[m_BackBuffer].hasFinalState ||
				m_Resources[m_BackBuffer].finalLayout != TextureLayout::Present ||
				m_Resources[m_BackBuffer].finalState.stages != PipelineStage::AllCommands ||
				m_Resources[m_BackBuffer].finalState.access != ResourceAccess::MemoryRead)) {
			error = "An imported backbuffer must retain its automatic Present layout and AllCommands/MemoryRead final "
					"state.";
			return false;
		}
		for (size_t i = 0; i < m_Resources.size(); ++i) {
			const auto& resource = m_Resources[i];
			const std::string context = "Resource " + std::to_string(i) + ": ";
			if (!IsCreated(resource) && !resource.texture && !resource.buffer) {
				error = context + "import a non-null texture or buffer.";
				return false;
			}
			if (resource.bufferDesc &&
				(resource.bufferDesc->size == 0 || resource.bufferDesc->usage == BufferUsage::None ||
					resource.bufferDesc->initialData)) {
				error = context +
					"created buffers require nonzero size/usage and no initialData; initialize in a Write pass.";
				return false;
			}
			if (resource.textureDesc &&
				(resource.textureDesc->width == 0 || resource.textureDesc->height == 0 ||
					resource.textureDesc->depth == 0 || resource.textureDesc->mipLevels == 0 ||
					resource.textureDesc->arrayLayers == 0 || resource.textureDesc->usage == TextureUsage::None ||
					resource.textureDesc->format == TextureFormat::Unknown)) {
				error = context + "created textures require nonzero dimensions, mip/layer counts, usage, and a format.";
				return false;
			}
			if (resource.buffer &&
				(resource.size == 0 || resource.offset >= resource.buffer->GetSize() ||
					resource.size > resource.buffer->GetSize() - resource.offset)) {
				error = context + "buffer range must be nonempty and fit the buffer.";
				return false;
			}
			const auto& range = resource.subresources;
			if (IsTexture(resource) &&
				(range.mipLevelCount == 0 || range.arrayLayerCount == 0 ||
					uint64_t(range.baseMipLevel) + range.mipLevelCount > UINT32_MAX ||
					uint64_t(range.baseArrayLayer) + range.arrayLayerCount > UINT32_MAX ||
					(resource.initialLayout != TextureLayout::Undefined && !ValidLayout(resource.initialLayout)))) {
				error = context + "texture range and initial layout must be valid.";
				return false;
			}
			if (resource.hasFinalState &&
				(!ValidState(resource.finalState) || (IsTexture(resource) && !ValidLayout(resource.finalLayout)) ||
					(!IsTexture(resource) && resource.finalLayout != TextureLayout::Undefined))) {
				error = context + "final state requires stages/access and a valid texture layout.";
				return false;
			}

			for (size_t j = 0; j < i; ++j) {
				const auto& other = m_Resources[j];
				bool overlaps = false;
				if (!IsCreated(resource) && !IsCreated(other) && resource.buffer && resource.buffer == other.buffer) {
					overlaps =
						resource.offset < other.offset + other.size && other.offset < resource.offset + resource.size;
				}
				if (!IsCreated(resource) && !IsCreated(other) && resource.texture &&
					resource.texture == other.texture) {
					const auto& otherRange = other.subresources;
					overlaps = Overlaps(range.baseMipLevel,
								   range.mipLevelCount,
								   otherRange.baseMipLevel,
								   otherRange.mipLevelCount) &&
						Overlaps(range.baseArrayLayer,
							range.arrayLayerCount,
							otherRange.baseArrayLayer,
							otherRange.arrayLayerCount);
				}
				if (overlaps) {
					error = context + "overlaps another import; use the same handle or import disjoint ranges.";
					return false;
				}
			}
		}

		return true;
	}

	bool RenderGraph::ValidatePasses(std::string& error) const {
		for (const auto& pass : m_Passes) {
			const std::string context = "Pass '" + pass.desc.name + "': ";
			if (pass.desc.name.empty() || !pass.record) {
				error = context + "provide a name and recording callback.";
				return false;
			}
			std::vector<uint32_t> seen;
			for (const auto& use : pass.desc.resources) {
				if (!IsValid(use.resource)) {
					error = context + "resource handle belongs to another graph, was reset, or is invalid.";
					return false;
				}
				if (std::find(seen.begin(), seen.end(), use.resource.index) != seen.end()) {
					error = context + "declare each resource once; use ReadWrite for an in-place operation.";
					return false;
				}
				seen.push_back(use.resource.index);
				const auto& resource = m_Resources[use.resource.index];
				const bool reads = HasAccess(use.state.access, s_ReadAccess);
				const bool writes = HasAccess(use.state.access, s_WriteAccess);
				if (!ValidState(use.state) ||
					(use.access < RenderGraphAccess::Read || use.access > RenderGraphAccess::ReadWrite) ||
					(use.access == RenderGraphAccess::Read && (!reads || writes)) ||
					(use.access == RenderGraphAccess::Write && !writes) ||
					(use.access == RenderGraphAccess::ReadWrite && (!reads || !writes)) ||
					(IsTexture(resource) &&
						(!ValidLayout(use.layout) ||
							(writes &&
								(use.layout == TextureLayout::ShaderRead ||
									use.layout == TextureLayout::DepthStencilReadOnly ||
									use.layout == TextureLayout::Present)))) ||
					(!IsTexture(resource) && use.layout != TextureLayout::Undefined)) {
					error =
						context + "access mode, stage/access masks, and texture layout must describe the operation.";
					return false;
				}
			}
			for (auto dependency : pass.desc.dependsOn) {
				if (!IsValid(dependency)) {
					error = context + "dependency handle is invalid or belongs to another graph.";
					return false;
				}
			}
		}
		for (const auto& dependency : m_Dependencies) {
			if (!IsValid(dependency.pass) || !IsValid(dependency.prerequisite)) {
				error = "Dependency references an invalid pass handle.";
				return false;
			}
		}
		return true;
	}

	void RenderGraph::BuildDependencies() {
		struct ResourceUsers {
			std::vector<uint32_t> writers;
			std::map<TextureLayout, std::vector<uint32_t>> readers;
		};

		const auto count = m_Passes.size();
		m_Prerequisites.assign(count, {});
		std::vector<ResourceUsers> users(m_Resources.size());
		std::vector<std::vector<uint32_t>> additionalDependencies(count);
		for (const auto& dependency : m_Dependencies) {
			additionalDependencies[dependency.pass.index].push_back(dependency.prerequisite.index);
		}
		// A pass pair can share many resources or repeat an explicit dependency.
		// Dense pass indices let us deduplicate without searching adjacency lists.
		std::vector<uint32_t> seen(count, UINT32_MAX);

		for (uint32_t pass = 0; pass < count; ++pass) {
			auto& prerequisites = m_Prerequisites[pass];
			auto connect = [&](uint32_t prerequisite) {
				if (seen[prerequisite] != pass) {
					seen[prerequisite] = pass;
					prerequisites.push_back(prerequisite);
				}
			};
			for (const auto& use : m_Passes[pass].desc.resources) {
				auto& resource = users[use.resource.index];
				for (uint32_t writer : resource.writers) {
					connect(writer);
				}
				// Reads sharing a layout have no hazard. Skip that entire group,
				// so many compatible readers do not require pairwise comparisons.
				for (const auto& [layout, readers] : resource.readers) {
					if (use.access != RenderGraphAccess::Read || use.layout != layout) {
						for (uint32_t reader : readers) {
							connect(reader);
						}
					}
				}
				if (use.access == RenderGraphAccess::Read) {
					resource.readers[use.layout].push_back(pass);
				}
				else {
					resource.writers.push_back(pass);
				}
			}
			// Keep diagnostic ordering: implicit hazards in insertion order,
			// then descriptor dependencies, then AddDependency declarations.
			std::sort(prerequisites.begin(), prerequisites.end());
			for (auto dependency : m_Passes[pass].desc.dependsOn) {
				connect(dependency.index);
			}
			for (uint32_t dependency : additionalDependencies[pass]) {
				connect(dependency);
			}
		}
	}

	bool RenderGraph::BuildOrder(std::string& error) {
		BuildDependencies();
		const auto count = m_Passes.size();
		std::vector<std::vector<uint32_t>> dependents(count);
		std::vector<uint32_t> incoming(count);
		std::priority_queue<uint32_t, std::vector<uint32_t>, std::greater<uint32_t>> ready;
		for (uint32_t pass = 0; pass < count; ++pass) {
			incoming[pass] = static_cast<uint32_t>(m_Prerequisites[pass].size());
			if (incoming[pass] == 0) {
				ready.push(pass);
			}
			for (uint32_t prerequisite : m_Prerequisites[pass]) {
				dependents[prerequisite].push_back(pass);
			}
		}
		m_Order.reserve(count);
		while (!ready.empty()) {
			// A newly ready earlier pass takes precedence over later waiting work.
			const uint32_t next = ready.top();
			ready.pop();
			m_Order.push_back(next);
			for (uint32_t destination : dependents[next]) {
				if (--incoming[destination] == 0) {
					ready.push(destination);
				}
			}
		}
		if (m_Order.size() != count) {
			error = "Pass dependencies contain a cycle or contradict resource version order.";
			return false;
		}
		return true;
	}

	void RenderGraph::CullUnusedPasses() {
		if (m_Options.cullUnusedPasses) {
			std::vector<bool> live(m_Passes.size());
			std::vector<uint32_t> pending;
			std::vector<uint32_t> lastUse(m_Resources.size(), UINT32_MAX);
			for (uint32_t index : m_Order) {
				bool root = m_Passes[index].desc.hasSideEffects;
				for (const auto& use : m_Passes[index].desc.resources) {
					lastUse[use.resource.index] = index;
					if (!IsCreated(m_Resources[use.resource.index]) && use.access != RenderGraphAccess::Read) {
						root = true;
					}
				}
				if (root) {
					pending.push_back(index);
				}
			}
			for (size_t i = 0; i < m_Resources.size(); ++i) {
				if (m_Resources[i].hasFinalState && lastUse[i] != UINT32_MAX) {
					pending.push_back(lastUse[i]);
				}
			}
			while (!pending.empty()) {
				const auto index = pending.back();
				pending.pop_back();
				if (live[index]) {
					continue;
				}
				live[index] = true;
				pending.insert(pending.end(), m_Prerequisites[index].begin(), m_Prerequisites[index].end());
			}
			std::erase_if(m_Order, [&](uint32_t index) {
				return !live[index];
			});
		}
		m_UsedResources.assign(m_Resources.size(), !m_Options.cullUnusedPasses);
		for (auto index : m_Order) {
			for (const auto& use : m_Passes[index].desc.resources) {
				m_UsedResources[use.resource.index] = 1;
			}
		}
		for (size_t i = 0; i < m_Resources.size(); ++i) {
			if (m_Resources[i].hasFinalState) {
				m_UsedResources[i] = 1;
			}
		}
	}

	void RenderGraph::Transition(uint32_t index,
		ResourceState after,
		TextureLayout layout,
		TrackedState& before,
		BarrierDesc& barriers) {
		auto& resource = m_Resources[index];
		const bool writes = HasAccess(after.access, s_WriteAccess);
		const bool layoutChanged = before.layout != layout;
		const bool visible =
			std::any_of(before.visibleReads.begin(), before.visibleReads.end(), [&](ResourceState read) {
				return (static_cast<uint64_t>(after.stages) & ~static_cast<uint64_t>(read.stages)) == 0 &&
					(static_cast<uint64_t>(after.access) & ~static_cast<uint64_t>(read.access)) == 0;
			});
		if (!before.used || writes || layoutChanged || (HasAccess(before.state.access, s_WriteAccess) && !visible)) {
			if (IsTexture(resource)) {
				barriers.textures.push_back(
					{.texture = resource.texture.Raw(),
						.before = before.state,
						.after = after,
						.oldLayout = before.layout,
						.newLayout = layout,
						.subresources = resource.subresources});
			}
			else {
				barriers.buffers.push_back(
					{.buffer = resource.buffer.Raw(),
						.before = before.state,
						.after = after,
						.offset = resource.offset,
						.size = resource.size});
			}
		}

		if (writes || layoutChanged) {
			before.visibleReads.clear();
		}
		if (!writes && (!visible || layoutChanged)) {
			before.visibleReads.push_back(after);
		}
		if (writes) {
			before.state = after;
		}
		else {
			// Keep the last writer and all reader stages until the next write. Every
			// reader needs visibility, and later writes must wait for all earlier readers.
			before.state.stages = before.state.stages | after.stages;
			before.state.access = before.state.access | after.access;
		}
		before.layout = layout;
		before.used = true;
	}

	bool RenderGraph::BuildBarriers(std::string& error) {
		std::vector<TrackedState> states;
		for (const auto& resource : m_Resources) {
			bool initialized = !IsCreated(resource);
			if (IsTexture(resource) && resource.initialLayout == TextureLayout::Undefined) {
				initialized = false;
			}
			states.push_back({resource.initialState, resource.initialLayout, false, initialized});
		}
		for (uint32_t index : m_Order) {
			const auto& pass = m_Passes[index];
			RenderGraphStep step{.name = pass.desc.name, .queue = pass.desc.queue};
			for (const auto& use : pass.desc.resources) {
				auto& before = states[use.resource.index];
				if (!before.initialized && use.access != RenderGraphAccess::Write) {
					error =
						"Pass '" + pass.desc.name + "': undefined resource contents must be written before reading.";
					return false;
				}
				Transition(use.resource.index, use.state, use.layout, before, step.barriers);
				before.initialized = true;
			}
			m_Plan.push_back(std::move(step));
		}
		for (uint32_t i = 0; i < m_Resources.size(); ++i) {
			const auto& resource = m_Resources[i];
			if (resource.hasFinalState) {
				if (!states[i].initialized) {
					error = "Cannot export an undefined resource without a producing pass.";
					return false;
				}
				Transition(i, resource.finalState, resource.finalLayout, states[i], m_FinalBarriers);
			}
		}
		return true;
	}

	bool RenderGraph::Compile(std::string& error) {
		Invalidate();
		error.clear();
		if (!ValidateResources(error) || !ValidatePasses(error) || !BuildOrder(error)) {
			Invalidate();
			return false;
		}
		CullUnusedPasses();
		if (!BuildBarriers(error)) {
			Invalidate();
			return false;
		}
		m_Compiled = true;
		return true;
	}

	RenderGraphDiagnostics RenderGraph::GetDiagnostics() const {
		RenderGraphDiagnostics diagnostics{.compiled = m_Compiled, .multiQueue = m_MultiQueue};
		if (!m_Compiled) {
			return diagnostics;
		}
		for (uint32_t i = 0; i < m_Passes.size(); ++i) {
			const auto& pass = m_Passes[i].desc;
			RenderGraphPassInfo info{.pass = {i, m_Id}, .name = pass.name, .queue = pass.queue};
			for (auto prerequisite : m_Prerequisites[i]) {
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
			if (resource.texture) {
				info.allocationBytes = resource.texture->GetAllocationSize();
			}
			else if (resource.buffer) {
				info.allocationBytes = resource.buffer->GetAllocationSize();
			}
			if (resource.aliasSource != UINT32_MAX) {
				info.aliasOf = RenderGraphResource{resource.aliasSource, m_Id};
				diagnostics.aliasSavedBytes += info.allocationBytes;
			}
			else if (IsCreated(resource)) {
				diagnostics.transientBytes += info.allocationBytes;
			}
		}
		for (uint32_t position = 0; position < m_Order.size(); ++position) {
			const auto index = m_Order[position];
			diagnostics.passes[index].executionIndex = position;
			diagnostics.passes[index].nativeQueue = m_Plan[position].nativeQueue;
			diagnostics.passes[index].batchIndex = m_Plan[position].batchIndex;
			for (const auto& use : m_Passes[index].desc.resources) {
				auto& resource = diagnostics.resources[use.resource.index];
				if (resource.firstUse == UINT32_MAX) {
					resource.firstUse = position;
				}
				resource.lastUse = position;
			}
		}
		for (const auto& release : m_InitialReleases) {
			RenderGraphInitialBarriers info{.queue = release.queue, .barriers = release.barriers};
			for (auto consumer : release.consumers) {
				info.consumers.push_back({consumer, m_Id});
			}
			diagnostics.initialBarriers.push_back(std::move(info));
		}
		diagnostics.batches = m_Batches;
		diagnostics.presentationBarriers = m_PresentationBarriers;
		return diagnostics;
	}

	std::string RenderGraph::GetDebugDescription() const {
		if (!m_Compiled) {
			return "Render graph is not compiled.\n";
		}
		const auto diagnostics = GetDiagnostics();
		std::ostringstream text;
		text << "Render graph: " << m_Order.size() << " scheduled, " << m_Passes.size() - m_Order.size() << " culled";
		if (m_MultiQueue) {
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
			const auto& step = m_Plan[pass.executionIndex];
			text << "  barriers before:\n";
			DescribeBarriers(text, step.barriers);
			text << "  barriers after:\n";
			DescribeBarriers(text, step.afterBarriers);
		}
		if (m_FrameSwapchain && m_MultiQueue) {
			text << "Presentation barriers:\n";
			DescribeBarriers(text, m_PresentationBarriers);
		}
		// Multi-queue exports already appear after their last-use passes or in prefix batches.
		if (!m_MultiQueue) {
			text << "Final barriers:\n";
			DescribeBarriers(text, m_FinalBarriers);
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
					text << "  p" << m_Order[batch.steps.back()] << " -> present;\n";
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

	void RenderGraph::RecordPass(CommandList& commands, size_t position) {
		const auto& step = m_Plan[position];
		commands.ClearResourceDeclarations();
		for (const auto& use : m_Passes[m_Order[position]].desc.resources) {
			auto& resource = m_Resources[use.resource.index];
			if (IsTexture(resource)) {
				commands.RetainResource(resource.texture.Raw());
			}
			else {
				commands.RetainResource(resource.buffer.Raw());
			}
		}
		uint32_t timing = UINT32_MAX;
		if (m_Options.enableGpuTimings) {
			const auto name = "RenderGraph: " + step.name;
			timing = commands.BeginGpuTiming(name.c_str());
		}
		commands.Barrier(step.barriers);
		commands.BeginDebugLabel(step.name.c_str());
		m_Passes[m_Order[position]].record(commands);
		commands.EndDebugLabel();
		commands.ClearResourceDeclarations();
		commands.Barrier(step.afterBarriers);
		commands.EndGpuTiming(timing);
	}

	Result RenderGraph::Execute(Device& device, CommandList& commands, TransientResourcePool* pool) {
		if (m_FrameSwapchain) {
			Report("Graphs importing a backbuffer require Submit to synchronize and present the acquired image.");
			return Result::InvalidParameter;
		}
		if (commands.GetState() != CommandListState::Recording) {
			Report("Execute requires an active command list recording outside rendering.");
			return Result::InvalidParameter;
		}
		const std::vector<QueueId> queues(m_Resources.size(), commands.GetQueueId());
		const Result result = AllocateResources(device, queues, pool);
		if (result != Result::Success) {
			return result;
		}
		for (size_t i = 0; i < m_Plan.size(); ++i) {
			m_Plan[i].nativeQueue = commands.GetQueueId();
			RecordPass(commands, i);
		}
		commands.ClearResourceDeclarations();
		commands.Barrier(m_FinalBarriers);
		return Result::Success;
	}
	bool RenderGraphSubmission::IsComplete(Device& device) const {
		for (auto token : completions) {
			auto* queue = FindQueue(device, token.queue);
			if (!token.IsValid() || !queue || !queue->IsComplete(token)) {
				return false;
			}
		}
		return true;
	}

	void RenderGraphSubmission::Wait(Device& device) const {
		for (auto token : completions) {
			auto* queue = FindQueue(device, token.queue);
			if (!queue) {
				DefaultDebugCallback(
					{DebugSeverity::Error,
						DebugMessageType::General,
						"RenderGraphSubmission::Wait: Submission references an unknown queue."});
				return;
			}
			queue->Wait(token);
		}
	}

	bool RenderGraph::ValidateQueues(Device& device, std::string& error) const {
		auto validRole = [&](QueueType role) {
			return (role == QueueType::Graphics || role == QueueType::Compute || role == QueueType::Transfer) &&
				device.GetCommandQueue(role) != nullptr;
		};
		auto compatible = [](QueueType role, ResourceState state) {
			if (role == QueueType::Graphics) {
				return true;
			}
			auto allowed = PipelineStage::AllCommands | PipelineStage::Transfer;
			if (role == QueueType::Compute) {
				allowed = allowed | PipelineStage::ComputeShader | PipelineStage::DrawIndirect;
			}
			return (static_cast<uint64_t>(state.stages) & ~static_cast<uint64_t>(allowed)) == 0;
		};
		for (auto index : m_Order) {
			const auto& pass = m_Passes[index];
			if (!validRole(pass.desc.queue)) {
				error = "Pass '" + pass.desc.name + "': select an available queue role.";
				return false;
			}
			for (const auto& use : pass.desc.resources) {
				if (!compatible(pass.desc.queue, use.state)) {
					error = "Pass '" + pass.desc.name + "': resource stages are incompatible with its queue role.";
					return false;
				}
			}
		}
		for (uint32_t i = 0; i < m_Resources.size(); ++i) {
			const auto& resource = m_Resources[i];
			if (resource.initialQueue && !validRole(*resource.initialQueue)) {
				error = "Imported resource owner must identify an available queue role.";
				return false;
			}
			if (resource.hasFinalState) {
				auto role = resource.initialQueue.value_or(QueueType::Graphics);
				for (uint32_t index : m_Order) {
					for (const auto& use : m_Passes[index].desc.resources) {
						if (use.resource.index == i) {
							role = m_Passes[index].desc.queue;
						}
					}
				}
				if (!compatible(role, resource.finalState)) {
					error = "Final resource stages must be supported by its last-use queue.";
					return false;
				}
			}
		}

		return true;
	}

	void RenderGraph::BuildQueuePlan(Device& device, std::vector<InitialRelease>& initialReleases) {
		m_Plan.clear();
		m_Plan.reserve(m_Order.size());
		m_FinalBarriers = {};
		std::vector<TrackedState> states;
		std::vector<uint32_t> lastUse(m_Resources.size(), UINT32_MAX);
		for (const auto& resource : m_Resources) {
			states.push_back({resource.initialState, resource.initialLayout});
		}
		auto initialRelease = [&](QueueType role) -> InitialRelease& {
			for (auto& release : initialReleases) {
				if (release.queue == role) {
					return release;
				}
			}
			initialReleases.push_back({.queue = role});
			return initialReleases.back();
		};

		for (uint32_t position = 0; position < m_Order.size(); ++position) {
			const uint32_t passIndex = m_Order[position];
			const auto& pass = m_Passes[passIndex].desc;
			m_Plan.push_back({.name = pass.name, .queue = pass.queue});
			auto& step = m_Plan.back();
			const auto destination = device.GetCommandQueue(pass.queue)->GetQueueId();
			step.nativeQueue = destination;
			for (const auto& use : pass.resources) {
				const uint32_t resourceIndex = use.resource.index;
				auto& state = states[resourceIndex];
				const uint32_t previous = lastUse[resourceIndex];
				std::optional<QueueType> sourceRole = m_Resources[resourceIndex].initialQueue;
				if (previous != UINT32_MAX) {
					sourceRole = m_Plan[previous].queue;
				}
				bool transfer = false;
				bool differentQueue = false;
				if (sourceRole) {
					const auto source = device.GetCommandQueue(*sourceRole)->GetQueueId();
					transfer = source.familyIndex != destination.familyIndex;
					differentQueue = source != destination;
					if (previous != UINT32_MAX && source != destination) {
						// Exclusive resources require ordered ownership even for read/read uses.
						auto& dependencies = m_Prerequisites[passIndex];
						const uint32_t producer = m_Order[previous];
						if (std::find(dependencies.begin(), dependencies.end(), producer) == dependencies.end()) {
							dependencies.push_back(producer);
						}
					}
				}

				// Visibility earned on another queue cannot suppress this queue's acquire.
				// Resource lifetimes are retained separately by RecordPass.
				if (differentQueue) {
					state.visibleReads.clear();
				}
				// Image barriers also seed per-list layout knowledge for callback operations
				// such as GenerateMipmaps, before earlier lists have been submitted.
				if (differentQueue || IsTexture(m_Resources[resourceIndex])) {
					state.used = false;
				}
				BarrierDesc barriers;
				Transition(resourceIndex, use.state, use.layout, state, barriers);
				if (transfer) {
					for (auto& barrier : barriers.buffers) {
						barrier.sourceQueue = sourceRole;
						barrier.destinationQueue = pass.queue;
					}
					for (auto& barrier : barriers.textures) {
						barrier.sourceQueue = sourceRole;
						barrier.destinationQueue = pass.queue;
					}
					if (previous != UINT32_MAX) {
						AppendBarriers(m_Plan[previous].afterBarriers, barriers);
					}
					else {
						auto& release = initialRelease(*sourceRole);
						AppendBarriers(release.barriers, barriers);
						release.consumers.push_back(passIndex);
					}
					// Do not carry stage masks from a foreign queue family into later barriers.
					state.state = use.state;
				}
				if (resourceIndex == m_BackBuffer && previous == UINT32_MAX) {
					for (auto& barrier : barriers.textures) {
						barrier.acquireFromPresent = true;
					}
				}
				AppendBarriers(step.barriers, barriers);
				lastUse[resourceIndex] = position;
			}
		}
		for (uint32_t i = 0; i < m_Resources.size(); ++i) {
			const auto& resource = m_Resources[i];
			if (!resource.hasFinalState) {
				continue;
			}
			BarrierDesc barriers;
			if (i == m_BackBuffer) {
				states[i].used = false;
			}
			Transition(i, resource.finalState, resource.finalLayout, states[i], barriers);
			AppendBarriers(m_FinalBarriers, barriers);
			if (i == m_BackBuffer) {
				for (auto& barrier : barriers.textures) {
					barrier.releaseToPresent = true;
				}
				AppendBarriers(m_PresentationBarriers, barriers);
				continue;
			}
			if (lastUse[i] != UINT32_MAX) {
				AppendBarriers(m_Plan[lastUse[i]].afterBarriers, barriers);
			}
			else {
				AppendBarriers(initialRelease(resource.initialQueue.value_or(QueueType::Graphics)).barriers, barriers);
			}
		}
	}

	void RenderGraph::BuildBatches(Device& device) {
		m_Batches.clear();
		bool acquired = false;
		QueueType presentationQueue = QueueType::Graphics;
		std::vector<uint32_t> passBatches(m_Passes.size(), UINT32_MAX);
		std::vector<std::vector<uint32_t>> consumers(m_Passes.size());
		std::vector<std::vector<uint32_t>> initialWaits(m_Passes.size());
		for (uint32_t i = 0; i < m_InitialReleases.size(); ++i) {
			const auto& release = m_InitialReleases[i];
			m_Batches.push_back(
				{.queue = release.queue,
					.nativeQueue = device.GetCommandQueue(release.queue)->GetQueueId(),
					.initialRelease = i});
			for (auto consumer : release.consumers) {
				initialWaits[consumer].push_back(i);
			}
		}
		for (uint32_t index : m_Order) {
			const auto queue = device.GetCommandQueue(m_Passes[index].desc.queue)->GetQueueId();
			for (auto prerequisite : m_Prerequisites[index]) {
				if (device.GetCommandQueue(m_Passes[prerequisite].desc.queue)->GetQueueId() != queue) {
					consumers[prerequisite].push_back(index);
				}
			}
		}

		for (uint32_t position = 0; position < m_Order.size(); ++position) {
			const auto index = m_Order[position];
			auto& step = m_Plan[position];
			const auto queue = *step.nativeQueue;
			std::vector<uint32_t> dependencies = initialWaits[index];
			for (auto prerequisite : m_Prerequisites[index]) {
				const auto batch = passBatches[prerequisite];
				if (m_Batches[batch].nativeQueue != queue) {
					dependencies.push_back(batch);
				}
			}
			std::sort(dependencies.begin(), dependencies.end());
			dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());

			const auto& uses = m_Passes[index].desc.resources;
			const bool backBuffer = std::any_of(uses.begin(), uses.end(), [&](const auto& use) {
				return use.resource.index == m_BackBuffer;
			});
			const bool firstBackBufferUse = backBuffer && !acquired;
			if (backBuffer) {
				presentationQueue = step.queue;
			}
			bool merge = false;
			if (!m_Batches.empty()) {
				const auto& previous = m_Batches.back();
				if (!firstBackBufferUse && !previous.steps.empty() && previous.nativeQueue == queue) {
					// Do not move a later pass's new wait ahead of earlier independent work.
					merge = std::includes(previous.prerequisites.begin(),
						previous.prerequisites.end(),
						dependencies.begin(),
						dependencies.end());
					for (auto earlier : previous.steps) {
						for (auto consumer : consumers[m_Order[earlier]]) {
							const auto& prerequisites = m_Prerequisites[consumer];
							if (std::find(prerequisites.begin(), prerequisites.end(), index) == prerequisites.end()) {
								// Preserve an early signal unless its consumer already needs this pass too.
								merge = false;
								break;
							}
						}
						if (!merge) {
							break;
						}
					}
				}
			}
			if (!merge) {
				m_Batches.push_back(
					{.queue = step.queue, .nativeQueue = queue, .prerequisites = std::move(dependencies)});
			}
			const auto batch = static_cast<uint32_t>(m_Batches.size() - 1);
			m_Batches.back().steps.push_back(position);
			passBatches[index] = batch;
			step.batchIndex = batch;
			if (firstBackBufferUse) {
				m_Batches.back().waitForAcquire = true;
				acquired = true;
			}
		}
		if (m_FrameSwapchain) {
			RenderGraphBatchInfo present{
				.queue = presentationQueue,
				.nativeQueue = device.GetCommandQueue(presentationQueue)->GetQueueId(),
				.presentation = true
			};
			for (uint32_t i = 0; i < m_Batches.size(); ++i) {
				if (m_Batches[i].nativeQueue != present.nativeQueue) {
					present.prerequisites.push_back(i);
				}
			}
			m_Batches.push_back(std::move(present));
		}
	}

	Result RenderGraph::Submit(Device& device,
		RenderGraphSubmission& outSubmission,
		TransientResourcePool* pool,
		std::span<const SubmissionToken> waitFor) {
		outSubmission.completions.clear();
		if (m_FrameSwapchain && !m_FrameSwapchain->IsFrameReady(m_Frame)) {
			Report("Submit requires a current unsubmitted swapchain frame; acquire another frame before rebuilding the "
				   "graph.");
			return Result::InvalidParameter;
		}
		std::string error;
		if (!Compile(error) || !ValidateQueues(device, error)) {
			Report(error);
			return Result::InvalidParameter;
		}
		for (auto token : waitFor) {
			if (!token.IsValid() || !FindQueue(device, token.queue)) {
				Report("Submit requires valid external wait tokens from known queues.");
				return Result::InvalidParameter;
			}
		}

		std::vector<QueueId> resourceQueues(m_Resources.size(), device.GetGraphicsCommandQueue()->GetQueueId());
		std::vector<uint8_t> poolingAllowed(m_Resources.size(), 1);
		std::vector<bool> used(m_Resources.size());
		for (auto index : m_Order) {
			const auto queue = device.GetCommandQueue(m_Passes[index].desc.queue)->GetQueueId();
			for (const auto& use : m_Passes[index].desc.resources) {
				const auto resource = use.resource.index;
				if (!used[resource]) {
					resourceQueues[resource] = queue;
				}
				else if (resourceQueues[resource] != queue) {
					poolingAllowed[resource] = 0;
				}
				used[resource] = true;
			}
		}
		const auto allocated = AllocateResources(device, resourceQueues, pool, poolingAllowed);
		if (allocated != Result::Success) {
			return allocated;
		}
		auto& initialReleases = m_InitialReleases;
		BuildQueuePlan(device, initialReleases);
		m_MultiQueue = true;

		BuildBatches(device);
		std::vector<Ref<CommandList>> commands(m_Batches.size());

		// Create and begin every list before invoking callbacks, then finish all recording
		// before submitting any work. Queue submission retains successful lists in flight.
		for (size_t i = 0; i < m_Batches.size(); ++i) {
			Ref<CommandAllocator> allocator;
			auto result = device.CreateCommandAllocator(m_Batches[i].queue, allocator);
			if (result != Result::Success) {
				return result;
			}
			result = device.CreateCommandList(allocator.Raw(), commands[i]);
			if (result != Result::Success) {
				return result;
			}
			result = commands[i]->Begin();
			if (result != Result::Success) {
				return result;
			}
		}
		for (size_t i = 0; i < m_Batches.size(); ++i) {
			const auto& batch = m_Batches[i];
			if (batch.initialRelease) {
				commands[i]->Barrier(initialReleases[*batch.initialRelease].barriers);
			}
			for (auto position : batch.steps) {
				RecordPass(*commands[i], position);
			}
			if (batch.presentation) {
				commands[i]->Barrier(m_PresentationBarriers);
			}
			const auto ended = commands[i]->End();
			if (ended != Result::Success) {
				return ended;
			}
		}
		for (size_t i = 0; i < m_Batches.size(); ++i) {
			const auto& batch = m_Batches[i];
			std::vector<SubmissionToken> waits(waitFor.begin(), waitFor.end());
			for (auto dependency : batch.prerequisites) {
				waits.push_back(outSubmission.completions[dependency]);
			}
			std::array<CommandList*, 1> lists{commands[i].Raw()};
			SubmitInfo submit{.commandLists = lists, .waitFor = waits};
			if (batch.waitForAcquire || batch.presentation) {
				submit.swapchain = SwapchainSubmitSync{
					.swapchain = m_FrameSwapchain.Raw(),
					.frame = m_Frame,
					.waitForAcquire = batch.waitForAcquire,
					.signalForPresent = batch.presentation
				};
			}
			const auto token = device.GetCommandQueue(batch.queue)->Submit(submit);
			if (!token.IsValid()) {
				Report("Submit failed; returned completion tokens cover the work already queued.");
				return Result::Failed;
			}
			outSubmission.completions.push_back(token);
		}
		if (m_FrameSwapchain) {
			return m_FrameSwapchain->Present(m_Frame);
		}
		return Result::Success;
	}

}
