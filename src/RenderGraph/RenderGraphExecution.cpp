#include "RenderGraph.h"

#include "Device.h"
#include "Detail/CpuProfiling.h"
#include "TransientResourcePool.h"

#include <algorithm>
#include <numeric>

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

	}

	void RenderGraph::ReleaseCreatedResources() {
		m_Execution = {};
	}

	void RenderGraph::BuildAliasPlan(std::span<const QueueId> queues, std::span<const uint8_t> poolingAllowed) {
		m_Execution.aliasSources.resize(m_Resources.size());
		std::iota(m_Execution.aliasSources.begin(), m_Execution.aliasSources.end(), 0u);
		if (!m_Options.enableAliasing) {
			return;
		}

		std::vector<uint32_t> firstUse(m_Resources.size(), UINT32_MAX);
		std::vector<uint32_t> lastUse(m_Resources.size(), UINT32_MAX);

		for (uint32_t position = 0; position < m_Compiled.order.size(); ++position) {
			for (const auto& use : m_Passes[m_Compiled.order[position]].desc.resources) {
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
					m_Execution.aliasSources[index] = m_Execution.aliasSources[occupant];
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
		ReleaseCreatedResources();
		REALRHI_CPU_ZONE("RealRHI: Allocate graph resources");

		BuildAliasPlan(queues, poolingAllowed);

		// Commit only after the entire allocation batch succeeds. In-flight executions
		// retain their old resources through CommandList, independently of this graph.
		std::vector<Ref<Texture>> textures(m_Resources.size());
		std::vector<Ref<Buffer>> buffers(m_Resources.size());

		for (size_t i = 0; i < m_Resources.size(); ++i) {
			const auto& resource = m_Resources[i];
			if (!m_Compiled.usedResources[i] || m_Execution.aliasSources[i] != i) {
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
			if (m_Execution.aliasSources[i] != i) {
				textures[i] = textures[m_Execution.aliasSources[i]];
				buffers[i] = buffers[m_Execution.aliasSources[i]];
			}
		}

		m_Execution.resources.resize(m_Resources.size());

		for (size_t i = 0; i < m_Resources.size(); ++i) {
			if (IsCreated(m_Resources[i]) && m_Compiled.usedResources[i]) {
				auto& physical = m_Execution.resources[i];
				physical.texture = std::move(textures[i]);
				physical.buffer = std::move(buffers[i]);
				physical.reused = m_Execution.aliasSources[i] != i ||
					(pool && (poolingAllowed.empty() || poolingAllowed[i]));
			}
		}

		return Result::Success;
	}

	// Graph constness protects declarations and bindings, not the referenced RHI
	// objects. Native barrier descriptions require mutable borrowed pointers.
	Texture* RenderGraph::ResolveTexture(uint32_t index) const {
		if (IsCreated(m_Resources[index])) {
			return index < m_Execution.resources.size() ?
				const_cast<Texture*>(m_Execution.resources[index].texture.Raw()) : nullptr;
		}

		return const_cast<Texture*>(m_Resources[index].texture.Raw());
	}

	Buffer* RenderGraph::ResolveBuffer(uint32_t index) const {
		if (IsCreated(m_Resources[index])) {
			return index < m_Execution.resources.size() ?
				const_cast<Buffer*>(m_Execution.resources[index].buffer.Raw()) : nullptr;
		}

		return const_cast<Buffer*>(m_Resources[index].buffer.Raw());
	}

	BarrierDesc RenderGraph::ResolveBarriers(const PlannedBarriers& barriers) const {
		BarrierDesc resolved;
		resolved.textures.reserve(barriers.textures.size());
		resolved.buffers.reserve(barriers.buffers.size());

		for (const auto& barrier : barriers.textures) {
			resolved.textures.push_back({.texture = ResolveTexture(barrier.resource),
				.before = barrier.before,
				.after = barrier.after,
				.oldLayout = barrier.oldLayout,
				.newLayout = barrier.newLayout,
				.subresources = barrier.subresources,
				.sourceQueue = barrier.sourceQueue,
				.destinationQueue = barrier.destinationQueue,
				.acquireFromPresent = barrier.acquireFromPresent,
				.releaseToPresent = barrier.releaseToPresent});
		}

		for (const auto& barrier : barriers.buffers) {
			resolved.buffers.push_back({.buffer = ResolveBuffer(barrier.resource),
				.before = barrier.before,
				.after = barrier.after,
				.offset = barrier.offset,
				.size = barrier.size,
				.sourceQueue = barrier.sourceQueue,
				.destinationQueue = barrier.destinationQueue});
		}

		return resolved;
	}

	void RenderGraph::EnsureSnapshot() const {
		if (m_Compiled.valid && !m_Execution.hasSnapshot) {
			ResolvePlan(m_Compiled.steps, m_Compiled.finalBarriers);
		}
	}

	void RenderGraph::ResolvePlan(std::span<const PlannedStep> steps,
		const PlannedBarriers& finalBarriers,
		const PlannedBarriers& presentationBarriers) const {
		m_Execution.steps.clear();
		m_Execution.steps.reserve(steps.size());

		for (const auto& step : steps) {
			m_Execution.steps.push_back({.name = step.name,
				.barriers = ResolveBarriers(step.barriers),
				.queue = step.queue,
				.afterBarriers = ResolveBarriers(step.afterBarriers),
				.nativeQueue = step.nativeQueue,
				.batchIndex = step.batchIndex});
		}

		m_Execution.finalBarriers = ResolveBarriers(finalBarriers);
		m_Execution.presentationBarriers = ResolveBarriers(presentationBarriers);
		m_Execution.hasSnapshot = true;
	}

	void RenderGraph::BindQueuePlan(QueuePlan plan) {
		ResolvePlan(plan.steps, plan.finalBarriers, plan.presentationBarriers);

		// Retain submission metadata; the temporary logical barriers can now die.
		m_Execution.plan = std::move(plan.submission);
	}

	void RenderGraph::PrepareSingleQueuePlan(QueueId queue) {
		ResolvePlan(m_Compiled.steps, m_Compiled.finalBarriers);

		// Created resources must first be written, so only their first barrier needs
		// the allocation's prior accesses. Subsequent states come from that write.
		std::vector<bool> seen(m_Resources.size());
		auto prepare = [&](auto& barriers, const auto& planned) {
			for (size_t i = 0; i < barriers.size(); ++i) {
				const auto resource = planned[i].resource;
				if (!seen[resource] && m_Execution.resources[resource].reused) {
					barriers[i].before = {PipelineStage::AllCommands,
						ResourceAccess::MemoryRead | ResourceAccess::MemoryWrite};
				}

				seen[resource] = true;
			}
		};

		for (size_t i = 0; i < m_Execution.steps.size(); ++i) {
			auto& step = m_Execution.steps[i];
			step.nativeQueue = queue;
			prepare(step.barriers.textures, m_Compiled.steps[i].barriers.textures);
			prepare(step.barriers.buffers, m_Compiled.steps[i].barriers.buffers);
		}
	}

	bool RenderGraphContext::Declares(RenderGraphResource resource) const {
		const auto& uses = m_Graph.m_Passes[m_Pass].desc.resources;
		const bool declared = std::any_of(uses.begin(), uses.end(), [&](const auto& use) {
			return use.resource.index == resource.index && use.resource.graph == resource.graph;
		});
		if (!declared) {
			m_Graph.Report("Pass '" + m_Graph.m_Passes[m_Pass].desc.name +
				"': context resolution requires a resource declared by this pass.");
		}

		return declared;
	}

	Texture* RenderGraphContext::GetTexture(RenderGraphResource resource) const {
		return Declares(resource) ? m_Graph.GetTexture(resource) : nullptr;
	}

	Buffer* RenderGraphContext::GetBuffer(RenderGraphResource resource) const {
		return Declares(resource) ? m_Graph.GetBuffer(resource) : nullptr;
	}

	void RenderGraph::RecordPass(CommandList& commands, size_t position) {
		REALRHI_CPU_ZONE("RealRHI: Record graph pass");
		REALRHI_CPU_ZONE_TEXT(m_Execution.steps[position].name.data(), m_Execution.steps[position].name.size());

		const auto& step = m_Execution.steps[position];
		commands.ClearResourceDeclarations();

		for (const auto& use : m_Passes[m_Compiled.order[position]].desc.resources) {
			auto& resource = m_Resources[use.resource.index];
			if (IsTexture(resource)) {
				commands.RetainResource(ResolveTexture(use.resource.index));
			}
			else {
				commands.RetainResource(ResolveBuffer(use.resource.index));
			}
		}

		commands.Barrier(step.barriers);

		commands.BeginDebugLabel(step.name.c_str());
		RenderGraphContext context(*this, commands, m_Compiled.order[position]);
		m_Passes[m_Compiled.order[position]].record(context);
		commands.EndDebugLabel();

		commands.ClearResourceDeclarations();
		commands.Barrier(step.afterBarriers);
	}

	Result RenderGraph::Execute(Device& device,
		CommandList& commands,
		TransientResourcePool* pool) {
		REALRHI_CPU_ZONE("RealRHI: Execute graph");

		if (m_FrameSwapchain) {
			Report("Graphs importing a backbuffer require Submit to synchronize and present the acquired image.");
			return Result::InvalidParameter;
		}

		if (commands.GetState() != CommandListState::Recording) {
			Report("Execute requires an active command list recording outside rendering.");
			return Result::InvalidParameter;
		}

		std::string error;
		if (!Compile(error)) {
			Report(error);
			return Result::InvalidParameter;
		}

		const std::vector<QueueId> queues(m_Resources.size(), commands.GetQueueId());
		const Result result = AllocateResources(device, queues, pool);
		if (result != Result::Success) {
			return result;
		}

		PrepareSingleQueuePlan(commands.GetQueueId());

		for (size_t i = 0; i < m_Execution.steps.size(); ++i) {
			RecordPass(commands, i);
		}

		commands.ClearResourceDeclarations();
		commands.Barrier(m_Execution.finalBarriers);

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

	Result RenderGraph::Submit(Device& device,
		RenderGraphSubmission& outSubmission,
		TransientResourcePool* pool,
		std::span<const SubmissionToken> waitFor) {
		REALRHI_CPU_ZONE("RealRHI: Submit graph");

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

		for (auto index : m_Compiled.order) {
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

		std::array<QueueId, 3> queues{};

		for (auto role : {QueueType::Graphics, QueueType::Compute, QueueType::Transfer}) {
			if (auto* queue = device.GetCommandQueue(role)) {
				queues[static_cast<size_t>(role)] = queue->GetQueueId();
			}
		}

		BindQueuePlan(BuildQueuePlan(queues));

		const auto& initialReleases = m_Execution.plan.initialReleases;
		std::vector<Ref<CommandList>> commands(m_Execution.plan.batches.size());

		// Create and begin every list before invoking callbacks, then finish all recording
		// before submitting any work. Queue submission retains successful lists in flight.
		for (size_t i = 0; i < m_Execution.plan.batches.size(); ++i) {
			Ref<CommandAllocator> allocator;
			auto result = device.CreateCommandAllocator(m_Execution.plan.batches[i].queue, allocator);
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

		for (size_t i = 0; i < m_Execution.plan.batches.size(); ++i) {
			const auto& batch = m_Execution.plan.batches[i];
			if (batch.initialRelease) {
				commands[i]->Barrier(ResolveBarriers(initialReleases[*batch.initialRelease].barriers));
			}

			for (auto position : batch.steps) {
				RecordPass(*commands[i], position);
			}

			if (batch.presentation) {
				// Ownership acquisition and the following presentation transition touch
				// the same image and must execute as separate dependencies.
				for (const auto& barrier : m_Execution.presentationBarriers.textures) {
					commands[i]->Barrier({.textures = {barrier}});
				}
			}

			const auto ended = commands[i]->End();
			if (ended != Result::Success) {
				return ended;
			}
		}

		for (size_t i = 0; i < m_Execution.plan.batches.size(); ++i) {
			const auto& batch = m_Execution.plan.batches[i];
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
