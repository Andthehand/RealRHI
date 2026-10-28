#include "../examples/ExampleSupport.h"
#include "../examples/SceneExample/SceneRenderer.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {
	using namespace RealRHI;
	using Clock = std::chrono::steady_clock;

	struct Options {
		uint32_t objects = 256;
		uint32_t frames = 300;
		uint32_t warmup = 60;
		uint32_t stream = 0;
		bool shared = false;
		bool validation = false;
		bool suite = false;
		bool customObjectCount = false;
		uint32_t repeats = 3;
		std::filesystem::path outputDirectory = "scene-resources";
		std::string csv = "scene-resources.csv";
	};

	bool ParseOptions(int argc, char** argv, Options& options) {
		options.suite = argc == 1;
		for (int i = 1; i < argc; ++i) {
			const std::string_view name(argv[i]);
			if (name == "--suite") {
				options.suite = true;
				continue;
			}
			if (name == "--shared") {
				options.shared = true;
				continue;
			}
			if (name == "--validation") {
				options.validation = true;
				continue;
			}
			if (++i == argc) {
				return false;
			}
			const std::string_view value(argv[i]);
			if (name == "--csv") {
				options.csv = value;
				continue;
			}
			if (name == "--output") {
				options.outputDirectory = value;
				continue;
			}
			uint32_t number = 0;
			const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
			if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
				return false;
			}
			if (name == "--objects") {
				options.objects = number;
				options.customObjectCount = true;
			}
			else if (name == "--frames") {
				options.frames = number;
			}
			else if (name == "--warmup") {
				options.warmup = number;
			}
			else if (name == "--stream") {
				options.stream = number;
			}
			else if (name == "--repeats") {
				options.repeats = number;
			}
			else {
				return false;
			}
		}
		return options.objects > 0 && options.objects <= 8192 && options.frames > 0 && options.frames <= 100000 &&
			options.warmup <= 100000 && options.stream <= options.objects && (!options.shared || options.stream <= 1) &&
			!options.csv.empty() && !options.outputDirectory.empty() && options.repeats > 0 && options.repeats <= 20 &&
			(!options.suite ||
				(!options.shared && !options.validation && options.stream == 0 &&
					options.csv == "scene-resources.csv"));
	}

	double Elapsed(Clock::time_point start) {
		return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
	}

	bool Check(Result result, const char* operation) {
		if (result != Result::Success) {
			std::cerr << "Scene resource benchmark: " << operation << " failed.\n";
			return false;
		}
		return true;
	}

	struct Sample {
		double frameCpu = 0;
		double streamCpu = 0;
		double recordCpu = 0;
		double gpuWait = 0;
		RenderGraphExecutionTimings graph;
		double submitCpu = 0;
		double validationCpu = 0;
		double nativeSubmitCpu = 0;
		uint64_t uploadedBytes = 0;
		DeviceStatistics memory;
		TransientResourcePoolStatistics pool;
	};

	class Benchmark {
	public:
		~Benchmark() {
			if (m_Device) {
				m_Device->WaitIdle();
			}
		}

		bool Run(const Options& options, std::vector<Sample>& outSamples, std::string& deviceName) {
			if (!Check(Device::Create({.applicationName = "Scene resource benchmark",
										  .shaderDirectory = REALRHI_SCENE_SHADER_DIRECTORY,
										  .enableDebug = true,
										  .enableProfiling = true,
										  .enableValidationLayers = options.validation,
										  .debugCallback = Examples::CaptureDebugMessage},
						   m_Device),
					"device creation")) {
				return false;
			}
			m_Renderer.ConfigureWorkload(options.objects, !options.shared);
			UploadSubmission initialUpload;
			if (!Check(
					m_Renderer.Init(*m_Device, TextureFormat::RGBA8_UNorm, 1, initialUpload), "scene initialization") ||
				!Check(m_Renderer.ResizeTargets(0, 960, 640), "target sizing") ||
				!Check(m_Device->CreateTexture({.width = 960,
												   .height = 640,
												   .format = TextureFormat::RGBA8_UNorm,
												   .usage = TextureUsage::RenderTarget | TextureUsage::TransferSrc},
						   m_Output),
					"output creation") ||
				!Check(m_Device->CreateCommandAllocator(QueueType::Graphics, m_Allocator), "allocator creation") ||
				!Check(m_Device->CreateCommandList(m_Allocator.Raw(), m_Commands), "command list creation")) {
				return false;
			}

			// Finish the initial ownership acquisition and mip generation before streaming can overwrite assets.
			if (!RecordAndSubmit(initialUpload, nullptr)) {
				return false;
			}
			m_Device->WaitIdle();
			std::vector<Sample> samples;
			samples.reserve(options.frames);
			for (uint32_t frame = 0; frame < options.warmup + options.frames; ++frame) {
				Sample sample;
				const auto before = m_Device->GetStatistics();
				const auto start = Clock::now();
				UploadSubmission upload;
				if (!Check(m_Renderer.StreamAssets(options.stream, frame, upload), "streaming upload")) {
					return false;
				}
				sample.streamCpu = Elapsed(start);
				if (!RecordAndSubmit({}, &sample)) {
					return false;
				}
				// RecordAndSubmit reports its wait separately; streaming uses the same native graphics queue.
				sample.frameCpu = Elapsed(start) - sample.gpuWait;
				const auto after = m_Device->GetStatistics();
				sample.submitCpu = after.submissions.totalMilliseconds - before.submissions.totalMilliseconds;
				sample.validationCpu =
					after.resourceStateValidation.totalMilliseconds - before.resourceStateValidation.totalMilliseconds;
				sample.nativeSubmitCpu =
					after.nativeSubmissions.totalMilliseconds - before.nativeSubmissions.totalMilliseconds;
				sample.uploadedBytes = after.submittedUploadBytes - before.submittedUploadBytes;
				sample.memory = after;
				sample.pool = m_Renderer.GetPoolStatistics();
				if (frame >= options.warmup) {
					samples.push_back(std::move(sample));
				}
			}
			if (!CheckOutput() || Examples::s_ErrorCount != 0) {
				return false;
			}
			if (!WriteResults(options, samples)) {
				return false;
			}
			deviceName = m_Device->GetCapabilities().name;
			outSamples = std::move(samples);
			return true;
		}

	private:
		bool RecordAndSubmit(const UploadSubmission& initialUpload, Sample* sample) {
			const auto recordStart = Clock::now();
			if (!Check(m_Commands->Reset(), "recording reset") || !Check(m_Commands->Begin(), "recording begin")) {
				return false;
			}
			BarrierDesc barriers;
			barriers.textures.push_back(
				{.texture = m_Output.Raw(),
					.before = {PipelineStage::ColorAttachmentOutput, ResourceAccess::ColorAttachmentWrite},
					.after = {PipelineStage::ColorAttachmentOutput, ResourceAccess::ColorAttachmentWrite},
					.oldLayout = TextureLayout::Undefined,
					.newLayout = TextureLayout::ColorAttachment});
			m_Commands->Barrier(barriers);
			RenderGraphExecutionTimings* timings = nullptr;
			if (sample) {
				timings = &sample->graph;
			}
			if (!Check(
					m_Renderer.Record(*m_Commands, 0, m_Output->GetTextureView(), 0.3f, initialUpload, true, timings),
					"scene recording") ||
				!Check(m_Commands->End(), "recording end")) {
				return false;
			}
			if (sample) {
				sample->recordCpu = Elapsed(recordStart);
			}
			std::span<const SubmissionToken> waits;
			if (initialUpload.completion.IsValid()) {
				waits = {&initialUpload.completion, 1};
			}
			auto* queue = m_Device->GetGraphicsCommandQueue();
			CommandList* lists[] = {m_Commands.Raw()};
			const auto token = queue->Submit({.commandLists = lists, .waitFor = waits});
			if (!token.IsValid()) {
				return false;
			}
			const auto waitStart = Clock::now();
			queue->Wait(token);
			if (sample) {
				sample->gpuWait = Elapsed(waitStart);
			}
			return true;
		}

		bool CheckOutput() {
			Ref<Buffer> readback;
			if (!Check(m_Device->CreateBuffer({.size = 960 * 640 * 4,
												  .usage = BufferUsage::TransferDst,
												  .memoryUsage = MemoryUsage::GPUToCPU},
						   readback),
					"readback creation") ||
				!Check(m_Commands->Reset(), "readback reset") || !Check(m_Commands->Begin(), "readback begin")) {
				return false;
			}
			BarrierDesc barriers;
			barriers.textures.push_back(
				{.texture = m_Output.Raw(),
					.before = {PipelineStage::ColorAttachmentOutput, ResourceAccess::ColorAttachmentWrite},
					.after = {PipelineStage::Transfer, ResourceAccess::TransferRead},
					.oldLayout = TextureLayout::ColorAttachment,
					.newLayout = TextureLayout::TransferSrc});
			m_Commands->Barrier(barriers);
			m_Commands->CopyTextureToBuffer(m_Output.Raw(), readback.Raw(), {.width = 960, .height = 640});
			if (!Check(m_Commands->End(), "readback end") ||
				!Check(m_Device->ImmediateSubmit(m_Commands.Raw()), "readback submission")) {
				return false;
			}
			std::vector<uint8_t> pixels(960 * 640 * 4);
			if (!Check(readback->ReadData(pixels.data(), pixels.size()), "pixel readback")) {
				return false;
			}
			uint64_t checksum = 14695981039346656037ull;
			uint32_t visible = 0;
			for (size_t i = 0; i < pixels.size(); i += 4) {
				if (pixels[i + 3] != 255) {
					std::cerr << "Scene output contains nonopaque pixels.\n";
					return false;
				}
				visible += std::max({pixels[i], pixels[i + 1], pixels[i + 2]}) > 30;
				for (size_t channel = 0; channel < 4; ++channel) {
					checksum = (checksum ^ pixels[i + channel]) * 1099511628211ull;
				}
			}
			std::cout << "Output checksum: " << checksum << "; visible pixels: " << visible << '\n';
			return visible > 100;
		}

		bool WriteResults(const Options& options, const std::vector<Sample>& samples) const {
			std::ofstream csv(options.csv);
			if (!csv) {
				std::cerr << "Cannot open benchmark CSV: " << options.csv << '\n';
				return false;
			}
			csv << "frame,mode,objects,asset_sets,streamed_assets,validation,frame_cpu_ms,stream_cpu_ms,record_total_"
				   "ms,graph_compile_ms,graph_allocate_ms,graph_record_ms,queue_submit_ms,state_validate_ms,native_"
				   "submit_ms,gpu_wait_ms,uploaded_bytes,buffer_count,buffer_live_bytes,buffer_peak_bytes,texture_"
				   "count,texture_live_bytes,texture_peak_bytes,descriptor_live_bytes,descriptor_reserved_bytes,pool_"
				   "resident_bytes,pool_peak_bytes,sampler_count,sampler_capacity\n";
			csv << std::fixed << std::setprecision(6);
			std::vector<double> frameTimes;
			for (size_t i = 0; i < samples.size(); ++i) {
				const auto& s = samples[i];
				uint32_t assets = options.objects;
				std::string_view mode = "unique";
				if (options.shared) {
					assets = 1;
					mode = "shared";
				}
				csv << i << ',' << mode << ',' << options.objects << ',' << assets << ',' << options.stream << ','
					<< options.validation << ',' << s.frameCpu << ',' << s.streamCpu << ',' << s.recordCpu << ','
					<< s.graph.compileMilliseconds << ',' << s.graph.allocationMilliseconds << ','
					<< s.graph.recordingMilliseconds << ',' << s.submitCpu << ',' << s.validationCpu << ','
					<< s.nativeSubmitCpu << ',' << s.gpuWait << ',' << s.uploadedBytes << ','
					<< s.memory.buffers.liveCount << ',' << s.memory.buffers.liveBytes << ','
					<< s.memory.buffers.peakBytes << ',' << s.memory.textures.liveCount << ','
					<< s.memory.textures.liveBytes << ',' << s.memory.textures.peakBytes << ','
					<< s.memory.descriptors.resourceBytes << ',' << s.memory.descriptors.resourceReservedBytes << ','
					<< s.pool.residentBytes << ',' << s.pool.peakResidentBytes << ','
					<< s.memory.descriptors.samplerCount << ',' << s.memory.descriptors.samplerCapacity << '\n';
				frameTimes.push_back(s.frameCpu);
			}
			csv.flush();
			std::sort(frameTimes.begin(), frameTimes.end());
			std::cout << "Device: " << m_Device->GetCapabilities().name << "\nMeasured frames: " << samples.size()
					  << "; warmup: " << options.warmup << "; CPU median: " << frameTimes[frameTimes.size() / 2]
					  << " ms\nCSV: " << options.csv << '\n';
			return csv.good();
		}

	private:
		std::unique_ptr<Device> m_Device;
		Examples::SceneRenderer m_Renderer;
		Ref<Texture> m_Output;
		Ref<CommandAllocator> m_Allocator;
		Ref<CommandList> m_Commands;
	};

	bool RunBenchmark(const Options& options, std::vector<Sample>& samples, std::string& deviceName) {
		bool success = false;
		{
			Benchmark benchmark;
			success = benchmark.Run(options, samples, deviceName);
		}
		return success && Examples::s_ErrorCount == 0;
	}

	double Median(std::vector<double> values) {
		std::sort(values.begin(), values.end());
		const size_t middle = values.size() / 2;
		if (values.size() % 2 == 0) {
			return (values[middle - 1] + values[middle]) * 0.5;
		}
		return values[middle];
	}

	bool WriteSummary(std::ostream& summary,
		const Options& options,
		uint32_t run,
		std::string_view mode,
		const std::vector<Sample>& samples) {
		std::vector<double> times;
		auto mean = [&](auto value) {
			double total = 0;
			for (const auto& sample : samples) {
				total += value(sample);
			}
			return total / samples.size();
		};
		for (const auto& sample : samples) {
			if (sample.uploadedBytes != uint64_t(options.stream) * 5120) {
				std::cerr << "Unexpected uploaded byte count in " << mode << " workload.\n";
				return false;
			}
			times.push_back(sample.frameCpu);
		}
		std::sort(times.begin(), times.end());
		const auto& last = samples.back();
		summary << mode << ',' << options.objects << ',' << run << ',' << samples.size() << ',' << Median(times) << ','
				<< times[static_cast<size_t>(std::ceil(times.size() * 0.95)) - 1] << ',' << mean([](const Sample& s) {
					   return s.frameCpu;
				   })
				<< ',' << mean([](const Sample& s) {
					   return s.streamCpu;
				   })
				<< ',' << mean([](const Sample& s) {
					   return s.recordCpu;
				   })
				<< ',' << mean([](const Sample& s) {
					   return s.graph.compileMilliseconds;
				   })
				<< ',' << mean([](const Sample& s) {
					   return s.graph.allocationMilliseconds;
				   })
				<< ',' << mean([](const Sample& s) {
					   return s.graph.recordingMilliseconds;
				   })
				<< ',' << mean([](const Sample& s) {
					   return s.submitCpu;
				   })
				<< ',' << mean([](const Sample& s) {
					   return s.validationCpu;
				   })
				<< ',' << mean([](const Sample& s) {
					   return s.nativeSubmitCpu;
				   })
				<< ',' << mean([](const Sample& s) {
					   return s.gpuWait;
				   })
				<< ',' << last.uploadedBytes << ',' << last.memory.buffers.liveCount << ','
				<< last.memory.textures.liveCount << ',' << last.memory.buffers.liveBytes << ','
				<< last.memory.textures.liveBytes << ',' << last.memory.buffers.peakBytes << ','
				<< last.memory.textures.peakBytes << ',' << last.memory.descriptors.resourceBytes << ','
				<< last.memory.descriptors.resourceReservedBytes << ',' << last.pool.residentBytes << ','
				<< last.pool.peakResidentBytes << ',' << last.memory.descriptors.samplerCount << ','
				<< last.memory.descriptors.samplerCapacity << '\n';
		summary.flush();
		return summary.good();
	}

	bool RunSuite(const Options& settings) {
		std::filesystem::create_directories(settings.outputDirectory);
		std::cout << "Suite reports: " << std::filesystem::absolute(settings.outputDirectory) << '\n';
		std::vector<uint32_t> counts{64, 256, 768};
		if (settings.customObjectCount) {
			counts = {settings.objects};
		}
		std::ofstream summary(settings.outputDirectory / "summary.csv");
		std::ofstream methodology(settings.outputDirectory / "methodology.txt");
		if (!summary || !methodology) {
			std::cerr << "Cannot open suite summary or methodology report.\n";
			return false;
		}
		methodology << "Status: incomplete (only Status: complete confirms the entire suite passed)\n"
					<< "Build: " << REALRHI_BENCHMARK_CONFIGURATION << "; compiler: " << REALRHI_BENCHMARK_COMPILER
					<< '\n'
					<< "Frames per run: " << settings.frames << "; warmup: " << settings.warmup
					<< "; repeats: " << settings.repeats << "\nObject counts:";
		for (auto count : counts) {
			methodology << ' ' << count;
		}
		methodology << "\n960x640 offscreen; shadows, profiling and GPU timestamps enabled.\n"
					<< "Separate validation probes run first. Timed workloads rotate shared/unique/streaming order per "
					   "repeat.\n"
					<< "One bootstrap frame precedes warmup; fixed rotation; streaming overwrites vertex buffers and "
					   "32x32 textures.\n"
					<< "Each frame waits for GPU completion; CPU frame time excludes the separately reported wait.\n"
					<< "record_total includes graph construction/destruction and compilation/allocation/recording.\n"
					<< "state_validate/native_submit are nested in queue_submit; stream_cpu includes upload "
					   "submission. Do not sum overlapping categories.\n"
					<< "Memory uses native VMA allocation sizes including descriptors/staging, not process RAM or all "
					   "driver allocations. Peaks include initialization.\n"
					<< "Pool bytes are included in buffer/texture totals. Serialized offscreen CPU times do not "
					   "establish FPS/GPU speedup.\n";
		methodology.flush();
		std::vector<Sample> samples;
		std::string deviceName;
		for (uint32_t mode = 0; mode < 4; ++mode) {
			Options validation = settings;
			validation.validation = true;
			validation.shared = mode == 0;
			validation.objects = 16;
			validation.frames = 3;
			validation.warmup = 1;
			validation.stream = 0;
			if (mode >= 2) {
				validation.stream = 4;
			}
			if (mode == 3) {
				validation.objects = counts.back();
				validation.stream = std::min(16u, validation.objects);
			}
			validation.csv = (settings.outputDirectory / ("validation-" + std::to_string(mode) + ".csv")).string();
			if (!RunBenchmark(validation, samples, deviceName)) {
				return false;
			}
		}
		methodology << "Device: " << deviceName << '\n';
		summary << "mode,objects,run,frames,cpu_median_ms,cpu_p95_ms,cpu_mean_ms,stream_mean_ms,record_total_mean_ms,"
				   "compile_mean_ms,allocate_mean_ms,record_mean_ms,submit_mean_ms,state_validate_mean_ms,native_"
				   "submit_mean_ms,"
				   "gpu_wait_mean_ms,uploaded_bytes_per_frame,buffer_count,texture_count,buffer_live_bytes,texture_"
				   "live_bytes,"
				   "buffer_peak_bytes,texture_peak_bytes,descriptor_live_bytes,descriptor_reserved_bytes,pool_resident_"
				   "bytes,"
				   "pool_peak_bytes,sampler_count,sampler_capacity\n"
				<< std::fixed << std::setprecision(6);
		const std::string_view modes[] = {"shared", "unique", "stream"};
		for (auto count : counts) {
			for (uint32_t run = 0; run < settings.repeats; ++run) {
				for (uint32_t index = 0; index < 3; ++index) {
					const auto mode = modes[(index + run) % 3];
					Options options = settings;
					options.objects = count;
					options.shared = mode == "shared";
					options.stream = 0;
					if (mode == "stream") {
						options.stream = std::min(16u, count);
					}
					const auto name =
						std::string(mode) + "-" + std::to_string(count) + "-run" + std::to_string(run + 1);
					options.csv = (settings.outputDirectory / (name + ".csv")).string();
					std::cout << "Running " << name << '\n';
					if (!RunBenchmark(options, samples, deviceName) ||
						!WriteSummary(summary, options, run + 1, mode, samples)) {
						return false;
					}
				}
			}
		}
		methodology << "Validation, final image readback and cleanup checks passed for every run.\nStatus: complete\n";
		methodology.flush();
		std::cout << "Suite complete. Summary: " << settings.outputDirectory / "summary.csv" << '\n';
		return methodology.good() && summary.good();
	}
}

int main(int argc, char** argv) {
	Options options;
	if (!ParseOptions(argc, argv, options)) {
		std::cerr
			<< "Usage: SceneResourceBenchmarks [--objects 1..8192] [--frames N] [--warmup N] "
			   "[--stream N] [--shared] [--validation] [--csv path]\n"
			   "       SceneResourceBenchmarks --suite [--objects N] [--frames N] [--warmup N] [--repeats 1..20] "
			   "[--output directory]\n"
			   "No arguments runs the full suite. Suite mode streams 16 assets per frame (or all assets if fewer).\n"
			   "--stream counts distinct assets per frame; shared mode permits at most one.\n";
		return 1;
	}
	try {
		if (options.suite) {
			return RunSuite(options) ? 0 : 1;
		}
		std::vector<Sample> samples;
		std::string deviceName;
		return RunBenchmark(options, samples, deviceName) ? 0 : 1;
	}
	catch (const std::exception& error) {
		std::cerr << "Scene benchmark failed: " << error.what() << '\n';
		return 1;
	}
}
