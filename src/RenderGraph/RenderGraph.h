#pragma once

#include "BufferDesc.h"
#include "CommandList.h"
#include "DeviceDesc.h"
#include "Swapchain.h"

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace RealRHI {
	class Device;
	class TransientResourcePool;
	class RenderGraph;

	struct RenderGraphResource {
		uint32_t index = UINT32_MAX;
		uint64_t graph = 0;
	};

	struct RenderGraphPass {
		uint32_t index = UINT32_MAX;
		uint64_t graph = 0;
	};

	enum class RenderGraphAccess {
		Read,
		Write,
		ReadWrite
	};

	struct RenderGraphUse {
		RenderGraphResource resource;

		ResourceState state;
		TextureLayout layout = TextureLayout::Undefined;
		RenderGraphAccess access = RenderGraphAccess::Read;

		static RenderGraphUse SampledRead(RenderGraphResource resource,
			PipelineStage stages = PipelineStage::FragmentShader) {
			return {resource, {stages, ResourceAccess::ShaderRead}, TextureLayout::ShaderRead};
		}

		static RenderGraphUse ColorAttachmentWrite(RenderGraphResource resource) {
			return {resource, {PipelineStage::ColorAttachmentOutput, ResourceAccess::ColorAttachmentWrite},
				TextureLayout::ColorAttachment, RenderGraphAccess::Write};
		}
	};

	// Borrowed for one callback. Resolution is restricted to that pass's declarations.
	// UseResource is still required for bindless operations.
	class RenderGraphContext {
	public:
		CommandList& GetCommands() const { return m_Commands; }

		Texture* GetTexture(RenderGraphResource resource) const;
		Buffer* GetBuffer(RenderGraphResource resource) const;

	private:
		friend class RenderGraph;

		RenderGraphContext(RenderGraph& graph, CommandList& commands, uint32_t pass)
			: m_Graph(graph), m_Commands(commands), m_Pass(pass) { }

		bool Declares(RenderGraphResource resource) const;

		RenderGraph& m_Graph;
		CommandList& m_Commands;
		uint32_t m_Pass;
	};

	struct RenderGraphOptions {
		bool cullUnusedPasses = false;

		// Reuse an identical physical resource for disjoint logical lifetimes on one
		// native queue. Exports, imports, host-visible buffers, and cross-queue resources
		// stay distinct. A retained Ref preserves allocation lifetime, not old contents:
		// use SetFinalState to export resources that must survive their last declared use.
		bool enableAliasing = false;
	};

	struct RenderGraphPassDesc {
		std::string name;

		std::vector<RenderGraphUse> resources;
		std::vector<RenderGraphPass> dependsOn;

		// Submit honors this preference; Execute uses its caller-owned list.
		QueueType queue = QueueType::Graphics;

		// Required when culling is enabled and the callback affects unmanaged resources
		// or performs other externally observable work. Writes to imports are roots too.
		bool hasSideEffects = false;
	};

	struct RenderGraphStep {
		std::string name;

		BarrierDesc barriers;
		QueueType queue = QueueType::Graphics;
		BarrierDesc afterBarriers;
		std::optional<QueueId> nativeQueue;

		// Submit batch index; UINT32_MAX until batch planning (or when culled).
		uint32_t batchIndex = UINT32_MAX;
	};

	struct RenderGraphPassInfo {
		RenderGraphPass pass;
		std::string name;

		QueueType queue = QueueType::Graphics;

		// UINT32_MAX identifies a culled pass; other indices address GetExecutionPlan().
		uint32_t executionIndex = UINT32_MAX;
		std::vector<RenderGraphPass> prerequisites;
		std::optional<QueueId> nativeQueue;

		// Submit batch index; UINT32_MAX until batch planning (or when culled).
		uint32_t batchIndex = UINT32_MAX;
	};

	struct RenderGraphResourceInfo {
		RenderGraphResource resource;
		std::string name;

		bool texture = false;
		bool imported = false;
		bool exported = false;

		// Inclusive schedule positions, UINT32_MAX for unused resources. These describe
		// CPU scheduling order, not GPU time overlap or permission to alias allocations.
		uint32_t firstUse = UINT32_MAX;
		uint32_t lastUse = UINT32_MAX;

		std::optional<RenderGraphResource> aliasOf;
		uint64_t allocationBytes = 0;
	};

	struct RenderGraphInitialBarriers {
		QueueType queue;
		BarrierDesc barriers;
		std::vector<RenderGraphPass> consumers;
	};

	struct RenderGraphBatchInfo {
		QueueType queue = QueueType::Graphics;
		QueueId nativeQueue;

		// Indices into GetExecutionPlan(), in recording order.
		std::vector<uint32_t> steps;

		// Earlier batch indices; same-queue ordering needs no semaphore wait.
		std::vector<uint32_t> prerequisites;

		// Prefix batches contain initial barriers instead of graph passes.
		std::optional<uint32_t> initialRelease;
		bool waitForAcquire = false;
		bool presentation = false;
	};

	struct RenderGraphDiagnostics {
		bool compiled = false;
		bool multiQueue = false;

		std::vector<RenderGraphPassInfo> passes;
		std::vector<RenderGraphResourceInfo> resources;
		std::vector<RenderGraphInitialBarriers> initialBarriers;
		std::vector<RenderGraphBatchInfo> batches;

		uint64_t transientBytes = 0;
		uint64_t aliasSavedBytes = 0;

		BarrierDesc presentationBarriers;
	};

	struct RenderGraphSubmission {
		// One token per successful batch, in diagnostic batch order, including the
		// queued prefix when Submit fails. A batch may contain several passes.
		std::vector<SubmissionToken> completions;

		bool IsComplete(Device& device) const;
		void Wait(Device& device) const;
	};

	// Serialize graph access, including diagnostic inspection; callbacks must not mutate it. Resource
	// versions follow insertion order; explicit dependencies reorder independent passes.
	// Execute records into one caller-owned list. Submit honors each pass's queue,
	// creates its own lists, and schedules GPU dependencies without CPU waits.
	class RenderGraph {
	public:
		explicit RenderGraph(DebugCallback callback = DefaultDebugCallback);
		RenderGraph(const RenderGraph&) = delete;
		RenderGraph& operator=(const RenderGraph&) = delete;

		// Deferred, uninitialized resources covering the complete buffer/image. Names
		// are copied; initialize data in a Write pass rather than BufferDesc::initialData.
		RenderGraphResource CreateTexture(const TextureDesc& desc);
		RenderGraphResource CreateBuffer(const BufferDesc& desc);

		// Borrowed until the next Execute/Submit/Reset/destruction; retain a Ref to keep an
		// output externally. Created resources resolve inside callbacks and after Execute.
		Texture* GetTexture(RenderGraphResource resource);
		Buffer* GetBuffer(RenderGraphResource resource);

		// Import non-overlapping ranges with their actual state at graph entry.
		// Undefined textures need a Write pass before reading. Imported buffer contents
		// are supplied by the caller. A zero buffer size selects the remaining bytes.
		// Texture ranges must fit the underlying image; the RHI validates their extents.
		// For Submit, owner identifies an external resource's current queue role. If
		// omitted, it must be unclaimed or already owned by its first use's family.
		// Execute requires ownership already acquired onto the caller's queue.
		RenderGraphResource ImportTexture(Texture* texture,
			TextureLayout layout,
			ResourceState state = {},
			TextureSubresourceRange subresources = {},
			std::optional<QueueType> owner = {});
		RenderGraphResource ImportBuffer(Buffer* buffer,
			ResourceState state = {},
			uint64_t offset = 0,
			uint64_t size = 0,
			std::optional<QueueType> owner = {});

		// One acquired frame per graph. Contents are discarded; first use must write.
		// Submit handles acquire synchronization, final ownership/layout, and Present.
		RenderGraphResource ImportBackBuffer(Swapchain& swapchain, const FrameContext& frame);
		void SetFinalState(RenderGraphResource resource,
			ResourceState state,
			TextureLayout layout = TextureLayout::Undefined);

		// Callbacks record complete passes, beginning/ending their own rendering scopes.
		// Imported ranges must enter and leave the callback in the declared state/layout.
		// Resolve declared resources and access the command list through the context.
		// Declare every graph-managed resource here. Callbacks may also access resources
		// whose states/dependencies are managed externally. UseResource belongs in each
		// callback: Execute clears prior declarations at pass boundaries and on exit,
		// retaining their lifetimes through CommandList reset as usual.
		RenderGraphPass AddPass(RenderGraphPassDesc desc, std::function<void(RenderGraphContext&)> record);
		void AddDependency(RenderGraphPass pass, RenderGraphPass prerequisite);

		// Compile validates the entire graph without recording anything. Plans remain
		// valid until the graph changes; rebuild if external resource states change.
		// Logical compilation is cached until mutation. Compile creates no GPU resources
		// and leaves an existing execution snapshot intact when the graph is unchanged.
		// Diagnostic snapshots are materialized on demand by the inspection methods.
		// Created-resource barrier pointers resolve during Execute/Submit.
		bool Compile(std::string& error);

		// Reset preserves options. Culling keeps exports, writes to imported resources,
		// side-effect passes, and all their prerequisites. Other callbacks may be skipped.
		void SetOptions(RenderGraphOptions options);

		// Snapshots describe the latest compiled/recording plan, including a failed submission. Submit adds
		// actual queue dependencies and prefix ownership releases. Barrier pointers are
		// borrowed from the graph; the plan and final barriers expose complete states.
		RenderGraphDiagnostics GetDiagnostics() const;
		std::string GetDebugDescription() const;
		std::string GetDependencyGraphDot() const;
		const std::vector<RenderGraphStep>& GetExecutionPlan() const {
			EnsureSnapshot();
			return m_Execution.steps;
		}

		const BarrierDesc& GetFinalBarriers() const {
			EnsureSnapshot();
			return m_Execution.finalBarriers;
		}

		// Compile, allocate resources, and record into an active list outside rendering.
		// An optional pool reuses compatible idle allocations; contents are always
		// discarded and require initialization. Debug names do not affect pool identity;
		// the first allocation names the shared resource. The pool belongs to device.
		// Validation/allocation failures return before recording any passes.
		Result Execute(Device& device,
			CommandList& commands,
			TransientResourcePool* pool = nullptr);

		// Batch consecutive passes sharing a native queue when no new cross-queue wait
		// or earlier cross-queue signal is needed. Record all lists before submitting any work.
		// External waits apply to all submissions; matching release/acquire pairs handle exclusive ownership.
		// Exports remain on their last-use queue. Resources crossing queues allocate
		// directly; the pool is used only for resources confined to one native queue.
		// outSubmission is replaced. On failure its tokens describe the queued prefix;
		// imported resources may be partway through a transfer and need state/ownership
		// recovery before reuse. Validation/allocation failures submit no work.
		// An imported frame is presented automatically; the last completion token joins
		// the entire graph. A partially submitted frame needs swapchain recreation before reuse.
		Result Submit(Device& device,
			RenderGraphSubmission& outSubmission,
			TransientResourcePool* pool = nullptr,
			std::span<const SubmissionToken> waitFor = {});
		void Reset();

	private:
		friend class RenderGraphContext;
		struct Resource {
			std::optional<TextureDesc> textureDesc;
			std::optional<BufferDesc> bufferDesc;
			std::string debugName;

			Ref<Texture> texture;
			Ref<Buffer> buffer;

			TextureSubresourceRange subresources;

			uint64_t offset = 0;
			uint64_t size = 0;

			ResourceState initialState;
			TextureLayout initialLayout = TextureLayout::Undefined;
			std::optional<QueueType> initialQueue;

			bool hasFinalState = false;
			ResourceState finalState;
			TextureLayout finalLayout = TextureLayout::Undefined;
		};

		struct Pass {
			RenderGraphPassDesc desc;
			std::function<void(RenderGraphContext&)> record;
		};

		struct Dependency {
			RenderGraphPass pass;
			RenderGraphPass prerequisite;
		};

		// Planned barriers carry logical identities; native pointers are bound only
		// in the execution snapshot.
		struct PlannedTextureBarrier {
			uint32_t resource = UINT32_MAX;

			ResourceState before;
			ResourceState after;

			TextureLayout oldLayout = TextureLayout::Undefined;
			TextureLayout newLayout = TextureLayout::Undefined;
			TextureSubresourceRange subresources;

			std::optional<QueueType> sourceQueue;
			std::optional<QueueType> destinationQueue;

			bool acquireFromPresent = false;
			bool releaseToPresent = false;
		};

		struct PlannedBufferBarrier {
			uint32_t resource = UINT32_MAX;

			ResourceState before;
			ResourceState after;

			uint64_t offset = 0;
			uint64_t size = 0;

			std::optional<QueueType> sourceQueue;
			std::optional<QueueType> destinationQueue;
		};

		struct PlannedBarriers {
			std::vector<PlannedTextureBarrier> textures;
			std::vector<PlannedBufferBarrier> buffers;
		};

		struct PlannedStep {
			std::string name;

			PlannedBarriers barriers;
			QueueType queue = QueueType::Graphics;
			PlannedBarriers afterBarriers;
			std::optional<QueueId> nativeQueue;
			uint32_t batchIndex = UINT32_MAX;
		};

		struct InitialRelease {
			QueueType queue;
			PlannedBarriers barriers;
			std::vector<uint32_t> consumers;
		};

		struct CompiledGraph {
			bool valid = false;

			std::vector<uint8_t> usedResources;
			std::vector<uint32_t> order;
			std::vector<std::vector<uint32_t>> prerequisites;

			std::vector<PlannedStep> steps;
			PlannedBarriers finalBarriers;
		};

		struct SubmissionPlan {
			bool multiQueue = false;

			std::vector<std::vector<uint32_t>> prerequisites;

			std::vector<InitialRelease> initialReleases;
			std::vector<RenderGraphBatchInfo> batches;
		};

		struct QueuePlan {
			SubmissionPlan submission;

			std::vector<PlannedStep> steps;
			PlannedBarriers finalBarriers;
			PlannedBarriers presentationBarriers;
		};

		struct PhysicalResource {
			Ref<Texture> texture;
			Ref<Buffer> buffer;

			bool reused = false;
		};

		struct Execution {
			std::vector<PhysicalResource> resources;
			std::vector<uint32_t> aliasSources;

			SubmissionPlan plan;

			// Diagnostic views are materialized on demand after logical compilation,
			// or once after physical resources and queues are prepared for recording.
			mutable bool hasSnapshot = false;
			mutable std::vector<RenderGraphStep> steps;
			mutable BarrierDesc finalBarriers;
			mutable BarrierDesc presentationBarriers;
		};

		struct TrackedState {
			ResourceState state;
			TextureLayout layout = TextureLayout::Undefined;

			bool used = false;
			bool initialized = false;

			std::vector<ResourceState> visibleReads;
		};

	private:
		static bool IsTexture(const Resource& resource);
		static bool IsCreated(const Resource& resource);

		void ReleaseCreatedResources();

		Texture* ResolveTexture(uint32_t resource) const;
		Buffer* ResolveBuffer(uint32_t resource) const;

		BarrierDesc ResolveBarriers(const PlannedBarriers& barriers) const;
		void EnsureSnapshot() const;
		void ResolvePlan(std::span<const PlannedStep> steps,
			const PlannedBarriers& finalBarriers,
			const PlannedBarriers& presentationBarriers = {}) const;
		void BindQueuePlan(QueuePlan plan);
		void PrepareSingleQueuePlan(QueueId queue);

		void BuildAliasPlan(std::span<const QueueId> queues, std::span<const uint8_t> poolingAllowed);
		Result AllocateResources(Device& device,
			std::span<const QueueId> queues,
			TransientResourcePool* pool,
			std::span<const uint8_t> poolingAllowed = {});

		bool IsValid(RenderGraphResource resource) const;
		bool IsValid(RenderGraphPass pass) const;

		bool ValidateResources(std::string& error) const;
		bool ValidatePasses(std::string& error) const;

		void BuildDependencies();
		bool BuildOrder(std::string& error);
		void CullUnusedPasses();

		void RecordPass(CommandList& commands, size_t position);
		bool BuildBarriers(std::string& error);
		bool ValidateQueues(Device& device, std::string& error) const;

		// Queue planning consumes resolved identities, without allocation or submission.
		QueuePlan BuildQueuePlan(const std::array<QueueId, 3>& queues) const;
		void BuildBatches(QueuePlan& plan, const std::array<QueueId, 3>& queues) const;

		void Transition(uint32_t resource,
			ResourceState after,
			TextureLayout layout,
			TrackedState& before,
			PlannedBarriers& barriers) const;

		void Invalidate();
		void Report(const std::string& error) const;

	private:
		DebugCallback m_Callback;

		Ref<Swapchain> m_FrameSwapchain;
		FrameContext m_Frame;
		uint32_t m_BackBuffer = UINT32_MAX;

		RenderGraphOptions m_Options;
		uint64_t m_Id = 0;

		std::vector<Resource> m_Resources;
		std::vector<Pass> m_Passes;
		std::vector<Dependency> m_Dependencies;

		CompiledGraph m_Compiled;
		Execution m_Execution;
	};
}
