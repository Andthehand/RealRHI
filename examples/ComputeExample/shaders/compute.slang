struct ComputeParameters {
	uint outputIndex;
	uint multiplier;
	uint addend;
	uint elementCount;
};

[[vk::push_constant]]
ConstantBuffer<ComputeParameters> parameters;

[require(spvDescriptorHeapEXT)]
[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 threadId : SV_DispatchThreadID) {
	if (threadId.x >= parameters.elementCount) {
		return;
	}

	RWStructuredBuffer<uint> output = RWStructuredBuffer<uint>.Handle(uint2(parameters.outputIndex, 0));
	output[threadId.x] = parameters.multiplier * threadId.x + parameters.addend;
}
