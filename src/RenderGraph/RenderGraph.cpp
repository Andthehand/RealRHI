#include "RenderGraph.h"

#include <algorithm>
#include <atomic>

namespace RealRHI {
	namespace {
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
		m_Order.clear();
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

	RenderGraphResource RenderGraph::ImportTexture(Texture* texture,
		TextureLayout layout,
		ResourceState state,
		TextureSubresourceRange subresources) {
		Invalidate();
		const auto index = static_cast<uint32_t>(m_Resources.size());
		m_Resources.push_back(
			{.texture = Ref<Texture>(texture),
				.subresources = subresources,
				.initialState = state,
				.initialLayout = layout});
		return {index, m_Id};
	}

	RenderGraphResource RenderGraph::ImportBuffer(Buffer* buffer, ResourceState state, uint64_t offset, uint64_t size) {
		Invalidate();
		if (size == 0 && buffer && offset < buffer->GetSize()) {
			size = buffer->GetSize() - offset;
		}
		const auto index = static_cast<uint32_t>(m_Resources.size());
		m_Resources.push_back({.buffer = Ref<Buffer>(buffer), .offset = offset, .size = size, .initialState = state});
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
			Report("SetFinalState requires a resource imported into this graph.");
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
			if (!resource.texture && !resource.buffer) {
				error = context + "import a non-null texture or buffer.";
				return false;
			}
			if (resource.buffer &&
				(resource.size == 0 || resource.offset >= resource.buffer->GetSize() ||
					resource.size > resource.buffer->GetSize() - resource.offset)) {
				error = context + "buffer range must be nonempty and fit the buffer.";
				return false;
			}
			const auto& range = resource.subresources;
			if (resource.texture &&
				(range.mipLevelCount == 0 || range.arrayLayerCount == 0 ||
					uint64_t(range.baseMipLevel) + range.mipLevelCount > UINT32_MAX ||
					uint64_t(range.baseArrayLayer) + range.arrayLayerCount > UINT32_MAX ||
					(resource.initialLayout != TextureLayout::Undefined && !ValidLayout(resource.initialLayout)))) {
				error = context + "texture range and initial layout must be valid.";
				return false;
			}
			if (resource.hasFinalState &&
				(!ValidState(resource.finalState) || (resource.texture && !ValidLayout(resource.finalLayout)) ||
					(resource.buffer && resource.finalLayout != TextureLayout::Undefined))) {
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
					(resource.texture &&
						(!ValidLayout(use.layout) ||
							(writes &&
								(use.layout == TextureLayout::ShaderRead ||
									use.layout == TextureLayout::DepthStencilReadOnly ||
									use.layout == TextureLayout::Present)))) ||
					(resource.buffer && use.layout != TextureLayout::Undefined)) {
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
		auto connect = [&](uint32_t from, uint32_t to) {
			auto& destinations = edges[from];
			if (std::find(destinations.begin(), destinations.end(), to) == destinations.end()) {
				destinations.push_back(to);
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
			if (resource.texture) {
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
			states.push_back({resource.initialState, resource.initialLayout});
		}
		for (uint32_t index : m_Order) {
			const auto& pass = m_Passes[index];
			RenderGraphStep step{.name = pass.desc.name};
			for (const auto& use : pass.desc.resources) {
				auto& before = states[use.resource.index];
				const auto& resource = m_Resources[use.resource.index];
				if (resource.texture && before.layout == TextureLayout::Undefined &&
					use.access != RenderGraphAccess::Write) {
					error = "Pass '" + pass.desc.name + "': undefined texture contents must be written before reading.";
					return false;
				}
				Transition(use.resource.index, use.state, use.layout, before, step.barriers);
			}
			m_Plan.push_back(std::move(step));
		}
		for (uint32_t i = 0; i < m_Resources.size(); ++i) {
			const auto& resource = m_Resources[i];
			if (resource.hasFinalState) {
				if (resource.texture && states[i].layout == TextureLayout::Undefined) {
					error = "Cannot export an undefined texture without a producing pass.";
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
		m_Compiled = true;
		return true;
	}

	void RenderGraph::Report(const std::string& error) const {
		const std::string message = "RenderGraph: " + error;
		m_Callback({DebugSeverity::Error, DebugMessageType::General, message.c_str()});
	}

	void RenderGraph::Record(CommandList& commands) {
		if (commands.GetState() != CommandListState::Recording) {
			Report("Record requires an active command list recording outside rendering.");
			return;
		}
		std::string error;
		if (!m_Compiled && !Compile(error)) {
			Report(error);
			return;
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
	}
}
