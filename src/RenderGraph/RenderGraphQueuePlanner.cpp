#include "RenderGraph.h"

#include "Device.h"

#include <algorithm>

namespace RealRHI {
	namespace {
		template <typename T>
		void AppendBarriers(T& destination, const T& source) {
			destination.buffers.insert(destination.buffers.end(), source.buffers.begin(), source.buffers.end());
			destination.textures.insert(destination.textures.end(), source.textures.begin(), source.textures.end());
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

		for (auto index : m_Compiled.order) {
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

				for (uint32_t index : m_Compiled.order) {
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

	RenderGraph::QueuePlan RenderGraph::BuildQueuePlan(const std::array<QueueId, 3>& queues) const {
		QueuePlan plan;
		plan.submission.multiQueue = true;
		plan.submission.prerequisites = m_Compiled.prerequisites;
		auto& initialReleases = plan.submission.initialReleases;
		auto queueId = [&](QueueType role) { return queues[static_cast<size_t>(role)]; };
		plan.steps.reserve(m_Compiled.order.size());
		std::vector<TrackedState> states;
		std::vector<uint32_t> lastUse(m_Resources.size(), UINT32_MAX);

		for (uint32_t i = 0; i < m_Resources.size(); ++i) {
			const auto& resource = m_Resources[i];
			const auto initial = m_Execution.resources[i].reused ?
				ResourceState{PipelineStage::AllCommands, ResourceAccess::MemoryRead | ResourceAccess::MemoryWrite} :
				resource.initialState;
			states.push_back({initial, resource.initialLayout});
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

		for (uint32_t position = 0; position < m_Compiled.order.size(); ++position) {
			const uint32_t passIndex = m_Compiled.order[position];
			const auto& pass = m_Passes[passIndex].desc;
			plan.steps.push_back({.name = pass.name, .queue = pass.queue});
			auto& step = plan.steps.back();
			const auto destination = queueId(pass.queue);
			step.nativeQueue = destination;

			for (const auto& use : pass.resources) {
				const uint32_t resourceIndex = use.resource.index;
				auto& state = states[resourceIndex];
				const uint32_t previous = lastUse[resourceIndex];
				std::optional<QueueType> sourceRole = m_Resources[resourceIndex].initialQueue;
				if (previous != UINT32_MAX) {
					sourceRole = plan.steps[previous].queue;
				}

				bool transfer = false;
				bool differentQueue = false;
				if (sourceRole) {
					const auto source = queueId(*sourceRole);
					transfer = source.familyIndex != destination.familyIndex;
					differentQueue = source != destination;
					if (previous != UINT32_MAX && source != destination) {
						// Exclusive resources require ordered ownership even for read/read uses.
						auto& dependencies = plan.submission.prerequisites[passIndex];
						const uint32_t producer = m_Compiled.order[previous];
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

				PlannedBarriers barriers;
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
						AppendBarriers(plan.steps[previous].afterBarriers, barriers);
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

			PlannedBarriers barriers;
			if (i == m_BackBuffer) {
				// Join presentation on graphics. A foreign family must release the image
				// and graphics must acquire it before transitioning it for presentation.
				const auto source = plan.steps[lastUse[i]].queue;
				if (queueId(source).familyIndex !=
					queueId(QueueType::Graphics).familyIndex) {
					PlannedBarriers ownership;
					states[i].used = false;
					Transition(i,
						{PipelineStage::AllCommands, ResourceAccess::MemoryRead},
						states[i].layout,
						states[i],
						ownership);

					for (auto& barrier : ownership.textures) {
						barrier.sourceQueue = source;
						barrier.destinationQueue = QueueType::Graphics;
					}

					AppendBarriers(plan.steps[lastUse[i]].afterBarriers, ownership);
					AppendBarriers(plan.presentationBarriers, ownership);
				}

				states[i].used = false;
			}

			Transition(i, resource.finalState, resource.finalLayout, states[i], barriers);
			AppendBarriers(plan.finalBarriers, barriers);
			if (i == m_BackBuffer) {
				for (auto& barrier : barriers.textures) {
					barrier.releaseToPresent = true;
				}

				AppendBarriers(plan.presentationBarriers, barriers);
				continue;
			}

			if (lastUse[i] != UINT32_MAX) {
				AppendBarriers(plan.steps[lastUse[i]].afterBarriers, barriers);
			}
			else {
				AppendBarriers(initialRelease(resource.initialQueue.value_or(QueueType::Graphics)).barriers, barriers);
			}
		}

		BuildBatches(plan, queues);

		return plan;
	}

	void RenderGraph::BuildBatches(QueuePlan& plan, const std::array<QueueId, 3>& queues) const {
		auto queueId = [&](QueueType role) { return queues[static_cast<size_t>(role)]; };
		bool acquired = false;
		QueueType presentationQueue = QueueType::Graphics;
		std::vector<uint32_t> passBatches(m_Passes.size(), UINT32_MAX);
		std::vector<std::vector<uint32_t>> consumers(m_Passes.size());
		std::vector<std::vector<uint32_t>> initialWaits(m_Passes.size());

		for (uint32_t i = 0; i < plan.submission.initialReleases.size(); ++i) {
			const auto& release = plan.submission.initialReleases[i];
			plan.submission.batches.push_back(
				{.queue = release.queue,
					.nativeQueue = queueId(release.queue),
					.initialRelease = i});

			for (auto consumer : release.consumers) {
				initialWaits[consumer].push_back(i);
			}
		}

		for (uint32_t index : m_Compiled.order) {
			const auto queue = queueId(m_Passes[index].desc.queue);

			for (auto prerequisite : plan.submission.prerequisites[index]) {
				if (queueId(m_Passes[prerequisite].desc.queue) != queue) {
					consumers[prerequisite].push_back(index);
				}
			}
		}

		for (uint32_t position = 0; position < m_Compiled.order.size(); ++position) {
			const auto index = m_Compiled.order[position];
			auto& step = plan.steps[position];
			const auto queue = *step.nativeQueue;
			std::vector<uint32_t> dependencies = initialWaits[index];

			for (auto prerequisite : plan.submission.prerequisites[index]) {
				const auto batch = passBatches[prerequisite];
				if (plan.submission.batches[batch].nativeQueue != queue) {
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
			bool merge = false;
			if (!plan.submission.batches.empty()) {
				const auto& previous = plan.submission.batches.back();
				if (!firstBackBufferUse && !previous.steps.empty() && previous.nativeQueue == queue) {
					// Do not move a later pass's new wait ahead of earlier independent work.
					merge = std::includes(previous.prerequisites.begin(),
						previous.prerequisites.end(),
						dependencies.begin(),
						dependencies.end());

					for (auto earlier : previous.steps) {
						for (auto consumer : consumers[m_Compiled.order[earlier]]) {
							const auto& prerequisites = plan.submission.prerequisites[consumer];
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
				plan.submission.batches.push_back(
					{.queue = step.queue, .nativeQueue = queue, .prerequisites = std::move(dependencies)});
			}

			const auto batch = static_cast<uint32_t>(plan.submission.batches.size() - 1);
			plan.submission.batches.back().steps.push_back(position);
			passBatches[index] = batch;
			step.batchIndex = batch;
			if (firstBackBufferUse) {
				plan.submission.batches.back().waitForAcquire = true;
				acquired = true;
			}
		}

		if (m_FrameSwapchain) {
			RenderGraphBatchInfo present{
				.queue = presentationQueue,
				.nativeQueue = queueId(presentationQueue),
				.presentation = true
			};

			for (uint32_t i = 0; i < plan.submission.batches.size(); ++i) {
				if (plan.submission.batches[i].nativeQueue != present.nativeQueue) {
					present.prerequisites.push_back(i);
				}
			}

			plan.submission.batches.push_back(std::move(present));
		}
	}

}
