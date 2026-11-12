#include "RenderGraph.h"

#include "Detail/CpuProfiling.h"

#include <algorithm>
#include <map>
#include <queue>
#include <unordered_map>

namespace RealRHI {
	namespace {
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

	bool RenderGraph::ValidateResources(std::string& error) const {
		std::unordered_map<const Buffer*, std::map<uint64_t, uint64_t>> bufferRanges;
		std::unordered_map<const Texture*, std::vector<TextureSubresourceRange>> textureRanges;
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

			if (IsCreated(resource)) {
				continue;
			}

			if (resource.buffer) {
				auto& ranges = bufferRanges[resource.buffer.Raw()];
				const uint64_t end = resource.offset + resource.size;
				const auto next = ranges.lower_bound(resource.offset);

				// Accepted ranges never overlap, so only the insertion neighbors can conflict.
				if ((next != ranges.end() && next->first < end) ||
					(next != ranges.begin() && std::prev(next)->second > resource.offset)) {
					error = context + "overlaps another import; use the same handle or import disjoint ranges.";
					return false;
				}

				ranges.emplace_hint(next, resource.offset, end);
			}

			if (resource.texture) {
				auto& ranges = textureRanges[resource.texture.Raw()];
				const bool overlaps = std::any_of(ranges.begin(), ranges.end(), [&](const auto& other) {
					return Overlaps(range.baseMipLevel, range.mipLevelCount, other.baseMipLevel, other.mipLevelCount) &&
						Overlaps(
							range.baseArrayLayer, range.arrayLayerCount, other.baseArrayLayer, other.arrayLayerCount);
				});
				if (overlaps) {
					error = context + "overlaps another import; use the same handle or import disjoint ranges.";
					return false;
				}

				ranges.push_back(range);
			}
		}

		return true;
	}

	bool RenderGraph::ValidatePasses(std::string& error) const {
		std::vector<size_t> seen(m_Resources.size(), m_Passes.size());

		for (size_t passIndex = 0; passIndex < m_Passes.size(); ++passIndex) {
			const auto& pass = m_Passes[passIndex];
			const std::string context = "Pass '" + pass.desc.name + "': ";
			if (pass.desc.name.empty() || !pass.record) {
				error = context + "provide a name and recording callback.";
				return false;
			}

			for (const auto& use : pass.desc.resources) {
				if (!IsValid(use.resource)) {
					error = context + "resource handle belongs to another graph, was reset, or is invalid.";
					return false;
				}

				if (seen[use.resource.index] == passIndex) {
					error = context + "declare each resource once; use ReadWrite for an in-place operation.";
					return false;
				}

				seen[use.resource.index] = passIndex;
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
		m_Compiled.prerequisites.assign(count, {});
		std::vector<ResourceUsers> users(m_Resources.size());
		std::vector<std::vector<uint32_t>> additionalDependencies(count);

		for (const auto& dependency : m_Dependencies) {
			additionalDependencies[dependency.pass.index].push_back(dependency.prerequisite.index);
		}

		// A pass pair can share many resources or repeat an explicit dependency.
		// Dense pass indices let us deduplicate without searching adjacency lists.
		std::vector<uint32_t> seen(count, UINT32_MAX);

		for (uint32_t pass = 0; pass < count; ++pass) {
			auto& prerequisites = m_Compiled.prerequisites[pass];
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
			incoming[pass] = static_cast<uint32_t>(m_Compiled.prerequisites[pass].size());
			if (incoming[pass] == 0) {
				ready.push(pass);
			}

			for (uint32_t prerequisite : m_Compiled.prerequisites[pass]) {
				dependents[prerequisite].push_back(pass);
			}
		}

		m_Compiled.order.reserve(count);

		while (!ready.empty()) {
			// A newly ready earlier pass takes precedence over later waiting work.
			const uint32_t next = ready.top();
			ready.pop();
			m_Compiled.order.push_back(next);

			for (uint32_t destination : dependents[next]) {
				if (--incoming[destination] == 0) {
					ready.push(destination);
				}
			}
		}

		if (m_Compiled.order.size() != count) {
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

			for (uint32_t index : m_Compiled.order) {
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
				pending.insert(pending.end(), m_Compiled.prerequisites[index].begin(), m_Compiled.prerequisites[index].end());
			}

			std::erase_if(m_Compiled.order, [&](uint32_t index) {
				return !live[index];
			});
		}

		m_Compiled.usedResources.assign(m_Resources.size(), !m_Options.cullUnusedPasses);

		for (auto index : m_Compiled.order) {
			for (const auto& use : m_Passes[index].desc.resources) {
				m_Compiled.usedResources[use.resource.index] = 1;
			}
		}

		for (size_t i = 0; i < m_Resources.size(); ++i) {
			if (m_Resources[i].hasFinalState) {
				m_Compiled.usedResources[i] = 1;
			}
		}
	}

	void RenderGraph::Transition(uint32_t index,
		ResourceState after,
		TextureLayout layout,
		TrackedState& before,
		PlannedBarriers& barriers) const {
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
					{.resource = index,
						.before = before.state,
						.after = after,
						.oldLayout = before.layout,
						.newLayout = layout,
						.subresources = resource.subresources});
			}
			else {
				barriers.buffers.push_back(
					{.resource = index,
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

		for (uint32_t index : m_Compiled.order) {
			const auto& pass = m_Passes[index];
			PlannedStep step{.name = pass.desc.name, .queue = pass.desc.queue};

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

			m_Compiled.steps.push_back(std::move(step));
		}

		for (uint32_t i = 0; i < m_Resources.size(); ++i) {
			const auto& resource = m_Resources[i];
			if (resource.hasFinalState) {
				if (!states[i].initialized) {
					error = "Cannot export an undefined resource without a producing pass.";
					return false;
				}

				Transition(i, resource.finalState, resource.finalLayout, states[i], m_Compiled.finalBarriers);
			}
		}

		return true;
	}

	bool RenderGraph::Compile(std::string& error) {
		REALRHI_CPU_ZONE("RealRHI: Compile graph");

		error.clear();
		if (m_Compiled.valid) {
			return true;
		}

		Invalidate();
		if (!ValidateResources(error) || !ValidatePasses(error) || !BuildOrder(error)) {
			Invalidate();
			return false;
		}

		CullUnusedPasses();
		if (!BuildBarriers(error)) {
			Invalidate();
			return false;
		}

		m_Compiled.valid = true;

		return true;
	}

}
