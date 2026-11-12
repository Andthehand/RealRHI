#include "RenderGraph/RenderGraph.h"
#include "RenderGraphBenchmarkDevice.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
	using namespace RealRHI;
	using namespace RealRHI::Benchmarks;
	using Clock = std::chrono::steady_clock;

	constexpr uint32_t s_ImportedResources = 32;
	constexpr ResourceState s_Read{PipelineStage::AllCommands, ResourceAccess::MemoryRead};
	constexpr ResourceState s_Write{PipelineStage::AllCommands, ResourceAccess::MemoryWrite};

	struct Options {
		uint32_t passes = 0;
		uint32_t iterations = 100;
		uint32_t samples = 7;
	};

	bool ParseOptions(int argc, char** argv, Options& options) {
		for (int i = 1; i < argc; ++i) {
			const std::string_view name(argv[i]);
			if (++i == argc) {
				return false;
			}

			const std::string_view value(argv[i]);
			uint32_t number = 0;
			const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
			if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || number == 0) {
				return false;
			}

			if (name == "--passes" && number <= 4096) {
				options.passes = number;
			}
			else if (name == "--iterations" && number <= 100000) {
				options.iterations = number;
			}
			else if (name == "--samples" && number <= 31) {
				options.samples = number;
			}
			else {
				return false;
			}
		}

		return true;
	}

	struct Inputs {
		std::vector<Ref<BenchmarkBuffer>> buffers;
		std::vector<Ref<BenchmarkTexture>> textures;

		Inputs() {
			for (uint32_t i = 0; i < s_ImportedResources / 2; ++i) {
				buffers.push_back(Ref<BenchmarkBuffer>::Create(64));
				textures.push_back(Ref<BenchmarkTexture>::Create());
			}
		}
	};

	enum class Operation {
		Compile,
		Inspect,
		Diagnostics,
		Execute,
		Submit
	};

	struct Workload {
		std::string_view name;
		Operation operation;
		bool reuseGraph = false;
		bool aliasing = false;
	};

	constexpr Workload s_Workloads[] = {
		{"build_compile", Operation::Compile},
		{"build_compile_inspect", Operation::Inspect},
		{"build_execute", Operation::Execute},
		{"build_submit", Operation::Submit},
		{"cached_compile", Operation::Compile, true},
		{"diagnostics", Operation::Diagnostics, true},
		{"repeat_execute", Operation::Execute, true},
		{"repeat_submit", Operation::Submit, true},
		{"repeat_execute_alias", Operation::Execute, true, true},
		{"repeat_submit_alias", Operation::Submit, true, true},
	};

	void CompileGraph(RenderGraph& graph, std::string& error) {
		if (!graph.Compile(error)) {
			throw std::runtime_error(error);
		}
	}

	void BuildGraph(RenderGraph& graph, Inputs& inputs, uint32_t passes, Workload workload, uint64_t& callbacks) {
		graph.SetOptions({.enableAliasing = workload.aliasing});

		std::vector<RenderGraphResource> resources;
		resources.reserve(s_ImportedResources);

		for (auto& buffer : inputs.buffers) {
			resources.push_back(graph.ImportBuffer(buffer.Raw(), s_Read));
		}

		for (auto& texture : inputs.textures) {
			resources.push_back(graph.ImportTexture(texture.Raw(), TextureLayout::General, s_Read));
		}

		for (uint32_t i = 0; i < passes; ++i) {
			const auto read = i % s_ImportedResources;
			const auto write = (i + 1) % s_ImportedResources;
			auto layout = [](uint32_t index) {
				return index < s_ImportedResources / 2 ? TextureLayout::Undefined : TextureLayout::General;
			};

			RenderGraphPassDesc desc{
				.name = "Pass " + std::to_string(i),
				.resources = {
					{resources[read], s_Read, layout(read)},
					{resources[write], s_Write, layout(write), RenderGraphAccess::Write}
				}
			};

			// Submit exercises three distinct native queue identities. Alias workloads
			// stay on graphics: schedule positions alone cannot prove cross-queue reuse.
			if (workload.operation == Operation::Submit && !workload.aliasing) {
				desc.queue = static_cast<QueueType>(i % 3);
			}

			if (workload.aliasing) {
				const auto transient = graph.CreateBuffer({.size = 64, .usage = BufferUsage::Storage});
				desc.resources.push_back({transient, s_Write, TextureLayout::Undefined, RenderGraphAccess::Write});
			}

			graph.AddPass(std::move(desc), [&callbacks](RenderGraphContext&) {
				++callbacks;
			});
		}
	}

	void Measure(BenchmarkDevice& device, Inputs& inputs, const Options& options, uint32_t passes, Workload workload) {
		RenderGraph retained;
		BenchmarkCommands commands;
		RenderGraphSubmission completion;
		uint64_t callbacks = 0;
		std::string error;

		if (workload.reuseGraph) {
			BuildGraph(retained, inputs, passes, workload, callbacks);
			CompileGraph(retained, error);
		}

		auto operate = [&](RenderGraph& graph) {
			switch (workload.operation) {
				case Operation::Compile:
					CompileGraph(graph, error);
					break;
				case Operation::Inspect:
					CompileGraph(graph, error);
					Check(graph.GetExecutionPlan().size() == passes, "Incomplete execution plan");
					break;
				case Operation::Diagnostics: {
					const auto diagnostics = graph.GetDiagnostics();
					Check(diagnostics.passes.size() == passes, "Incomplete diagnostics");
					break;
				}
				case Operation::Execute:
					Check(commands.Reset() == Result::Success && commands.Begin() == Result::Success,
						"Begin simulated recording");
					Check(graph.Execute(device, commands) == Result::Success, "Execute benchmark graph");
					Check(commands.End() == Result::Success, "End simulated recording");
					break;
				case Operation::Submit:
					Check(graph.Submit(device, completion) == Result::Success, "Submit benchmark graph");
					break;
			}
		};

		auto run = [&] {
			if (workload.reuseGraph) {
				operate(retained);
			}
			else {
				RenderGraph graph;
				BuildGraph(graph, inputs, passes, workload, callbacks);
				operate(graph);
			}
		};

		std::vector<double> samples;
		samples.reserve(options.samples);

		// Discard one full warmup sample. Every reported sample uses the same graph
		// shape, deterministic insertion order, and number of operations.
		for (uint32_t sample = 0; sample <= options.samples; ++sample) {
			const auto start = Clock::now();

			for (uint32_t iteration = 0; iteration < options.iterations; ++iteration) {
				run();
			}

			const double microseconds = std::chrono::duration<double, std::micro>(Clock::now() - start).count();
			if (sample != 0) {
				samples.push_back(microseconds / options.iterations);
			}
		}

		const bool records = workload.operation == Operation::Execute || workload.operation == Operation::Submit;
		const uint64_t expectedCallbacks = records ? uint64_t{passes} * options.iterations * (options.samples + 1) : 0;
		Check(callbacks == expectedCallbacks, "Benchmark skipped or duplicated callbacks");

		if (workload.operation == Operation::Submit) {
			Check(completion.IsComplete(device), "Simulated submissions did not complete");
		}

		if (workload.aliasing && passes > 1) {
			Check(retained.GetDiagnostics().aliasSavedBytes == uint64_t{passes - 1} * 64,
				"Alias workload did not reuse disjoint transient allocations");
		}

		std::sort(samples.begin(), samples.end());
		std::cout << workload.name << ',' << passes << ',' << s_ImportedResources + (workload.aliasing ? passes : 0)
				  << ',' << options.iterations << ',' << options.samples << ',' << samples[samples.size() / 2] << ','
				  << samples.front() << ',' << samples.back() << '\n';
	}
}

int main(int argc, char** argv) {
	Options options;
	if (!ParseOptions(argc, argv, options)) {
		std::cerr << "Usage: RenderGraphBenchmarks [--passes 1..4096] [--iterations 1..100000] [--samples 1..31]\n";
		return 1;
	}

	try {
		std::cerr << "CPU-only simulated backend; no GPU execution or driver timing. " << REALRHI_BENCHMARK_COMPILER
				  << ' ' << REALRHI_BENCHMARK_CONFIGURATION << '\n';
		std::cout << std::fixed << std::setprecision(3)
				  << "workload,passes,resources,iterations,samples,median_us,min_us,max_us\n";

		BenchmarkDevice device;
		Inputs inputs;

		const std::vector<uint32_t> passCounts =
			options.passes != 0 ? std::vector<uint32_t>{options.passes} : std::vector<uint32_t>{32, 128, 512};

		for (uint32_t passes : passCounts) {
			for (const auto workload : s_Workloads) {
				Measure(device, inputs, options, passes, workload);
			}
		}
	}
	catch (const std::exception& error) {
		std::cerr << "Render graph benchmark failed: " << error.what() << '\n';
		return 1;
	}

	return 0;
}
