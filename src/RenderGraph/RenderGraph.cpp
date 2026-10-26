#include "RenderGraph.h"

#include "Device.h"
#include "TransientResourcePool.h"

#include <algorithm>
#include <array>
#include <atomic>

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
		m_Order.clear();
		m_Prerequisites.clear();
		m_Plan.clear();
		m_FinalBarriers = {};
	}

	void RenderGraph::Reset() {
		Invalidate();
		m_Passes.clear();
		m_Resources.clear();
		m_Dependencies.clear();
		m_Id = s_NextGraphId++;
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

		// Commit only after the entire allocation batch succeeds. In-flight executions
		// retain their old resources through CommandList, independently of this graph.
		std::vector<Ref<Texture>> textures(m_Resources.size());
		std::vector<Ref<Buffer>> buffers(m_Resources.size());
		for (size_t i = 0; i < m_Resources.size(); ++i) {
			const auto& resource = m_Resources[i];
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
			if (IsCreated(m_Resources[i])) {
				m_Resources[i].texture = std::move(textures[i]);
				m_Resources[i].buffer = std::move(buffers[i]);
				if (pool && (poolingAllowed.empty() || poolingAllowed[i])) {
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
				if (resource.buffer && resource.buffer == other.buffer) {
					overlaps =
						resource.offset < other.offset + other.size && other.offset < resource.offset + resource.size;
				}
				if (resource.texture && resource.texture == other.texture) {
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

	bool RenderGraph::BuildOrder(std::string& error) {
		const auto count = m_Passes.size();
		std::vector<std::vector<uint32_t>> edges(count);
		std::vector<uint32_t> incoming(count);
		m_Prerequisites.resize(count);
		auto connect = [&](uint32_t from, uint32_t to) {
			auto& destinations = edges[from];
			if (std::find(destinations.begin(), destinations.end(), to) == destinations.end()) {
				destinations.push_back(to);
				m_Prerequisites[to].push_back(from);
				++incoming[to];
			}
		};

		for (uint32_t i = 0; i < count; ++i) {
			for (auto dependency : m_Passes[i].desc.dependsOn) {
				connect(dependency.index, i);
			}
			for (uint32_t j = i + 1; j < count; ++j) {
				for (const auto& before : m_Passes[i].desc.resources) {
					for (const auto& after : m_Passes[j].desc.resources) {
						if (before.resource.index == after.resource.index &&
							(before.access != RenderGraphAccess::Read || after.access != RenderGraphAccess::Read ||
								before.layout != after.layout)) {
							connect(i, j);
						}
					}
				}
			}
		}
		for (const auto& dependency : m_Dependencies) {
			connect(dependency.prerequisite.index, dependency.pass.index);
		}

		std::vector<bool> scheduled(count);
		while (m_Order.size() < count) {
			uint32_t next = UINT32_MAX;
			for (uint32_t i = 0; i < count; ++i) {
				if (!scheduled[i] && incoming[i] == 0) {
					next = i;
					break;
				}
			}
			if (next == UINT32_MAX) {
				error = "Pass dependencies contain a cycle or contradict resource version order.";
				return false;
			}
			scheduled[next] = true;
			m_Order.push_back(next);
			for (uint32_t destination : edges[next]) {
				--incoming[destination];
			}
		}
		return true;
	}

	void RenderGraph::Transition(uint32_t index,
		ResourceState after,
		TextureLayout layout,
		TrackedState& before,
		BarrierDesc& barriers) {
		auto& resource = m_Resources[index];
		if (!before.used || HasAccess(before.state.access, s_WriteAccess) || HasAccess(after.access, s_WriteAccess) ||
			before.layout != layout) {
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

		if (HasAccess(after.access, s_WriteAccess)) {
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
		if (!ValidateResources(error) || !ValidatePasses(error) || !BuildOrder(error) || !BuildBarriers(error)) {
			Invalidate();
			return false;
		}
		return true;
	}

	void RenderGraph::Report(const std::string& error) const {
		const std::string message = "RenderGraph: " + error;
		m_Callback({DebugSeverity::Error, DebugMessageType::General, message.c_str()});
	}

	Result RenderGraph::Execute(Device& device, CommandList& commands, TransientResourcePool* pool) {
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
			const auto& step = m_Plan[i];
			commands.ClearResourceDeclarations();
			commands.Barrier(step.barriers);
			commands.BeginDebugLabel(step.name.c_str());
			m_Passes[m_Order[i]].record(commands);
			commands.EndDebugLabel();
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
		for (const auto& pass : m_Passes) {
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
			for (const auto& use : pass.resources) {
				const uint32_t resourceIndex = use.resource.index;
				auto& state = states[resourceIndex];
				const uint32_t previous = lastUse[resourceIndex];
				std::optional<QueueType> sourceRole = m_Resources[resourceIndex].initialQueue;
				if (previous != UINT32_MAX) {
					sourceRole = m_Plan[previous].queue;
				}
				bool transfer = false;
				if (sourceRole) {
					const auto source = device.GetCommandQueue(*sourceRole)->GetQueueId();
					transfer = source.familyIndex != destination.familyIndex;
					if (previous != UINT32_MAX && source != destination) {
						// Exclusive resources require ordered ownership even for read/read uses.
						auto& dependencies = m_Prerequisites[passIndex];
						const uint32_t producer = m_Order[previous];
						if (std::find(dependencies.begin(), dependencies.end(), producer) == dependencies.end()) {
							dependencies.push_back(producer);
						}
					}
				}

				// Each pass has its own list. Even read/read uses need a retained reference
				// in that list, independent of earlier submissions completing and retiring.
				state.used = false;
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
			Transition(i, resource.finalState, resource.finalLayout, states[i], barriers);
			AppendBarriers(m_FinalBarriers, barriers);
			if (lastUse[i] != UINT32_MAX) {
				AppendBarriers(m_Plan[lastUse[i]].afterBarriers, barriers);
			}
			else {
				AppendBarriers(initialRelease(resource.initialQueue.value_or(QueueType::Graphics)).barriers, barriers);
			}
		}
	}

	Result RenderGraph::Submit(Device& device,
		RenderGraphSubmission& outSubmission,
		TransientResourcePool* pool,
		std::span<const SubmissionToken> waitFor) {
		outSubmission.completions.clear();
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
		std::vector<InitialRelease> initialReleases;
		BuildQueuePlan(device, initialReleases);

		struct Batch {
			QueueType queue;
			Ref<CommandList> commands;
			std::vector<size_t> dependencies;
		};
		std::vector<Batch> batches;
		for (const auto& release : initialReleases) {
			batches.push_back({.queue = release.queue});
		}
		std::vector<size_t> passBatches(m_Passes.size());
		for (uint32_t index : m_Order) {
			passBatches[index] = batches.size();
			batches.push_back({.queue = m_Passes[index].desc.queue});
		}
		for (uint32_t index : m_Order) {
			for (auto dependency : m_Prerequisites[index]) {
				batches[passBatches[index]].dependencies.push_back(passBatches[dependency]);
			}
		}
		for (size_t i = 0; i < initialReleases.size(); ++i) {
			for (auto consumer : initialReleases[i].consumers) {
				batches[passBatches[consumer]].dependencies.push_back(i);
			}
		}

		// Create and begin every list before invoking callbacks, then finish all recording
		// before submitting any work. Queue submission retains successful lists in flight.
		for (auto& batch : batches) {
			Ref<CommandAllocator> allocator;
			auto result = device.CreateCommandAllocator(batch.queue, allocator);
			if (result != Result::Success) {
				return result;
			}
			result = device.CreateCommandList(allocator.Raw(), batch.commands);
			if (result != Result::Success) {
				return result;
			}
			result = batch.commands->Begin();
			if (result != Result::Success) {
				return result;
			}
		}
		for (size_t i = 0; i < batches.size(); ++i) {
			auto& commands = *batches[i].commands;
			if (i < initialReleases.size()) {
				commands.Barrier(initialReleases[i].barriers);
			}
			else {
				const size_t position = i - initialReleases.size();
				const auto& step = m_Plan[position];
				commands.Barrier(step.barriers);
				commands.BeginDebugLabel(step.name.c_str());
				m_Passes[m_Order[position]].record(commands);
				commands.EndDebugLabel();
				commands.ClearResourceDeclarations();
				commands.Barrier(step.afterBarriers);
			}
			const auto ended = commands.End();
			if (ended != Result::Success) {
				return ended;
			}
		}
		for (auto& batch : batches) {
			std::vector<SubmissionToken> waits(waitFor.begin(), waitFor.end());
			for (size_t dependency : batch.dependencies) {
				waits.push_back(outSubmission.completions[dependency]);
			}
			std::array<CommandList*, 1> lists{batch.commands.Raw()};
			const auto token = device.GetCommandQueue(batch.queue)->Submit({.commandLists = lists, .waitFor = waits});
			if (!token.IsValid()) {
				Report("Submit failed; returned completion tokens cover the work already queued.");
				return Result::Failed;
			}
			outSubmission.completions.push_back(token);
		}
		return Result::Success;
	}

}
