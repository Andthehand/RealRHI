struct Parameters {
	uint bufferIndex;
	uint elementCount;
};
[[vk::push_constant]] ConstantBuffer<Parameters> parameters;

[require(spvDescriptorHeapEXT)]
[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 id : SV_DispatchThreadID) {
	if (id.x < parameters.elementCount) {
		RWStructuredBuffer<uint> values = RWStructuredBuffer<uint>.Handle(uint2(parameters.bufferIndex, 0));
		values[id.x] += 3;
	}
}
