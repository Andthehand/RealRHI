#pragma once
#include "BufferDesc.h"
#include "CommandList.h"
#include "DeviceDesc.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace RealRHI {
	class Device;
	class TransientResourcePool;

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
	};

	struct RenderGraphOptions {
		bool cullUnusedPasses = false;
		// Best effort paired pass timings, including per-pass barriers. Requires device
		// profiling; unsupported/full query pools skip timing without skipping work.
		// DeviceStatistics reports these intervals as "RenderGraph: <pass name>".
		bool enableGpuTimings = false;
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
	};

	struct RenderGraphPassInfo {
		RenderGraphPass pass;
		std::string name;
		QueueType queue = QueueType::Graphics;
		// UINT32_MAX identifies a culled pass; other indices address GetExecutionPlan().
		uint32_t executionIndex = UINT32_MAX;
		std::vector<RenderGraphPass> prerequisites;
		std::optional<QueueId> nativeQueue;
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
	};

	struct RenderGraphInitialBarriers {
		QueueType queue;
		BarrierDesc barriers;
		std::vector<RenderGraphPass> consumers;
	};

	struct RenderGraphDiagnostics {
		bool compiled = false;
		bool multiQueue = false;
		std::vector<RenderGraphPassInfo> passes;
		std::vector<RenderGraphResourceInfo> resources;
		std::vector<RenderGraphInitialBarriers> initialBarriers;
	};

	struct RenderGraphSubmission {
		// Includes every successful submission, even if Submit fails partway through.
		std::vector<SubmissionToken> completions;
		bool IsComplete(Device& device) const;
		void Wait(Device& device) const;
	};

	// Serialize building/execution; callbacks must not mutate the graph. Resource
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
		// Borrowed until the next Execute/Reset/destruction; retain a Ref to keep an
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
		void SetFinalState(RenderGraphResource resource,
			ResourceState state,
			TextureLayout layout = TextureLayout::Undefined);

		// Callbacks record complete passes, beginning/ending their own rendering scopes.
		// Imported ranges must enter and leave the callback in the declared state/layout.
		// Declare every graph-managed resource here. Callbacks may also access resources
		// whose states/dependencies are managed externally. UseResource belongs in each
		// callback: Execute clears prior declarations at pass boundaries and on exit,
		// retaining their lifetimes through CommandList reset as usual.
		RenderGraphPass AddPass(RenderGraphPassDesc desc, std::function<void(CommandList&)> record);
		void AddDependency(RenderGraphPass pass, RenderGraphPass prerequisite);

		// Compile validates the entire graph without recording anything. Plans remain
		// valid until the graph changes; rebuild if external resource states change.
		// Created-resource barrier pointers resolve during Execute. Compile allocates nothing.
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
		const std::vector<RenderGraphStep>& GetExecutionPlan() const { return m_Plan; }
		const BarrierDesc& GetFinalBarriers() const { return m_FinalBarriers; }
		// Compile, allocate resources, and record into an active list outside rendering.
		// An optional pool reuses compatible idle allocations; contents are always
		// discarded and require initialization. Debug names do not affect pool identity;
		// the first allocation names the shared resource. The pool belongs to device.
		// Validation/allocation failures return before recording any passes.
		Result Execute(Device& device, CommandList& commands, TransientResourcePool* pool = nullptr);
		// Record all lists before submitting any work. External waits apply to all
		// submissions; matching release/acquire pairs handle exclusive ownership.
		// Exports remain on their last-use queue. Resources crossing queues allocate
		// directly; the pool is used only for resources confined to one native queue.
		// outSubmission is replaced. On failure its tokens describe the queued prefix;
		// imported resources may be partway through a transfer and need state/ownership
		// recovery before reuse. Validation/allocation failures submit no work.
		Result Submit(Device& device,
			RenderGraphSubmission& outSubmission,
			TransientResourcePool* pool = nullptr,
			std::span<const SubmissionToken> waitFor = {});
		void Reset();

	private:
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
			std::function<void(CommandList&)> record;
		};
		struct Dependency {
			RenderGraphPass pass;
			RenderGraphPass prerequisite;
		};
		struct InitialRelease {
			QueueType queue;
			BarrierDesc barriers;
			std::vector<uint32_t> consumers;
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
		Result AllocateResources(Device& device,
			std::span<const QueueId> queues,
			TransientResourcePool* pool,
			std::span<const uint8_t> poolingAllowed = {});
		bool IsValid(RenderGraphResource resource) const;
		bool IsValid(RenderGraphPass pass) const;
		bool ValidateResources(std::string& error) const;
		bool ValidatePasses(std::string& error) const;
		bool BuildOrder(std::string& error);
		void CullUnusedPasses();
		void RecordPass(CommandList& commands, size_t position);
		bool BuildBarriers(std::string& error);
		bool ValidateQueues(Device& device, std::string& error) const;
		void BuildQueuePlan(Device& device, std::vector<InitialRelease>& initialReleases);
		void Transition(uint32_t resource,
			ResourceState after,
			TextureLayout layout,
			TrackedState& before,
			BarrierDesc& barriers);
		void Invalidate();
		void Report(const std::string& error) const;

	private:
		DebugCallback m_Callback;
		RenderGraphOptions m_Options;
		bool m_Compiled = false;
		bool m_MultiQueue = false;
		std::vector<uint8_t> m_UsedResources;
		std::vector<InitialRelease> m_InitialReleases;
		uint64_t m_Id = 0;
		std::vector<Resource> m_Resources;
		std::vector<Pass> m_Passes;
		std::vector<Dependency> m_Dependencies;
		std::vector<uint32_t> m_Order;
		std::vector<std::vector<uint32_t>> m_Prerequisites;
		std::vector<RenderGraphStep> m_Plan;
		BarrierDesc m_FinalBarriers;
	};
}
