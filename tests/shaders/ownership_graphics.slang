struct Parameters {
	uint bufferIndex;
	uint elementCount;
};
[[vk::push_constant]] ConstantBuffer<Parameters> parameters;

struct VertexInput {
	float2 position;
};

[shader("vertex")]
float4 vertexMain(VertexInput input) : SV_Position {
	return float4(input.position, 0, 1);
}

[require(spvDescriptorHeapEXT)]
[shader("fragment")]
float4 fragmentMain() : SV_Target {
	RWStructuredBuffer<uint> values = RWStructuredBuffer<uint>.Handle(uint2(parameters.bufferIndex, 0));
	return float4(values[0], values[1], values[2], values[3]) / 255.0;
}
