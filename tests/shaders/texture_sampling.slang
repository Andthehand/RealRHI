struct Parameters {
	uint firstView;
	uint secondView;
	uint repeatSampler;
	uint borderSampler;
	uint outputIndex;
};
[[vk::push_constant]] ConstantBuffer<Parameters> parameters;

[require(spvDescriptorHeapEXT)]
[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain() {
	Texture2D<float4> first = Texture2D<float4>.Handle(uint2(parameters.firstView, 0));
	Texture2D<float4> second = Texture2D<float4>.Handle(uint2(parameters.secondView, 0));
	SamplerState repeatSampler = SamplerState.Handle(uint2(parameters.repeatSampler, 0));
	SamplerState borderSampler = SamplerState.Handle(uint2(parameters.borderSampler, 0));
	RWStructuredBuffer<uint> output = RWStructuredBuffer<uint>.Handle(uint2(parameters.outputIndex, 0));

	output[0] = uint(round(first.SampleLevel(repeatSampler, float2(0.5, 0.5), 1.0).r * 255.0));
	output[1] = uint(round(first.SampleLevel(borderSampler, float2(0.5, 0.5), 0.5).r * 255.0));
	output[2] = uint(round(second.SampleLevel(repeatSampler, float2(0.5, 0.5), 0.0).r * 255.0));
	output[3] = uint(round(first.SampleLevel(borderSampler, float2(-2.0, 0.5), 0.0).r * 255.0));
	output[4] = uint(round(first.SampleLevel(repeatSampler, float2(-2.0, 0.5), 0.0).r * 255.0));
}
