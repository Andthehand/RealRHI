struct Parameters {
	float4 color;
	uint textureIndex;
	uint sampleTexture;
};

[[vk::push_constant]]
ConstantBuffer<Parameters> parameters;

[shader("vertex")]
float4 vertexMain(uint vertex : SV_VulkanVertexID) : SV_Position {
	const float2 positions[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
	return float4(positions[vertex], 0, 1);
}

[require(spvDescriptorHeapEXT)]
[shader("fragment")]
float4 fragmentMain() : SV_Target0 {
	if (parameters.sampleTexture != 0) {
		Texture2D<float4> source = Texture2D<float4>.Handle(uint2(parameters.textureIndex, 0));
		return source.Load(int3(0, 0, 0));
	}
	return parameters.color;
}
